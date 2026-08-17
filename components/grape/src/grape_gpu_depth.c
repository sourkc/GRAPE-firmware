#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "grape_gpu_internal.h"
#include "grape_internal.h"

static uint32_t gpu_depth_caps(grape_memory_t memory)
{
    grape_memory_t resolved = memory;
    if (resolved == GRAPE_MEMORY_DEFAULT) {
#if CONFIG_GRAPE_TEXTURE_DEFAULT_PSRAM
        resolved = GRAPE_MEMORY_PSRAM;
#else
        resolved = GRAPE_MEMORY_INTERNAL;
#endif
    }

    if (resolved == GRAPE_MEMORY_PSRAM) {
        return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }

    return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
}

static uint16_t gpu_depth_to_d16(float depth)
{
    if (depth <= 0.0f) {
        return 0U;
    }
    if (depth >= 1.0f) {
        return UINT16_MAX;
    }
    return (uint16_t)lroundf(depth * 65535.0f);
}

static void gpu_depth_fill_local(uint16_t *dst,
                                 uint32_t stride_values,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t samples,
                                 uint16_t value)
{
    const size_t values_per_row = (size_t)width * samples;
    for (uint32_t y = 0U; y < height; ++y) {
        uint16_t *row = dst + (size_t)y * stride_values;
        if (value == 0U) {
            memset(row, 0, values_per_row * sizeof(*row));
        } else if (value == UINT16_MAX) {
            memset(row, 0xFF, values_per_row * sizeof(*row));
        } else {
            for (size_t i = 0U; i < values_per_row; ++i) {
                row[i] = value;
            }
        }
    }
}

esp_err_t grape_gpu_depth_begin_pass(grape_gpu_depth_buffer_t *buffer,
                                     grape_gpu_load_op_t load_op,
                                     float clear_depth)
{
    GRAPE_TIME_SCOPE(GPU_DEPTH_CLEAR);

    if (!buffer || load_op < GRAPE_GPU_LOAD_OP_LOAD || load_op > GRAPE_GPU_LOAD_OP_CLEAR) {
        return ESP_ERR_INVALID_ARG;
    }

    if (load_op == GRAPE_GPU_LOAD_OP_LOAD) {
        return ESP_OK;
    }
    if (!isfinite(clear_depth) || clear_depth < 0.0f || clear_depth > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer->lazy_clear_value = gpu_depth_to_d16(clear_depth);
    buffer->lazy_clear_active = true;
    memset(buffer->lazy_tiles, 0, buffer->tile_count);
    return ESP_OK;
}

void grape_gpu_depth_load_tile(const grape_gpu_depth_buffer_t *buffer,
                               uint32_t tile_x,
                               uint32_t tile_y,
                               uint16_t *dst,
                               uint32_t dst_stride_values)
{
    if (!buffer || !dst || tile_x >= buffer->tile_cols || tile_y >= buffer->tile_rows) {
        return;
    }

    GRAPE_TIME_SCOPE(GPU_DEPTH_TILE_INIT);

    const uint32_t x0 = tile_x * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t y0 = tile_y * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t width = x0 + GRAPE_GPU_MSAA_TILE_SIZE <= buffer->width
        ? GRAPE_GPU_MSAA_TILE_SIZE
        : buffer->width - x0;
    const uint32_t height = y0 + GRAPE_GPU_MSAA_TILE_SIZE <= buffer->height
        ? GRAPE_GPU_MSAA_TILE_SIZE
        : buffer->height - y0;
    const uint32_t samples = (uint32_t)buffer->sample_count;
    const size_t tile_index = (size_t)tile_y * buffer->tile_cols + tile_x;

    if (buffer->lazy_clear_active && !buffer->lazy_tiles[tile_index]) {
        gpu_depth_fill_local(dst, dst_stride_values, width, height, samples,
                             buffer->lazy_clear_value);
        return;
    }

    const size_t values_per_row = (size_t)width * samples;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint16_t *src = (const uint16_t *)(
            (const uint8_t *)buffer->data + (size_t)(y0 + y) * buffer->stride
        ) + (size_t)x0 * samples;
        memcpy(dst + (size_t)y * dst_stride_values,
               src,
               values_per_row * sizeof(*src));
    }
}

