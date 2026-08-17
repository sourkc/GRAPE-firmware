#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "png.h"

#include "grape_internal.h"
#include "grape/grape_screenshot.h"

#define GRAPE_SCREENSHOT_PNG_EXTRA_BYTES (64U * 1024U)
#define GRAPE_SCREENSHOT_JPEG_BYTES_PER_PIXEL_LIMIT 6U
#define GRAPE_SCREENSHOT_JPEG_EXTRA_BYTES (64U * 1024U)

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
} grape_png_sink_t;

static bool checked_mul_size(size_t a, size_t b, size_t *out)
{
    if (!out || (a != 0U && b > SIZE_MAX / a)) {
        return false;
    }
    *out = a * b;
    return true;
}

static void *screenshot_alloc(size_t size)
{
    if (size == 0U) {
        return NULL;
    }

    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static esp_err_t screenshot_frame_layout(const grape_display_info_t *info,
                                         size_t *out_bpp,
                                         size_t *out_row_bytes,
                                         size_t *out_frame_size)
{
    if (!info || !out_bpp || !out_row_bytes || !out_frame_size ||
        info->width == 0U || info->height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bpp;
    switch (info->format) {
        case GRAPE_PIXEL_FORMAT_RGB565:
            bpp = 2U;
            break;
        case GRAPE_PIXEL_FORMAT_RGB888:
            bpp = 3U;
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (info->width > SIZE_MAX / bpp) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t row_bytes = (size_t)info->width * bpp;
    if (info->height > SIZE_MAX / row_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_bpp = bpp;
    *out_row_bytes = row_bytes;
    *out_frame_size = row_bytes * info->height;
    return ESP_OK;
}

static esp_err_t screenshot_validate_desc(const grape_screenshot_desc_t *desc)
{
    if (!desc) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (desc->format) {
        case GRAPE_SCREENSHOT_FORMAT_PNG:
            if (desc->png_compression_level < 0 || desc->png_compression_level > 9) {
                return ESP_ERR_INVALID_ARG;
            }
            return ESP_OK;

        case GRAPE_SCREENSHOT_FORMAT_JPEG:
            if (desc->jpeg_quality < 1U || desc->jpeg_quality > 100U ||
                desc->jpeg_subsampling < GRAPE_SCREENSHOT_JPEG_YUV444 ||
                desc->jpeg_subsampling > GRAPE_SCREENSHOT_JPEG_YUV420) {
                return ESP_ERR_INVALID_ARG;
            }
            return ESP_OK;

        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static void png_write_to_sink(png_structp png_ptr, png_bytep data, png_size_t length)
{
    grape_png_sink_t *sink = (grape_png_sink_t *)png_get_io_ptr(png_ptr);
    if (!sink || !data) {
        png_error(png_ptr, "invalid GRAPE PNG sink");
        return;
    }

    if ((size_t)length > sink->capacity - sink->size) {
        png_error(png_ptr, "GRAPE PNG output buffer exhausted");
        return;
    }

    memcpy(sink->data + sink->size, data, (size_t)length);
    sink->size += (size_t)length;
}

static void png_flush_sink(png_structp png_ptr)
{
    (void)png_ptr;
}

static void expand_rgb565_row_to_rgb888(const uint8_t *src,
                                        uint8_t *dst,
                                        uint32_t width)
{
    for (uint32_t x = 0U; x < width; ++x) {
        uint16_t pixel = (uint16_t)src[(size_t)x * 2U] |
                         ((uint16_t)src[(size_t)x * 2U + 1U] << 8U);
        uint8_t r5 = (uint8_t)((pixel >> 11U) & 0x1FU);
        uint8_t g6 = (uint8_t)((pixel >> 5U) & 0x3FU);
        uint8_t b5 = (uint8_t)(pixel & 0x1FU);

        dst[(size_t)x * 3U] = (uint8_t)((r5 << 3U) | (r5 >> 2U));
        dst[(size_t)x * 3U + 1U] = (uint8_t)((g6 << 2U) | (g6 >> 4U));
        dst[(size_t)x * 3U + 2U] = (uint8_t)((b5 << 3U) | (b5 >> 2U));
    }
}

static esp_err_t capture_png(grape_context_t *context,
                             const grape_screenshot_desc_t *desc,
                             grape_screenshot_t *out_screenshot)
{
    const grape_display_info_t *info = &context->display_info;
    size_t bpp = 0U;
    size_t row_bytes = 0U;
    size_t frame_size = 0U;
    esp_err_t ret = screenshot_frame_layout(info, &bpp, &row_bytes, &frame_size);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *frame = screenshot_alloc(frame_size);
    if (!frame) {
        return ESP_ERR_NO_MEM;
    }

    ret = grape_display_copy_presented_frame(context->display, frame, frame_size);
    if (ret != ESP_OK) {
        free(frame);
        return ret;
    }

    size_t png_row_size = 0U;
    size_t raw_rgb888_size = 0U;
    if (!checked_mul_size((size_t)info->width, 3U, &png_row_size) ||
        !checked_mul_size(png_row_size, (size_t)info->height, &raw_rgb888_size)) {
        free(frame);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t slack = raw_rgb888_size / 16U;
    if (raw_rgb888_size > SIZE_MAX - slack ||
        raw_rgb888_size + slack > SIZE_MAX - GRAPE_SCREENSHOT_PNG_EXTRA_BYTES) {
        free(frame);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t output_capacity = raw_rgb888_size + slack + GRAPE_SCREENSHOT_PNG_EXTRA_BYTES;

    uint8_t *output = screenshot_alloc(output_capacity);
    if (!output) {
        free(frame);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *converted_row = NULL;
    if (info->format == GRAPE_PIXEL_FORMAT_RGB565) {
        converted_row = heap_caps_malloc(png_row_size, MALLOC_CAP_8BIT);
        if (!converted_row) {
            converted_row = malloc(png_row_size);
        }
        if (!converted_row) {
            free(output);
            free(frame);
            return ESP_ERR_NO_MEM;
        }
    }

    grape_png_sink_t sink = {
        .data = output,
        .size = 0U,
        .capacity = output_capacity,
    };

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        free(converted_row);
        free(output);
        free(frame);
        return ESP_ERR_NO_MEM;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, NULL);
        free(converted_row);
        free(output);
        free(frame);
        return ESP_ERR_NO_MEM;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        ret = ESP_FAIL;
        png_destroy_write_struct(&png_ptr, &info_ptr);
        free(converted_row);
        free(output);
        free(frame);
        return ret;
    }

    png_set_write_fn(png_ptr, &sink, png_write_to_sink, png_flush_sink);
    png_set_compression_level(png_ptr, desc->png_compression_level);
    png_set_IHDR(png_ptr,
                 info_ptr,
                 info->width,
                 info->height,
                 8,
                 PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png_ptr, info_ptr);

    for (uint32_t y = 0U; y < info->height; ++y) {
        const uint8_t *src_row = frame + (size_t)y * row_bytes;
        if (info->format == GRAPE_PIXEL_FORMAT_RGB565) {
            expand_rgb565_row_to_rgb888(src_row, converted_row, info->width);
            png_write_row(png_ptr, converted_row);
        } else {
            png_write_row(png_ptr, (png_const_bytep)src_row);
        }
    }

    png_write_end(png_ptr, info_ptr);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    free(converted_row);
    free(frame);

    out_screenshot->data = output;
    out_screenshot->size = sink.size;
    out_screenshot->width = info->width;
    out_screenshot->height = info->height;
    out_screenshot->format = GRAPE_SCREENSHOT_FORMAT_PNG;
    return ESP_OK;
}

static jpeg_down_sampling_type_t jpeg_subsampling_to_idf(
    grape_screenshot_jpeg_subsampling_t subsampling)
{
    switch (subsampling) {
        case GRAPE_SCREENSHOT_JPEG_YUV422:
            return JPEG_DOWN_SAMPLING_YUV422;
        case GRAPE_SCREENSHOT_JPEG_YUV420:
            return JPEG_DOWN_SAMPLING_YUV420;
        case GRAPE_SCREENSHOT_JPEG_YUV444:
        default:
            return JPEG_DOWN_SAMPLING_YUV444;
    }
}

static void rgb888_to_jpeg_bgr888(uint8_t *frame, size_t pixel_count)
{
    /*
     * The ESP JPEG encoder's RGB888 input path consumes BGR24-style bytes.
     * GRAPE's RGB888 framebuffer is R,G,B, so swap R/B once in the aligned
     * JPEG input copy. RGB565 does not need this conversion.
     */
    for (size_t i = 0U; i < pixel_count; ++i) {
        uint8_t *pixel = frame + i * 3U;
        uint8_t r = pixel[0];
        pixel[0] = pixel[2];
        pixel[2] = r;
    }
}

static esp_err_t capture_jpeg(grape_context_t *context,
                              const grape_screenshot_desc_t *desc,
                              grape_screenshot_t *out_screenshot)
{
    const grape_display_info_t *info = &context->display_info;
    size_t bpp = 0U;
    size_t row_bytes = 0U;
    size_t frame_size = 0U;
    esp_err_t ret = screenshot_frame_layout(info, &bpp, &row_bytes, &frame_size);
    if (ret != ESP_OK) {
        return ret;
    }
    (void)bpp;
    (void)row_bytes;

    if (frame_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    jpeg_encode_memory_alloc_cfg_t input_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
    };
    size_t input_allocated_size = 0U;
    uint8_t *input = jpeg_alloc_encoder_mem(frame_size,
                                            &input_mem_cfg,
                                            &input_allocated_size);
    if (!input || input_allocated_size < frame_size) {
        free(input);
        return ESP_ERR_NO_MEM;
    }

    ret = grape_display_copy_presented_frame(context->display, input, frame_size);
    if (ret != ESP_OK) {
        free(input);
        return ret;
    }

    if (info->format == GRAPE_PIXEL_FORMAT_RGB888) {
        size_t rgb888_pixel_count = 0U;
        if (!checked_mul_size((size_t)info->width, (size_t)info->height,
                              &rgb888_pixel_count)) {
            free(input);
            return ESP_ERR_INVALID_SIZE;
        }
        rgb888_to_jpeg_bgr888(input, rgb888_pixel_count);
    }

    size_t pixel_count = 0U;
    if (!checked_mul_size((size_t)info->width, (size_t)info->height, &pixel_count)) {
        free(input);
        return ESP_ERR_INVALID_SIZE;
    }
    if (pixel_count > (SIZE_MAX - GRAPE_SCREENSHOT_JPEG_EXTRA_BYTES) /
                      GRAPE_SCREENSHOT_JPEG_BYTES_PER_PIXEL_LIMIT) {
        free(input);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t output_request = pixel_count * GRAPE_SCREENSHOT_JPEG_BYTES_PER_PIXEL_LIMIT +
                            GRAPE_SCREENSHOT_JPEG_EXTRA_BYTES;
    if (output_request > UINT32_MAX) {
        free(input);
        return ESP_ERR_INVALID_SIZE;
    }

    jpeg_encode_memory_alloc_cfg_t output_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };
    size_t output_allocated_size = 0U;
    uint8_t *output = jpeg_alloc_encoder_mem(output_request,
                                             &output_mem_cfg,
                                             &output_allocated_size);
    if (!output || output_allocated_size < output_request || output_allocated_size > UINT32_MAX) {
        free(output);
        free(input);
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 1000,
    };
    jpeg_encoder_handle_t encoder = NULL;
    ret = jpeg_new_encoder_engine(&engine_cfg, &encoder);
    if (ret != ESP_OK) {
        free(output);
        free(input);
        return ret;
    }

    jpeg_encode_cfg_t encode_cfg = {
        .src_type = info->format == GRAPE_PIXEL_FORMAT_RGB565
            ? JPEG_ENCODE_IN_FORMAT_RGB565
            : JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample = jpeg_subsampling_to_idf(desc->jpeg_subsampling),
        .image_quality = desc->jpeg_quality,
        .width = info->width,
        .height = info->height,
        .pixel_reverse = false,
    };

    uint32_t encoded_size = 0U;
    ret = jpeg_encoder_process(encoder,
                               &encode_cfg,
                               input,
                               (uint32_t)frame_size,
                               output,
                               (uint32_t)output_allocated_size,
                               &encoded_size);

    esp_err_t delete_ret = jpeg_del_encoder_engine(encoder);
    free(input);

    if (ret != ESP_OK) {
        free(output);
        return ret;
    }
    if (delete_ret != ESP_OK) {
        free(output);
        return delete_ret;
    }
    if (encoded_size == 0U || encoded_size > output_allocated_size) {
        free(output);
        return ESP_ERR_INVALID_SIZE;
    }

    out_screenshot->data = output;
    out_screenshot->size = encoded_size;
    out_screenshot->width = info->width;
    out_screenshot->height = info->height;
    out_screenshot->format = GRAPE_SCREENSHOT_FORMAT_JPEG;
    return ESP_OK;
}

esp_err_t grape_screenshot_capture(grape_context_t *context,
                                   const grape_screenshot_desc_t *desc,
                                   grape_screenshot_t *out_screenshot)
{
    if (!context || !desc || !out_screenshot) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_screenshot = (grape_screenshot_t){0};

    esp_err_t ret = screenshot_validate_desc(desc);
    if (ret != ESP_OK) {
        return ret;
    }

    switch (desc->format) {
        case GRAPE_SCREENSHOT_FORMAT_PNG:
            return capture_png(context, desc, out_screenshot);
        case GRAPE_SCREENSHOT_FORMAT_JPEG:
            return capture_jpeg(context, desc, out_screenshot);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t grape_screenshot_save(grape_context_t *context,
                                const grape_screenshot_desc_t *desc,
                                const char *path)
{
    if (!context || !desc || !path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    grape_screenshot_t screenshot = {0};
    esp_err_t ret = grape_screenshot_capture(context, desc, &screenshot);
    if (ret != ESP_OK) {
        return ret;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        grape_screenshot_release(&screenshot);
        return ESP_FAIL;
    }

    size_t written = fwrite(screenshot.data, 1U, screenshot.size, file);
    int close_result = fclose(file);
    if (written != screenshot.size || close_result != 0) {
        ret = ESP_FAIL;
    }

    grape_screenshot_release(&screenshot);
    return ret;
}

void grape_screenshot_release(grape_screenshot_t *screenshot)
{
    if (!screenshot) {
        return;
    }

    free(screenshot->data);
    *screenshot = (grape_screenshot_t){0};
}
