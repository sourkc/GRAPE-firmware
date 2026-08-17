#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "grape_gpu_internal.h"
#include "grape_internal.h"

#define GPU_TILE_PIXELS (GRAPE_GPU_MSAA_TILE_SIZE * GRAPE_GPU_MSAA_TILE_SIZE)
#define GPU_TILE_MAX_SAMPLES 4U

static bool gpu_depth_compare_tile(grape_gpu_compare_op_t op,
                                   uint16_t incoming,
                                   uint16_t stored)
{
    switch (op) {
        case GRAPE_GPU_COMPARE_LESS:
            return incoming < stored;
        case GRAPE_GPU_COMPARE_LEQUAL:
            return incoming <= stored;
        case GRAPE_GPU_COMPARE_EQUAL:
            return incoming == stored;
        case GRAPE_GPU_COMPARE_GEQUAL:
            return incoming >= stored;
        case GRAPE_GPU_COMPARE_GREATER:
            return incoming > stored;
        case GRAPE_GPU_COMPARE_NEVER:
            return false;
        case GRAPE_GPU_COMPARE_ALWAYS:
            return true;
        default:
            return false;
    }
}

static uint16_t gpu_depth_to_d16_tile(float depth)
{
    if (depth <= 0.0f) {
        return 0U;
    }
    if (depth >= 65535.0f) {
        return UINT16_MAX;
    }
    return (uint16_t)(depth + 0.5f);
}

static uint32_t gpu_pack_color(grape_color_t color)
{
    const uint8_t rgba[4] = { color.r, color.g, color.b, color.a };
    uint32_t packed;
    memcpy(&packed, rgba, sizeof(packed));
    return packed;
}

static void gpu_sample_offsets(grape_gpu_sample_count_t sample_count,
                               const int8_t **out_x,
                               const int8_t **out_y)
{
    static const int8_t sample_2x_x[2] = { 2, 6 };
    static const int8_t sample_2x_y[2] = { 2, 6 };
    static const int8_t sample_4x_x[4] = { 3, 7, 1, 5 };
    static const int8_t sample_4x_y[4] = { 1, 3, 5, 7 };

    if (sample_count == GRAPE_GPU_SAMPLE_COUNT_2) {
        *out_x = sample_2x_x;
        *out_y = sample_2x_y;
        return;
    }

    *out_x = sample_4x_x;
    *out_y = sample_4x_y;
}

static void gpu_build_sample_biases(grape_gpu_prepared_triangle_t *primitive,
                                    grape_gpu_sample_count_t sample_count)
{
    const int8_t *sample_x = NULL;
    const int8_t *sample_y = NULL;
    gpu_sample_offsets(sample_count, &sample_x, &sample_y);

    const grape_gpu_triangle_setup_t *setup = &primitive->setup;
    const int64_t e0_subpixel_x = setup->e0_step_x / 8;
    const int64_t e1_subpixel_x = setup->e1_step_x / 8;
    const int64_t e2_subpixel_x = setup->e2_step_x / 8;
    const int64_t e0_subpixel_y = setup->e0_step_y / 8;
    const int64_t e1_subpixel_y = setup->e1_step_y / 8;
    const int64_t e2_subpixel_y = setup->e2_step_y / 8;
    const float inverse_subpixel = 1.0f / 8.0f;

    for (uint32_t sample = 0U; sample < (uint32_t)sample_count; ++sample) {
        const int32_t dx = (int32_t)sample_x[sample] - 4;
        const int32_t dy = (int32_t)sample_y[sample] - 4;
        primitive->sample_e0_bias[sample] = e0_subpixel_x * dx + e0_subpixel_y * dy;
        primitive->sample_e1_bias[sample] = e1_subpixel_x * dx + e1_subpixel_y * dy;
        primitive->sample_e2_bias[sample] = e2_subpixel_x * dx + e2_subpixel_y * dy;
        primitive->sample_depth_bias[sample] =
            setup->depth_step_x * ((float)dx * inverse_subpixel) +
            setup->depth_step_y * ((float)dy * inverse_subpixel);
    }
}

