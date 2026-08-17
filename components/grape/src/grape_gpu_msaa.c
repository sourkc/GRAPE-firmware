#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "grape_gpu_internal.h"
#include "grape_internal.h"

static esp_err_t gpu_msaa_required_size(const grape_gpu_context_t *context, size_t *out_size)
{
    const size_t width = context->color_attachment->width;
    const size_t height = context->color_attachment->height;
    const size_t samples = (size_t)context->sample_count;

    if (width > SIZE_MAX / height) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t pixels = width * height;
    if (pixels > SIZE_MAX / samples) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t sample_pixels = pixels * samples;
    if (sample_pixels > SIZE_MAX / 4U) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_size = sample_pixels * 4U;
    return ESP_OK;
}

static esp_err_t gpu_msaa_ensure_storage(grape_gpu_context_t *context)
{
    size_t required = 0U;
    esp_err_t ret = gpu_msaa_required_size(context, &required);
    if (ret != ESP_OK) {
        return ret;
    }

    if (context->msaa_color && context->msaa_color_size >= required) {
        return ESP_OK;
    }

    uint8_t *replacement = heap_caps_malloc(required, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!replacement) {
        replacement = heap_caps_malloc(required, MALLOC_CAP_8BIT);
    }
    if (!replacement) {
        return ESP_ERR_NO_MEM;
    }

    heap_caps_free(context->msaa_color);
    context->msaa_color = replacement;
    context->msaa_color_size = required;
    return ESP_OK;
}

esp_err_t grape_gpu_msaa_begin(grape_gpu_context_t *context,
                               grape_gpu_load_op_t load_op,
                               grape_color_t clear_color)
{
    if (!context || !context->color_attachment ||
        context->sample_count == GRAPE_GPU_SAMPLE_COUNT_1) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = gpu_msaa_ensure_storage(context);
    if (ret != ESP_OK) {
        return ret;
    }

    const uint32_t width = context->color_attachment->width;
    const uint32_t height = context->color_attachment->height;
    const uint32_t samples = (uint32_t)context->sample_count;
    uint8_t *dst = context->msaa_color;

    if (load_op == GRAPE_GPU_LOAD_OP_CLEAR) {
        const uint8_t rgba[4] = { clear_color.r, clear_color.g, clear_color.b, clear_color.a };
        const size_t sample_pixels = (size_t)width * height * samples;
        for (size_t i = 0U; i < sample_pixels; ++i) {
            memcpy(dst + i * 4U, rgba, sizeof(rgba));
        }
        return ESP_OK;
    }

    grape_texture_t *source = context->color_attachment;
    for (uint32_t y = 0U; y < height; ++y) {
        const uint8_t *src_row = source->pixels + (size_t)y * source->stride;
        for (uint32_t x = 0U; x < width; ++x) {
            const uint8_t *pixel = src_row + (size_t)x * 4U;
            uint8_t *sample_base = dst + (((size_t)y * width + x) * samples) * 4U;
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                memcpy(sample_base + (size_t)sample * 4U, pixel, 4U);
            }
        }
    }

    return ESP_OK;
}

esp_err_t grape_gpu_msaa_resolve(grape_gpu_context_t *context)
{
    if (!context || !context->color_attachment || !context->msaa_color ||
        context->sample_count == GRAPE_GPU_SAMPLE_COUNT_1) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!context->dirty_valid) {
        return ESP_OK;
    }

    grape_texture_t *target = context->color_attachment;
    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t width = target->width;
    const grape_rect_t dirty = context->dirty_rect;
    const int32_t end_x = dirty.x + dirty.width;
    const int32_t end_y = dirty.y + dirty.height;

    for (int32_t y = dirty.y; y < end_y; ++y) {
        uint8_t *dst_row = target->pixels + (size_t)y * target->stride;
        for (int32_t x = dirty.x; x < end_x; ++x) {
            const uint8_t *sample_base = context->msaa_color +
                (((size_t)y * width + (uint32_t)x) * samples) * 4U;

            uint32_t alpha_sum = 0U;
            uint32_t premul_r_sum = 0U;
            uint32_t premul_g_sum = 0U;
            uint32_t premul_b_sum = 0U;
            uint32_t opaque_r_sum = 0U;
            uint32_t opaque_g_sum = 0U;
            uint32_t opaque_b_sum = 0U;
            uint32_t opaque_samples = 0U;
            bool binary_alpha = true;
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                const uint8_t *rgba = sample_base + (size_t)sample * 4U;
                const uint32_t alpha = rgba[3];
                alpha_sum += alpha;
                premul_r_sum += (uint32_t)rgba[0] * alpha;
                premul_g_sum += (uint32_t)rgba[1] * alpha;
                premul_b_sum += (uint32_t)rgba[2] * alpha;
                if (alpha == 255U) {
                    ++opaque_samples;
                    opaque_r_sum += rgba[0];
                    opaque_g_sum += rgba[1];
                    opaque_b_sum += rgba[2];
                } else if (alpha != 0U) {
                    binary_alpha = false;
                }
            }

            uint8_t *dst = dst_row + (size_t)x * 4U;
            if (alpha_sum == 0U) {
                dst[0] = 0U;
                dst[1] = 0U;
                dst[2] = 0U;
                dst[3] = 0U;
                continue;
            }

            if (binary_alpha) {
                dst[0] = (uint8_t)((opaque_r_sum + opaque_samples / 2U) / opaque_samples);
                dst[1] = (uint8_t)((opaque_g_sum + opaque_samples / 2U) / opaque_samples);
                dst[2] = (uint8_t)((opaque_b_sum + opaque_samples / 2U) / opaque_samples);
                dst[3] = (uint8_t)((opaque_samples * 255U + samples / 2U) / samples);
                continue;
            }

            dst[0] = (uint8_t)((premul_r_sum + alpha_sum / 2U) / alpha_sum);
            dst[1] = (uint8_t)((premul_g_sum + alpha_sum / 2U) / alpha_sum);
            dst[2] = (uint8_t)((premul_b_sum + alpha_sum / 2U) / alpha_sum);
            dst[3] = (uint8_t)((alpha_sum + samples / 2U) / samples);
        }
    }

    return ESP_OK;
}

void grape_gpu_msaa_release(grape_gpu_context_t *context)
{
    if (!context) {
        return;
    }
    heap_caps_free(context->msaa_color);
    context->msaa_color = NULL;
    context->msaa_color_size = 0U;
}