void grape_gpu_depth_store_tile(grape_gpu_depth_buffer_t *buffer,
                                uint32_t tile_x,
                                uint32_t tile_y,
                                const uint16_t *src,
                                uint32_t src_stride_values)
{
    if (!buffer || !src || tile_x >= buffer->tile_cols || tile_y >= buffer->tile_rows) {
        return;
    }

    GRAPE_TIME_SCOPE(GPU_DEPTH_TILE_STORE);

    const uint32_t x0 = tile_x * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t y0 = tile_y * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t width = x0 + GRAPE_GPU_MSAA_TILE_SIZE <= buffer->width
        ? GRAPE_GPU_MSAA_TILE_SIZE
        : buffer->width - x0;
    const uint32_t height = y0 + GRAPE_GPU_MSAA_TILE_SIZE <= buffer->height
        ? GRAPE_GPU_MSAA_TILE_SIZE
        : buffer->height - y0;
    const uint32_t samples = (uint32_t)buffer->sample_count;
    const size_t values_per_row = (size_t)width * samples;

    for (uint32_t y = 0U; y < height; ++y) {
        uint16_t *dst = (uint16_t *)(
            (uint8_t *)buffer->data + (size_t)(y0 + y) * buffer->stride
        ) + (size_t)x0 * samples;
        memcpy(dst,
               src + (size_t)y * src_stride_values,
               values_per_row * sizeof(*dst));
    }

    buffer->lazy_tiles[(size_t)tile_y * buffer->tile_cols + tile_x] = 1U;
}

esp_err_t grape_gpu_depth_buffer_create(grape_gpu_context_t *context,
                                        const grape_gpu_depth_buffer_desc_t *desc,
                                        grape_gpu_depth_buffer_t **out_buffer)
{
    if (!context || !desc || !out_buffer || desc->width == 0U || desc->height == 0U ||
        desc->format != GRAPE_GPU_DEPTH_D16 ||
        !grape_gpu_sample_count_valid(desc->sample_count) ||
        desc->memory < GRAPE_MEMORY_DEFAULT || desc->memory > GRAPE_MEMORY_PSRAM) {
        return ESP_ERR_INVALID_ARG;
    }

    const grape_gpu_sample_count_t sample_count = grape_gpu_sample_count_resolve(desc->sample_count);
    size_t samples_per_row = (size_t)desc->width * (size_t)sample_count;
    if (samples_per_row / (size_t)sample_count != desc->width) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t stride = samples_per_row * sizeof(uint16_t);
    if (stride / sizeof(uint16_t) != samples_per_row) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t size = stride * (size_t)desc->height;
    if (size / stride != desc->height) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t tile_cols = (desc->width + GRAPE_GPU_MSAA_TILE_SIZE - 1U) / GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t tile_rows = (desc->height + GRAPE_GPU_MSAA_TILE_SIZE - 1U) / GRAPE_GPU_MSAA_TILE_SIZE;
    const size_t tile_count = (size_t)tile_cols * tile_rows;
    if (tile_cols != 0U && tile_count / tile_cols != tile_rows) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_gpu_depth_buffer_t *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    buffer->data = heap_caps_malloc(size, gpu_depth_caps(desc->memory));
    if (!buffer->data) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    buffer->lazy_tiles = calloc(tile_count, 1U);
    if (!buffer->lazy_tiles) {
        heap_caps_free(buffer->data);
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    buffer->context = context;
    buffer->stride = stride;
    buffer->size = size;
    buffer->width = desc->width;
    buffer->height = desc->height;
    buffer->format = desc->format;
    buffer->sample_count = sample_count;
    buffer->memory = desc->memory;
    buffer->tile_cols = tile_cols;
    buffer->tile_rows = tile_rows;
    buffer->tile_count = tile_count;
    buffer->next = context->depth_buffers;
    context->depth_buffers = buffer;

    *out_buffer = buffer;
    return ESP_OK;
}

esp_err_t grape_gpu_depth_buffer_destroy(grape_gpu_depth_buffer_t *buffer)
{
    if (!buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_gpu_context_t *context = buffer->context;
    if (context->render_pass_active && context->depth_attachment == buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_gpu_depth_buffer_t **cursor = &context->depth_buffers;
    while (*cursor && *cursor != buffer) {
        cursor = &(*cursor)->next;
    }
    if (*cursor != buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    *cursor = buffer->next;
    if (context->depth_attachment == buffer) {
        context->depth_attachment = NULL;
    }
    free(buffer->lazy_tiles);
    heap_caps_free(buffer->data);
    free(buffer);
    return ESP_OK;
}

uint32_t grape_gpu_depth_buffer_width(const grape_gpu_depth_buffer_t *buffer)
{
    return buffer ? buffer->width : 0U;
}

uint32_t grape_gpu_depth_buffer_height(const grape_gpu_depth_buffer_t *buffer)
{
    return buffer ? buffer->height : 0U;
}

grape_gpu_sample_count_t grape_gpu_depth_buffer_sample_count(const grape_gpu_depth_buffer_t *buffer)
{
    return buffer ? buffer->sample_count : GRAPE_GPU_SAMPLE_COUNT_1;
}