static esp_err_t gpu_ensure_primitive_capacity(grape_gpu_context_t *context, size_t required)
{
    if (required <= context->tile_primitive_capacity) {
        return ESP_OK;
    }

    size_t capacity = context->tile_primitive_capacity ? context->tile_primitive_capacity : 32U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return ESP_ERR_INVALID_SIZE;
        }
        capacity *= 2U;
    }

    grape_gpu_prepared_triangle_t *replacement = realloc(
        context->tile_primitives,
        capacity * sizeof(*replacement)
    );
    if (!replacement) {
        return ESP_ERR_NO_MEM;
    }

    context->tile_primitives = replacement;
    context->tile_primitive_capacity = capacity;
    return ESP_OK;
}

static esp_err_t gpu_ensure_tile_metadata(grape_gpu_context_t *context, size_t tile_count)
{
    if (tile_count <= context->tile_meta_capacity) {
        return ESP_OK;
    }

    uint32_t *counts = realloc(context->tile_counts, tile_count * sizeof(*counts));
    if (!counts) {
        return ESP_ERR_NO_MEM;
    }
    context->tile_counts = counts;

    uint32_t *offsets = realloc(context->tile_offsets, (tile_count + 1U) * sizeof(*offsets));
    if (!offsets) {
        return ESP_ERR_NO_MEM;
    }
    context->tile_offsets = offsets;
    context->tile_meta_capacity = tile_count;
    return ESP_OK;
}

static esp_err_t gpu_ensure_tile_refs(grape_gpu_context_t *context, size_t required)
{
    if (required <= context->tile_ref_capacity) {
        return ESP_OK;
    }
    if (required > SIZE_MAX / sizeof(*context->tile_refs)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t *refs = realloc(context->tile_refs, required * sizeof(*refs));
    if (!refs) {
        return ESP_ERR_NO_MEM;
    }
    context->tile_refs = refs;
    context->tile_ref_capacity = required;
    return ESP_OK;
}

static esp_err_t gpu_ensure_workspace(grape_gpu_context_t *context)
{
    const size_t samples = (size_t)context->sample_count;
    const size_t color_values = GPU_TILE_PIXELS * samples;
    const size_t depth_values = GPU_TILE_PIXELS * samples;

    if (context->tile_color_capacity < color_values) {
        uint32_t *replacement = heap_caps_malloc(
            color_values * sizeof(*replacement),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
        if (!replacement) {
            return ESP_ERR_NO_MEM;
        }
        heap_caps_free(context->tile_color);
        context->tile_color = replacement;
        context->tile_color_capacity = color_values;
    }

    if (context->depth_attachment && context->tile_depth_capacity < depth_values) {
        uint16_t *replacement = heap_caps_malloc(
            depth_values * sizeof(*replacement),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
        if (!replacement) {
            return ESP_ERR_NO_MEM;
        }
        heap_caps_free(context->tile_depth);
        context->tile_depth = replacement;
        context->tile_depth_capacity = depth_values;
    }

    return ESP_OK;
}

esp_err_t grape_gpu_tile_begin(grape_gpu_context_t *context,
                               grape_gpu_load_op_t load_op,
                               grape_color_t clear_color)
{
    GRAPE_TIME_SCOPE(GPU_MSAA_PREPARE);
    if (!context || !context->color_attachment || context->sample_count == GRAPE_GPU_SAMPLE_COUNT_1) {
        return ESP_ERR_INVALID_STATE;
    }

    context->tile_primitive_count = 0U;
    context->tile_color_load_op = load_op;
    context->tile_clear_color = clear_color;
    context->tile_cols =
        (context->color_attachment->width + GRAPE_GPU_MSAA_TILE_SIZE - 1U) /
        GRAPE_GPU_MSAA_TILE_SIZE;
    context->tile_rows =
        (context->color_attachment->height + GRAPE_GPU_MSAA_TILE_SIZE - 1U) /
        GRAPE_GPU_MSAA_TILE_SIZE;

    return gpu_ensure_workspace(context);
}

esp_err_t grape_gpu_tile_enqueue(grape_gpu_context_t *context,
                                 const grape_gpu_triangle_setup_t *setup,
                                 grape_color_t color,
                                 grape_gpu_depth_state_t depth)
{
    if (!context || !setup || context->sample_count == GRAPE_GPU_SAMPLE_COUNT_1) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = gpu_ensure_primitive_capacity(context, context->tile_primitive_count + 1U);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_gpu_prepared_triangle_t *primitive =
        &context->tile_primitives[context->tile_primitive_count++];
    memset(primitive, 0, sizeof(*primitive));
    primitive->setup = *setup;
    primitive->color = color;
    primitive->depth = depth;
    gpu_build_sample_biases(primitive, context->sample_count);
    return ESP_OK;
}

static esp_err_t gpu_build_bins(grape_gpu_context_t *context)
{
    GRAPE_TIME_SCOPE(GPU_TILE_BIN);

    const size_t tile_count = (size_t)context->tile_cols * context->tile_rows;
    if (context->tile_cols != 0U && tile_count / context->tile_cols != context->tile_rows) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = gpu_ensure_tile_metadata(context, tile_count);
    if (ret != ESP_OK) {
        return ret;
    }
    memset(context->tile_counts, 0, tile_count * sizeof(*context->tile_counts));

    size_t total_refs = 0U;
    for (size_t i = 0U; i < context->tile_primitive_count; ++i) {
        const grape_gpu_triangle_setup_t *setup = &context->tile_primitives[i].setup;
        const uint32_t tx0 = (uint32_t)setup->min_x / GRAPE_GPU_MSAA_TILE_SIZE;
        const uint32_t ty0 = (uint32_t)setup->min_y / GRAPE_GPU_MSAA_TILE_SIZE;
        const uint32_t tx1 = (uint32_t)setup->max_x / GRAPE_GPU_MSAA_TILE_SIZE;
        const uint32_t ty1 = (uint32_t)setup->max_y / GRAPE_GPU_MSAA_TILE_SIZE;

        for (uint32_t ty = ty0; ty <= ty1; ++ty) {
            for (uint32_t tx = tx0; tx <= tx1; ++tx) {
                const size_t tile_index = (size_t)ty * context->tile_cols + tx;
                if (context->tile_counts[tile_index] == UINT32_MAX || total_refs == UINT32_MAX) {
                    return ESP_ERR_INVALID_SIZE;
                }
                ++context->tile_counts[tile_index];
                ++total_refs;
            }
        }
    }

    ret = gpu_ensure_tile_refs(context, total_refs);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t running = 0U;
    for (size_t tile = 0U; tile < tile_count; ++tile) {
        context->tile_offsets[tile] = running;
        running += context->tile_counts[tile];
        context->tile_counts[tile] = 0U;
    }
    context->tile_offsets[tile_count] = running;

    for (uint32_t i = 0U; i < (uint32_t)context->tile_primitive_count; ++i) {
        const grape_gpu_triangle_setup_t *setup = &context->tile_primitives[i].setup;
        const uint32_t tx0 = (uint32_t)setup->min_x / GRAPE_GPU_MSAA_TILE_SIZE;
        const uint32_t ty0 = (uint32_t)setup->min_y / GRAPE_GPU_MSAA_TILE_SIZE;
        const uint32_t tx1 = (uint32_t)setup->max_x / GRAPE_GPU_MSAA_TILE_SIZE;
        const uint32_t ty1 = (uint32_t)setup->max_y / GRAPE_GPU_MSAA_TILE_SIZE;
        for (uint32_t ty = ty0; ty <= ty1; ++ty) {
            for (uint32_t tx = tx0; tx <= tx1; ++tx) {
                const size_t tile_index = (size_t)ty * context->tile_cols + tx;
                const uint32_t dst = context->tile_offsets[tile_index] +
                                     context->tile_counts[tile_index]++;
                context->tile_refs[dst] = i;
            }
        }
    }

    return ESP_OK;
}

static void gpu_init_color_tile(grape_gpu_context_t *context,
                                uint32_t tile_x,
                                uint32_t tile_y,
                                uint32_t tile_width,
                                uint32_t tile_height)
{
    GRAPE_TIME_SCOPE(GPU_MSAA_TILE_INIT);

    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t stride_values = GRAPE_GPU_MSAA_TILE_SIZE * samples;

    if (context->tile_color_load_op == GRAPE_GPU_LOAD_OP_CLEAR) {
        const uint32_t packed = gpu_pack_color(context->tile_clear_color);
        const size_t values_per_row = (size_t)tile_width * samples;
        for (uint32_t y = 0U; y < tile_height; ++y) {
            uint32_t *row = context->tile_color + (size_t)y * stride_values;
            for (size_t i = 0U; i < values_per_row; ++i) {
                row[i] = packed;
            }
        }
        return;
    }

    const grape_texture_t *target = context->color_attachment;
    const uint32_t x0 = tile_x * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t y0 = tile_y * GRAPE_GPU_MSAA_TILE_SIZE;
    for (uint32_t y = 0U; y < tile_height; ++y) {
        const uint8_t *src = target->pixels + (size_t)(y0 + y) * target->stride + (size_t)x0 * 4U;
        uint32_t *dst = context->tile_color + (size_t)y * stride_values;
        for (uint32_t x = 0U; x < tile_width; ++x) {
            uint32_t packed;
            memcpy(&packed, src + (size_t)x * 4U, sizeof(packed));
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                dst[(size_t)x * samples + sample] = packed;
            }
        }
    }
}

static bool gpu_raster_tile_color_only(grape_gpu_context_t *context,
                                       const grape_gpu_prepared_triangle_t *primitive,
                                       int32_t x0,
                                       int32_t y0,
                                       int32_t x1,
                                       int32_t y1,
                                       uint32_t packed_color)
{
    const grape_gpu_triangle_setup_t *setup = &primitive->setup;
    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t tile_stride = GRAPE_GPU_MSAA_TILE_SIZE * samples;
    const int32_t dx0 = x0 - setup->min_x;
    const int32_t dy0 = y0 - setup->min_y;
    int64_t row_e0 = setup->row_e0 + setup->e0_step_x * dx0 + setup->e0_step_y * dy0;
    int64_t row_e1 = setup->row_e1 + setup->e1_step_x * dx0 + setup->e1_step_y * dy0;
    int64_t row_e2 = setup->row_e2 + setup->e2_step_x * dx0 + setup->e2_step_y * dy0;
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = context->tile_color +
            (size_t)local_y * tile_stride +
            ((uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U)) * samples;

        for (int32_t x = x0; x <= x1; ++x) {
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + primitive->sample_e0_bias[sample] >= 0 &&
                    e1 + primitive->sample_e1_bias[sample] >= 0 &&
                    e2 + primitive->sample_e2_bias[sample] >= 0) {
                    color[sample] = packed_color;
                    wrote = true;
                }
            }
            color += samples;
            e0 += setup->e0_step_x;
            e1 += setup->e1_step_x;
            e2 += setup->e2_step_x;
        }

        row_e0 += setup->e0_step_y;
        row_e1 += setup->e1_step_y;
        row_e2 += setup->e2_step_y;
    }
    return wrote;
}

static bool gpu_raster_tile_depth_less_write(grape_gpu_context_t *context,
                                             const grape_gpu_prepared_triangle_t *primitive,
                                             int32_t x0,
                                             int32_t y0,
                                             int32_t x1,
                                             int32_t y1,
                                             uint32_t packed_color,
                                             bool *out_depth_dirty)
{
    const grape_gpu_triangle_setup_t *setup = &primitive->setup;
    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t tile_stride = GRAPE_GPU_MSAA_TILE_SIZE * samples;
    const int32_t dx0 = x0 - setup->min_x;
    const int32_t dy0 = y0 - setup->min_y;
    int64_t row_e0 = setup->row_e0 + setup->e0_step_x * dx0 + setup->e0_step_y * dy0;
    int64_t row_e1 = setup->row_e1 + setup->e1_step_x * dx0 + setup->e1_step_y * dy0;
    int64_t row_e2 = setup->row_e2 + setup->e2_step_x * dx0 + setup->e2_step_y * dy0;
    float row_depth = setup->depth_row_start + setup->depth_step_x * dx0 + setup->depth_step_y * dy0;
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        float depth_f = row_depth;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        const uint32_t local_x0 = (uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = context->tile_color + (size_t)local_y * tile_stride + local_x0 * samples;
        uint16_t *depth = context->tile_depth + (size_t)local_y * tile_stride + local_x0 * samples;

        for (int32_t x = x0; x <= x1; ++x) {
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + primitive->sample_e0_bias[sample] >= 0 &&
                    e1 + primitive->sample_e1_bias[sample] >= 0 &&
                    e2 + primitive->sample_e2_bias[sample] >= 0) {
                    const uint16_t incoming = gpu_depth_to_d16_tile(
                        depth_f + primitive->sample_depth_bias[sample]
                    );
                    if (incoming < depth[sample]) {
                        depth[sample] = incoming;
                        color[sample] = packed_color;
                        wrote = true;
                        *out_depth_dirty = true;
                    }
                }
            }
            color += samples;
            depth += samples;
            e0 += setup->e0_step_x;
            e1 += setup->e1_step_x;
            e2 += setup->e2_step_x;
            depth_f += setup->depth_step_x;
        }

        row_e0 += setup->e0_step_y;
        row_e1 += setup->e1_step_y;
        row_e2 += setup->e2_step_y;
        row_depth += setup->depth_step_y;
    }
    return wrote;
}

static bool gpu_raster_tile_depth_generic(grape_gpu_context_t *context,
                                          const grape_gpu_prepared_triangle_t *primitive,
                                          int32_t x0,
                                          int32_t y0,
                                          int32_t x1,
                                          int32_t y1,
                                          uint32_t packed_color,
                                          bool *out_depth_dirty)
{
    const grape_gpu_triangle_setup_t *setup = &primitive->setup;
    const grape_gpu_depth_state_t depth_state = primitive->depth;
    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t tile_stride = GRAPE_GPU_MSAA_TILE_SIZE * samples;
    const int32_t dx0 = x0 - setup->min_x;
    const int32_t dy0 = y0 - setup->min_y;
    int64_t row_e0 = setup->row_e0 + setup->e0_step_x * dx0 + setup->e0_step_y * dy0;
    int64_t row_e1 = setup->row_e1 + setup->e1_step_x * dx0 + setup->e1_step_y * dy0;
    int64_t row_e2 = setup->row_e2 + setup->e2_step_x * dx0 + setup->e2_step_y * dy0;
    float row_depth = setup->depth_row_start + setup->depth_step_x * dx0 + setup->depth_step_y * dy0;
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        float depth_f = row_depth;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        const uint32_t local_x0 = (uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = context->tile_color + (size_t)local_y * tile_stride + local_x0 * samples;
        uint16_t *depth = context->tile_depth + (size_t)local_y * tile_stride + local_x0 * samples;

        for (int32_t x = x0; x <= x1; ++x) {
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + primitive->sample_e0_bias[sample] >= 0 &&
                    e1 + primitive->sample_e1_bias[sample] >= 0 &&
                    e2 + primitive->sample_e2_bias[sample] >= 0) {
                    const uint16_t incoming = gpu_depth_to_d16_tile(
                        depth_f + primitive->sample_depth_bias[sample]
                    );
                    const bool pass = !depth_state.test_enable ||
                        gpu_depth_compare_tile(depth_state.compare_op, incoming, depth[sample]);
                    if (pass) {
                        if (depth_state.write_enable) {
                            depth[sample] = incoming;
                            *out_depth_dirty = true;
                        }
                        color[sample] = packed_color;
                        wrote = true;
                    }
                }
            }
            color += samples;
            depth += samples;
            e0 += setup->e0_step_x;
            e1 += setup->e1_step_x;
            e2 += setup->e2_step_x;
            depth_f += setup->depth_step_x;
        }

        row_e0 += setup->e0_step_y;
        row_e1 += setup->e1_step_y;
        row_e2 += setup->e2_step_y;
        row_depth += setup->depth_step_y;
    }
    return wrote;
}

static inline void gpu_resolve_mixed(const uint32_t *sample_color,
                                     uint32_t samples,
                                     uint8_t *dst)
{
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
        uint8_t rgba[4];
        memcpy(rgba, &sample_color[sample], sizeof(rgba));
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

    if (alpha_sum == 0U) {
        memset(dst, 0, 4U);
        return;
    }
    if (binary_alpha) {
        dst[0] = (uint8_t)((opaque_r_sum + opaque_samples / 2U) / opaque_samples);
        dst[1] = (uint8_t)((opaque_g_sum + opaque_samples / 2U) / opaque_samples);
        dst[2] = (uint8_t)((opaque_b_sum + opaque_samples / 2U) / opaque_samples);
        dst[3] = (uint8_t)((opaque_samples * 255U + samples / 2U) / samples);
        return;
    }

    dst[0] = (uint8_t)((premul_r_sum + alpha_sum / 2U) / alpha_sum);
    dst[1] = (uint8_t)((premul_g_sum + alpha_sum / 2U) / alpha_sum);
    dst[2] = (uint8_t)((premul_b_sum + alpha_sum / 2U) / alpha_sum);
    dst[3] = (uint8_t)((alpha_sum + samples / 2U) / samples);
}

static void gpu_resolve_tile(grape_gpu_context_t *context,
                             uint32_t tile_x,
                             uint32_t tile_y,
                             uint32_t tile_width,
                             uint32_t tile_height)
{
    GRAPE_TIME_SCOPE(GPU_MSAA_RESOLVE);

    grape_texture_t *target = context->color_attachment;
    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t tile_stride = GRAPE_GPU_MSAA_TILE_SIZE * samples;
    const uint32_t x0 = tile_x * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t y0 = tile_y * GRAPE_GPU_MSAA_TILE_SIZE;

    for (uint32_t y = 0U; y < tile_height; ++y) {
        uint8_t *dst = target->pixels + (size_t)(y0 + y) * target->stride + (size_t)x0 * 4U;
        const uint32_t *samples_row = context->tile_color + (size_t)y * tile_stride;
        for (uint32_t x = 0U; x < tile_width; ++x) {
            const uint32_t *pixel_samples = samples_row + (size_t)x * samples;
            bool identical = true;
            for (uint32_t sample = 1U; sample < samples; ++sample) {
                if (pixel_samples[sample] != pixel_samples[0]) {
                    identical = false;
                    break;
                }
            }
            if (identical) {
                memcpy(dst, &pixel_samples[0], 4U);
            } else {
                gpu_resolve_mixed(pixel_samples, samples, dst);
            }
            dst += 4U;
        }
    }
}

static esp_err_t gpu_render_tile(grape_gpu_context_t *context,
                                 uint32_t tile_x,
                                 uint32_t tile_y,
                                 uint32_t ref_begin,
                                 uint32_t ref_end)
{
    const uint32_t x0 = tile_x * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t y0 = tile_y * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t tile_width = x0 + GRAPE_GPU_MSAA_TILE_SIZE <= context->color_attachment->width
        ? GRAPE_GPU_MSAA_TILE_SIZE
        : context->color_attachment->width - x0;
    const uint32_t tile_height = y0 + GRAPE_GPU_MSAA_TILE_SIZE <= context->color_attachment->height
        ? GRAPE_GPU_MSAA_TILE_SIZE
        : context->color_attachment->height - y0;
    const uint32_t depth_stride_values = GRAPE_GPU_MSAA_TILE_SIZE * (uint32_t)context->sample_count;

    gpu_init_color_tile(context, tile_x, tile_y, tile_width, tile_height);

    bool needs_depth = false;
    for (uint32_t ref = ref_begin; ref < ref_end; ++ref) {
        const grape_gpu_depth_state_t *depth = &context->tile_primitives[context->tile_refs[ref]].depth;
        if (depth->test_enable || depth->write_enable) {
            needs_depth = true;
            break;
        }
    }
    if (needs_depth) {
        grape_gpu_depth_load_tile(context->depth_attachment,
                                  tile_x,
                                  tile_y,
                                  context->tile_depth,
                                  depth_stride_values);
    }

    bool color_dirty = false;
    bool depth_dirty = false;
    GRAPE_TIME_BLOCK(GPU_TILE_RASTER) {
        for (uint32_t ref = ref_begin; ref < ref_end; ++ref) {
            const grape_gpu_prepared_triangle_t *primitive =
                &context->tile_primitives[context->tile_refs[ref]];
            const grape_gpu_triangle_setup_t *setup = &primitive->setup;
            const int32_t rx0 = setup->min_x > (int32_t)x0 ? setup->min_x : (int32_t)x0;
            const int32_t ry0 = setup->min_y > (int32_t)y0 ? setup->min_y : (int32_t)y0;
            const int32_t tile_x1 = (int32_t)(x0 + tile_width - 1U);
            const int32_t tile_y1 = (int32_t)(y0 + tile_height - 1U);
            const int32_t rx1 = setup->max_x < tile_x1 ? setup->max_x : tile_x1;
            const int32_t ry1 = setup->max_y < tile_y1 ? setup->max_y : tile_y1;
            if (rx0 > rx1 || ry0 > ry1) {
                continue;
            }

            const uint32_t packed = gpu_pack_color(primitive->color);
            bool wrote = false;
            if (!primitive->depth.test_enable && !primitive->depth.write_enable) {
                wrote = gpu_raster_tile_color_only(context, primitive, rx0, ry0, rx1, ry1, packed);
            } else if (primitive->depth.test_enable && primitive->depth.write_enable &&
                       primitive->depth.compare_op == GRAPE_GPU_COMPARE_LESS) {
                wrote = gpu_raster_tile_depth_less_write(
                    context, primitive, rx0, ry0, rx1, ry1, packed, &depth_dirty
                );
            } else {
                wrote = gpu_raster_tile_depth_generic(
                    context, primitive, rx0, ry0, rx1, ry1, packed, &depth_dirty
                );
            }
            color_dirty |= wrote;
        }
    }

    if (depth_dirty) {
        grape_gpu_depth_store_tile(context->depth_attachment,
                                   tile_x,
                                   tile_y,
                                   context->tile_depth,
                                   depth_stride_values);
    }

    if (color_dirty) {
        gpu_resolve_tile(context, tile_x, tile_y, tile_width, tile_height);
        grape_gpu_dirty_add(context, (grape_rect_t) {
            .x = (int32_t)x0,
            .y = (int32_t)y0,
            .width = (int32_t)tile_width,
            .height = (int32_t)tile_height,
        });
    }
    return ESP_OK;
}

esp_err_t grape_gpu_tile_execute(grape_gpu_context_t *context)
{
    if (!context || context->sample_count == GRAPE_GPU_SAMPLE_COUNT_1) {
        return ESP_ERR_INVALID_STATE;
    }
    if (context->tile_primitive_count == 0U) {
        return ESP_OK;
    }

    esp_err_t ret = gpu_build_bins(context);
    if (ret != ESP_OK) {
        context->tile_primitive_count = 0U;
        return ret;
    }

    const size_t tile_count = (size_t)context->tile_cols * context->tile_rows;
    for (size_t tile = 0U; tile < tile_count; ++tile) {
        const uint32_t begin = context->tile_offsets[tile];
        const uint32_t end = context->tile_offsets[tile + 1U];
        if (begin == end) {
            continue;
        }

        const uint32_t tile_y = (uint32_t)(tile / context->tile_cols);
        const uint32_t tile_x = (uint32_t)(tile - (size_t)tile_y * context->tile_cols);
        ret = gpu_render_tile(context, tile_x, tile_y, begin, end);
        if (ret != ESP_OK) {
            break;
        }
    }

    context->tile_primitive_count = 0U;
    return ret;
}

void grape_gpu_tile_release(grape_gpu_context_t *context)
{
    if (!context) {
        return;
    }

    free(context->tile_primitives);
    context->tile_primitives = NULL;
    context->tile_primitive_count = 0U;
    context->tile_primitive_capacity = 0U;
    free(context->tile_counts);
    context->tile_counts = NULL;
    free(context->tile_offsets);
    context->tile_offsets = NULL;
    context->tile_meta_capacity = 0U;
    free(context->tile_refs);
    context->tile_refs = NULL;
    context->tile_ref_capacity = 0U;
    heap_caps_free(context->tile_color);
    context->tile_color = NULL;
    context->tile_color_capacity = 0U;
    heap_caps_free(context->tile_depth);
    context->tile_depth = NULL;
    context->tile_depth_capacity = 0U;
    context->tile_cols = 0U;
    context->tile_rows = 0U;
}
