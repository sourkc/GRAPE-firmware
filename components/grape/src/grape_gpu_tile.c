#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
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

#define GPU_SUBPIXEL_SCALE 8
#define GPU_SUBPIXEL_HALF 4
#define GPU_DEPTH_FP_BITS 14
#define GPU_DEPTH_FP_SCALE (1 << GPU_DEPTH_FP_BITS)
#define GPU_DEPTH_FP_HALF (GPU_DEPTH_FP_SCALE / 2)
#define GPU_DEPTH_FP_MAX ((int32_t)(65535U << GPU_DEPTH_FP_BITS))

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

static bool gpu_i64_to_i32_checked(int64_t value, int32_t *out_value)
{
    if (value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *out_value = (int32_t)value;
    return true;
}

static bool gpu_float_to_depth_fp_checked(float value, int32_t *out_value)
{
    const double scaled = (double)value * (double)GPU_DEPTH_FP_SCALE;
    if (!isfinite(value) || scaled < (double)INT32_MIN || scaled > (double)INT32_MAX) {
        return false;
    }
    *out_value = (int32_t)llround(scaled);
    return true;
}

static bool gpu_edge_range_fits_i32(const grape_gpu_triangle_setup_t *setup,
                                    uint32_t edge,
                                    int64_t min_bias,
                                    int64_t max_bias)
{
    const int64_t row = edge == 0U ? setup->row_e0 : edge == 1U ? setup->row_e1 : setup->row_e2;
    const int64_t step_x = edge == 0U ? setup->e0_step_x : edge == 1U ? setup->e1_step_x : setup->e2_step_x;
    const int64_t step_y = edge == 0U ? setup->e0_step_y : edge == 1U ? setup->e1_step_y : setup->e2_step_y;
    const int64_t width = (int64_t)setup->max_x - setup->min_x;
    const int64_t height = (int64_t)setup->max_y - setup->min_y;
    const int64_t corners[4] = {
        row,
        row + step_x * width,
        row + step_y * height,
        row + step_x * width + step_y * height,
    };

    if (step_x < INT32_MIN || step_x > INT32_MAX || step_y < INT32_MIN || step_y > INT32_MAX) {
        return false;
    }

    for (size_t i = 0U; i < 4U; ++i) {
        if (corners[i] < INT32_MIN || corners[i] > INT32_MAX ||
            corners[i] + min_bias < INT32_MIN || corners[i] + min_bias > INT32_MAX ||
            corners[i] + max_bias < INT32_MIN || corners[i] + max_bias > INT32_MAX) {
            return false;
        }
    }
    return true;
}

static bool gpu_depth_range_fits_i32(const grape_gpu_triangle_setup_t *setup,
                                     const float sample_bias[4],
                                     uint32_t sample_count)
{
    float min_bias = sample_bias[0];
    float max_bias = sample_bias[0];
    for (uint32_t sample = 1U; sample < sample_count; ++sample) {
        if (sample_bias[sample] < min_bias) {
            min_bias = sample_bias[sample];
        }
        if (sample_bias[sample] > max_bias) {
            max_bias = sample_bias[sample];
        }
    }

    const float width = (float)(setup->max_x - setup->min_x);
    const float height = (float)(setup->max_y - setup->min_y);
    const float corners[4] = {
        setup->depth_row_start,
        setup->depth_row_start + setup->depth_step_x * width,
        setup->depth_row_start + setup->depth_step_y * height,
        setup->depth_row_start + setup->depth_step_x * width + setup->depth_step_y * height,
    };

    for (size_t i = 0U; i < 4U; ++i) {
        int32_t ignored;
        if (!gpu_float_to_depth_fp_checked(corners[i], &ignored) ||
            !gpu_float_to_depth_fp_checked(corners[i] + min_bias, &ignored) ||
            !gpu_float_to_depth_fp_checked(corners[i] + max_bias, &ignored)) {
            return false;
        }
    }
    return true;
}

static void gpu_build_sample_biases(grape_gpu_prepared_triangle_t *primitive,
                                    grape_gpu_sample_count_t sample_count)
{
    const int8_t *sample_x = NULL;
    const int8_t *sample_y = NULL;
    gpu_sample_offsets(sample_count, &sample_x, &sample_y);

    const grape_gpu_triangle_setup_t *setup = &primitive->setup;
    const int64_t edge_subpixel_x[3] = {
        setup->e0_step_x / GPU_SUBPIXEL_SCALE,
        setup->e1_step_x / GPU_SUBPIXEL_SCALE,
        setup->e2_step_x / GPU_SUBPIXEL_SCALE,
    };
    const int64_t edge_subpixel_y[3] = {
        setup->e0_step_y / GPU_SUBPIXEL_SCALE,
        setup->e1_step_y / GPU_SUBPIXEL_SCALE,
        setup->e2_step_y / GPU_SUBPIXEL_SCALE,
    };
    const float inverse_subpixel = 1.0f / (float)GPU_SUBPIXEL_SCALE;
    float depth_bias[4] = {0};
    int64_t edge_bias[3][4] = {{0}};

    for (uint32_t sample = 0U; sample < (uint32_t)sample_count; ++sample) {
        const int32_t dx = (int32_t)sample_x[sample] - GPU_SUBPIXEL_HALF;
        const int32_t dy = (int32_t)sample_y[sample] - GPU_SUBPIXEL_HALF;
        for (uint32_t edge = 0U; edge < 3U; ++edge) {
            edge_bias[edge][sample] = edge_subpixel_x[edge] * dx + edge_subpixel_y[edge] * dy;
        }
        depth_bias[sample] =
            setup->depth_step_x * ((float)dx * inverse_subpixel) +
            setup->depth_step_y * ((float)dy * inverse_subpixel);
    }

    for (uint32_t sample = 0U; sample < (uint32_t)sample_count; ++sample) {
        primitive->sample_e0_bias_fallback[sample] = edge_bias[0][sample];
        primitive->sample_e1_bias_fallback[sample] = edge_bias[1][sample];
        primitive->sample_e2_bias_fallback[sample] = edge_bias[2][sample];
        primitive->sample_depth_bias_fallback[sample] = depth_bias[sample];
    }

    primitive->raster_i32_valid = true;
    for (uint32_t edge = 0U; edge < 3U; ++edge) {
        int64_t min_bias = edge_bias[edge][0];
        int64_t max_bias = edge_bias[edge][0];
        for (uint32_t sample = 1U; sample < (uint32_t)sample_count; ++sample) {
            if (edge_bias[edge][sample] < min_bias) {
                min_bias = edge_bias[edge][sample];
            }
            if (edge_bias[edge][sample] > max_bias) {
                max_bias = edge_bias[edge][sample];
            }
        }

        const int64_t row = edge == 0U ? setup->row_e0 : edge == 1U ? setup->row_e1 : setup->row_e2;
        const int64_t step_x = edge == 0U ? setup->e0_step_x : edge == 1U ? setup->e1_step_x : setup->e2_step_x;
        const int64_t step_y = edge == 0U ? setup->e0_step_y : edge == 1U ? setup->e1_step_y : setup->e2_step_y;
        if (!gpu_edge_range_fits_i32(setup, edge, min_bias, max_bias) ||
            !gpu_i64_to_i32_checked(row, &primitive->row_e[edge]) ||
            !gpu_i64_to_i32_checked(step_x, &primitive->edge_step_x[edge]) ||
            !gpu_i64_to_i32_checked(step_y, &primitive->edge_step_y[edge]) ||
            !gpu_i64_to_i32_checked(min_bias, &primitive->min_sample_edge_bias[edge])) {
            primitive->raster_i32_valid = false;
            break;
        }
        for (uint32_t sample = 0U; sample < (uint32_t)sample_count; ++sample) {
            if (!gpu_i64_to_i32_checked(edge_bias[edge][sample],
                                        &primitive->sample_edge_bias[edge][sample])) {
                primitive->raster_i32_valid = false;
                break;
            }
        }
        if (!primitive->raster_i32_valid) {
            break;
        }
    }

    if (!primitive->raster_i32_valid ||
        !gpu_depth_range_fits_i32(setup, depth_bias, (uint32_t)sample_count) ||
        !gpu_float_to_depth_fp_checked(setup->depth_row_start, &primitive->depth_row_start_fp) ||
        !gpu_float_to_depth_fp_checked(setup->depth_step_x, &primitive->depth_step_x_fp) ||
        !gpu_float_to_depth_fp_checked(setup->depth_step_y, &primitive->depth_step_y_fp)) {
        primitive->raster_i32_valid = false;
        return;
    }

    for (uint32_t sample = 0U; sample < (uint32_t)sample_count; ++sample) {
        if (!gpu_float_to_depth_fp_checked(depth_bias[sample],
                                           &primitive->sample_depth_bias_fp[sample])) {
            primitive->raster_i32_valid = false;
            return;
        }
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

static esp_err_t gpu_ensure_tile_jobs(grape_gpu_context_t *context, size_t required)
{
    if (required <= context->tile_job_capacity) {
        return ESP_OK;
    }
    if (required > SIZE_MAX / sizeof(*context->tile_jobs)) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_gpu_tile_job_t *jobs = realloc(
        context->tile_jobs,
        required * sizeof(*jobs)
    );
    if (!jobs) {
        return ESP_ERR_NO_MEM;
    }

    context->tile_jobs = jobs;
    context->tile_job_capacity = required;
    return ESP_OK;
}

static esp_err_t gpu_ensure_worker_workspace(grape_gpu_context_t *context,
                                             grape_gpu_worker_t *worker)
{
    const size_t samples = (size_t)context->sample_count;
    const size_t color_values = GPU_TILE_PIXELS * samples;
    const size_t depth_values = GPU_TILE_PIXELS * samples;

    if (worker->tile_color_capacity < color_values) {
        uint32_t *replacement = heap_caps_malloc(
            color_values * sizeof(*replacement),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
        if (!replacement) {
            return ESP_ERR_NO_MEM;
        }
        heap_caps_free(worker->tile_color);
        worker->tile_color = replacement;
        worker->tile_color_capacity = color_values;
    }

    if (context->depth_attachment && worker->tile_depth_capacity < depth_values) {
        uint16_t *replacement = heap_caps_malloc(
            depth_values * sizeof(*replacement),
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
        );
        if (!replacement) {
            return ESP_ERR_NO_MEM;
        }
        heap_caps_free(worker->tile_depth);
        worker->tile_depth = replacement;
        worker->tile_depth_capacity = depth_values;
    }

    return ESP_OK;
}

static void gpu_worker_release(grape_gpu_worker_t *worker)
{
    if (!worker) {
        return;
    }

    heap_caps_free(worker->tile_color);
    worker->tile_color = NULL;
    worker->tile_color_capacity = 0U;
    heap_caps_free(worker->tile_depth);
    worker->tile_depth = NULL;
    worker->tile_depth_capacity = 0U;
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
    context->tile_job_count = 0U;
    context->tile_job_next = 0U;
    context->tile_color_load_op = load_op;
    context->tile_clear_color = clear_color;
    context->tile_cols =
        (context->color_attachment->width + GRAPE_GPU_MSAA_TILE_SIZE - 1U) /
        GRAPE_GPU_MSAA_TILE_SIZE;
    context->tile_rows =
        (context->color_attachment->height + GRAPE_GPU_MSAA_TILE_SIZE - 1U) /
        GRAPE_GPU_MSAA_TILE_SIZE;

    return gpu_ensure_worker_workspace(context, &context->primary_worker);
}

esp_err_t grape_gpu_tile_enqueue(grape_gpu_context_t *context,
                                 const grape_gpu_triangle_setup_t *setup,
                                 grape_color_t color,
                                 grape_gpu_depth_state_t depth,
                                 grape_gpu_fragment_program_t fragment_program,
                                 grape_texture_t *texture,
                                 grape_gpu_sampler_desc_t sampler)
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
    primitive->fragment_program = fragment_program;
    primitive->texture = texture;
    primitive->sampler = sampler;
    if (texture) {
        texture->ref_count++;
    }
    gpu_build_sample_biases(primitive, context->sample_count);
    return ESP_OK;
}

static esp_err_t gpu_build_bins(grape_gpu_context_t *context)
{
    GRAPE_TIME_SCOPE(GPU_TILE_BIN);
    const bool profile = context->stats_enabled;
    const int64_t bin_start_us = profile ? esp_timer_get_time() : 0;

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
    uint32_t active_tiles = 0U;
    for (size_t tile = 0U; tile < tile_count; ++tile) {
        context->tile_offsets[tile] = running;
        if (profile && context->tile_counts[tile] != 0U) {
            ++active_tiles;
        }
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

    if (profile) {
        context->current_stats.tile_bin_us +=
            (uint64_t)(esp_timer_get_time() - bin_start_us);
        context->current_stats.active_tiles = active_tiles;
        context->current_stats.tile_references = running;
    }

    return ESP_OK;
}

static esp_err_t gpu_build_jobs(grape_gpu_context_t *context)
{
    const size_t tile_count = (size_t)context->tile_cols * context->tile_rows;
    esp_err_t ret = gpu_ensure_tile_jobs(context, tile_count);
    if (ret != ESP_OK) {
        return ret;
    }

    context->tile_job_count = 0U;
    context->tile_job_next = 0U;

    for (size_t tile = 0U; tile < tile_count; ++tile) {
        const uint32_t begin = context->tile_offsets[tile];
        const uint32_t end = context->tile_offsets[tile + 1U];
        if (begin == end) {
            continue;
        }

        const uint32_t tile_y = (uint32_t)(tile / context->tile_cols);
        const uint32_t tile_x = (uint32_t)(tile - (size_t)tile_y * context->tile_cols);
        context->tile_jobs[context->tile_job_count++] = (grape_gpu_tile_job_t) {
            .tile_x = tile_x,
            .tile_y = tile_y,
            .ref_begin = begin,
            .ref_end = end,
        };
    }

    return ESP_OK;
}

/* Jobs and their inputs are immutable from wake until both workers finish.
 * The relaxed atomic only assigns ownership; semaphore handoffs publish inputs
 * and completed framebuffer writes. No raster work runs under a scheduler lock.
 */
static bool gpu_claim_tile_job(grape_gpu_context_t *context, grape_gpu_tile_job_t *out_job)
{
    if (__atomic_load_n(&context->tile_jobs_cancelled, __ATOMIC_RELAXED)) {
        return false;
    }
    const size_t index = __atomic_fetch_add(&context->tile_job_next, 1U, __ATOMIC_RELAXED);
    if (index >= context->tile_job_count) {
        return false;
    }
    *out_job = context->tile_jobs[index];
    return true;
}

static void gpu_init_color_tile(grape_gpu_context_t *context,
                                grape_gpu_worker_t *worker,
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
            uint32_t *row = worker->tile_color + (size_t)y * stride_values;
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
        uint32_t *dst = worker->tile_color + (size_t)y * stride_values;
        for (uint32_t x = 0U; x < tile_width; ++x) {
            uint32_t packed;
            memcpy(&packed, src + (size_t)x * 4U, sizeof(packed));
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                dst[(size_t)x * samples + sample] = packed;
            }
        }
    }
}

static inline uint16_t gpu_depth_fp_to_d16(int32_t depth_fp)
{
    if (depth_fp <= 0) {
        return 0U;
    }
    if (depth_fp >= GPU_DEPTH_FP_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t)((depth_fp + GPU_DEPTH_FP_HALF) >> GPU_DEPTH_FP_BITS);
}

static inline bool gpu_all_samples_covered_i32(const grape_gpu_prepared_triangle_t *primitive,
                                                int32_t e0,
                                                int32_t e1,
                                                int32_t e2)
{
    return e0 + primitive->min_sample_edge_bias[0] >= 0 &&
           e1 + primitive->min_sample_edge_bias[1] >= 0 &&
           e2 + primitive->min_sample_edge_bias[2] >= 0;
}

static inline bool gpu_sample_covered_i32(const grape_gpu_prepared_triangle_t *primitive,
                                          uint32_t sample,
                                          int32_t e0,
                                          int32_t e1,
                                          int32_t e2)
{
    return e0 + primitive->sample_edge_bias[0][sample] >= 0 &&
           e1 + primitive->sample_edge_bias[1][sample] >= 0 &&
           e2 + primitive->sample_edge_bias[2][sample] >= 0;
}
static bool gpu_fragment_is_shaded_tile(grape_gpu_fragment_program_t program)
{
    return program == GRAPE_GPU_FRAGMENT_PROGRAM_VERTEX_COLOR ||
           program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE ||
           program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR;
}

static uint32_t gpu_coverage_mask_i32(const grape_gpu_prepared_triangle_t *primitive,
                                      uint32_t samples,
                                      int32_t e0,
                                      int32_t e1,
                                      int32_t e2)
{
    if (gpu_all_samples_covered_i32(primitive, e0, e1, e2)) {
        return (1U << samples) - 1U;
    }

    uint32_t mask = 0U;
    for (uint32_t sample = 0U; sample < samples; ++sample) {
        if (gpu_sample_covered_i32(primitive, sample, e0, e1, e2)) {
            mask |= 1U << sample;
        }
    }
    return mask;
}

static void gpu_coverage_centroid(uint32_t mask,
                                  grape_gpu_sample_count_t sample_count,
                                  float *out_dx,
                                  float *out_dy)
{
    const uint32_t samples = (uint32_t)sample_count;
    if (mask == ((1U << samples) - 1U)) {
        *out_dx = 0.0f;
        *out_dy = 0.0f;
        return;
    }

    const int8_t *sample_x = NULL;
    const int8_t *sample_y = NULL;
    gpu_sample_offsets(sample_count, &sample_x, &sample_y);
    int32_t sum_x = 0;
    int32_t sum_y = 0;
    uint32_t count = 0U;
    for (uint32_t sample = 0U; sample < samples; ++sample) {
        if ((mask & (1U << sample)) != 0U) {
            sum_x += (int32_t)sample_x[sample] - GPU_SUBPIXEL_HALF;
            sum_y += (int32_t)sample_y[sample] - GPU_SUBPIXEL_HALF;
            ++count;
        }
    }
    if (count == 0U) {
        *out_dx = 0.0f;
        *out_dy = 0.0f;
        return;
    }
    *out_dx = (float)sum_x / ((float)count * (float)GPU_SUBPIXEL_SCALE);
    *out_dy = (float)sum_y / ((float)count * (float)GPU_SUBPIXEL_SCALE);
}

static esp_err_t gpu_raster_tile_shaded_i32(grape_gpu_context_t *context,
                                             grape_gpu_worker_t *worker,
                                             const grape_gpu_prepared_triangle_t *primitive,
                                             int32_t x0,
                                             int32_t y0,
                                             int32_t x1,
                                             int32_t y1,
                                             bool *out_wrote,
                                             bool *out_depth_dirty)
{
    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t tile_stride = GRAPE_GPU_MSAA_TILE_SIZE * samples;
    const int32_t dx0 = x0 - primitive->setup.min_x;
    const int32_t dy0 = y0 - primitive->setup.min_y;
    int32_t row_e0 = (int32_t)((int64_t)primitive->row_e[0] +
                               (int64_t)primitive->edge_step_x[0] * dx0 +
                               (int64_t)primitive->edge_step_y[0] * dy0);
    int32_t row_e1 = (int32_t)((int64_t)primitive->row_e[1] +
                               (int64_t)primitive->edge_step_x[1] * dx0 +
                               (int64_t)primitive->edge_step_y[1] * dy0);
    int32_t row_e2 = (int32_t)((int64_t)primitive->row_e[2] +
                               (int64_t)primitive->edge_step_x[2] * dx0 +
                               (int64_t)primitive->edge_step_y[2] * dy0);
    int32_t row_depth = (int32_t)((int64_t)primitive->depth_row_start_fp +
                                  (int64_t)primitive->depth_step_x_fp * dx0 +
                                  (int64_t)primitive->depth_step_y_fp * dy0);
    const grape_gpu_depth_state_t depth_state = primitive->depth;
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int32_t e0 = row_e0;
        int32_t e1 = row_e1;
        int32_t e2 = row_e2;
        int32_t depth_fp = row_depth;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        const uint32_t local_x0 = (uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = worker->tile_color + (size_t)local_y * tile_stride + local_x0 * samples;
        uint16_t *depth = worker->tile_depth
            ? worker->tile_depth + (size_t)local_y * tile_stride + local_x0 * samples
            : NULL;

        for (int32_t x = x0; x <= x1; ++x) {
            const uint32_t coverage_mask = gpu_coverage_mask_i32(primitive, samples, e0, e1, e2);
            if (coverage_mask != 0U) {
                uint32_t pass_mask = 0U;
                uint16_t incoming[4] = {0U, 0U, 0U, 0U};
                for (uint32_t sample = 0U; sample < samples; ++sample) {
                    const uint32_t bit = 1U << sample;
                    if ((coverage_mask & bit) == 0U) {
                        continue;
                    }
                    incoming[sample] = gpu_depth_fp_to_d16(
                        depth_fp + primitive->sample_depth_bias_fp[sample]
                    );
                    if (!depth_state.test_enable ||
                        gpu_depth_compare_tile(depth_state.compare_op, incoming[sample], depth[sample])) {
                        pass_mask |= bit;
                    }
                }

                if (pass_mask != 0U) {
                    float centroid_x = 0.0f;
                    float centroid_y = 0.0f;
                    gpu_coverage_centroid(
                        coverage_mask, context->sample_count, &centroid_x, &centroid_y
                    );
                    grape_color_t shaded;
                    esp_err_t ret = grape_gpu_shade_fragment(
                        &primitive->setup,
                        primitive->fragment_program,
                        primitive->texture,
                        &primitive->sampler,
                        (float)x + 0.5f + centroid_x,
                        (float)y + 0.5f + centroid_y,
                        &shaded
                    );
                    if (ret != ESP_OK) {
                        return ret;
                    }
                    const uint32_t packed = gpu_pack_color(shaded);
                    for (uint32_t sample = 0U; sample < samples; ++sample) {
                        const uint32_t bit = 1U << sample;
                        if ((pass_mask & bit) == 0U) {
                            continue;
                        }
                        if (depth_state.write_enable) {
                            depth[sample] = incoming[sample];
                            *out_depth_dirty = true;
                        }
                        color[sample] = packed;
                    }
                    wrote = true;
                }
            }

            color += samples;
            if (depth) depth += samples;
            e0 += primitive->edge_step_x[0];
            e1 += primitive->edge_step_x[1];
            e2 += primitive->edge_step_x[2];
            depth_fp += primitive->depth_step_x_fp;
        }
        row_e0 += primitive->edge_step_y[0];
        row_e1 += primitive->edge_step_y[1];
        row_e2 += primitive->edge_step_y[2];
        row_depth += primitive->depth_step_y_fp;
    }

    *out_wrote = wrote;
    return ESP_OK;
}

static bool gpu_raster_tile_color_only_i32(grape_gpu_context_t *context,
                                           grape_gpu_worker_t *worker,
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
    int32_t row_e0 = (int32_t)((int64_t)primitive->row_e[0] +
                               (int64_t)primitive->edge_step_x[0] * dx0 +
                               (int64_t)primitive->edge_step_y[0] * dy0);
    int32_t row_e1 = (int32_t)((int64_t)primitive->row_e[1] +
                               (int64_t)primitive->edge_step_x[1] * dx0 +
                               (int64_t)primitive->edge_step_y[1] * dy0);
    int32_t row_e2 = (int32_t)((int64_t)primitive->row_e[2] +
                               (int64_t)primitive->edge_step_x[2] * dx0 +
                               (int64_t)primitive->edge_step_y[2] * dy0);
    const int32_t e0_step_x = primitive->edge_step_x[0];
    const int32_t e1_step_x = primitive->edge_step_x[1];
    const int32_t e2_step_x = primitive->edge_step_x[2];
    const int32_t e0_step_y = primitive->edge_step_y[0];
    const int32_t e1_step_y = primitive->edge_step_y[1];
    const int32_t e2_step_y = primitive->edge_step_y[2];
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int32_t e0 = row_e0;
        int32_t e1 = row_e1;
        int32_t e2 = row_e2;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = worker->tile_color +
            (size_t)local_y * tile_stride +
            ((uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U)) * samples;

        for (int32_t x = x0; x <= x1; ++x) {
            if (gpu_all_samples_covered_i32(primitive, e0, e1, e2)) {
                for (uint32_t sample = 0U; sample < samples; ++sample) {
                    color[sample] = packed_color;
                }
                wrote = true;
            } else {
                for (uint32_t sample = 0U; sample < samples; ++sample) {
                    if (gpu_sample_covered_i32(primitive, sample, e0, e1, e2)) {
                        color[sample] = packed_color;
                        wrote = true;
                    }
                }
            }
            color += samples;
            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
    }
    return wrote;
}

static bool gpu_raster_tile_color_only_4x_i32(grape_gpu_worker_t *worker,
                                               const grape_gpu_prepared_triangle_t *primitive,
                                               int32_t x0,
                                               int32_t y0,
                                               int32_t x1,
                                               int32_t y1,
                                               uint32_t packed_color)
{
    const grape_gpu_triangle_setup_t *setup = &primitive->setup;
    const int32_t dx0 = x0 - setup->min_x;
    const int32_t dy0 = y0 - setup->min_y;
    int32_t row_e0 = (int32_t)((int64_t)primitive->row_e[0] +
                               (int64_t)primitive->edge_step_x[0] * dx0 +
                               (int64_t)primitive->edge_step_y[0] * dy0);
    int32_t row_e1 = (int32_t)((int64_t)primitive->row_e[1] +
                               (int64_t)primitive->edge_step_x[1] * dx0 +
                               (int64_t)primitive->edge_step_y[1] * dy0);
    int32_t row_e2 = (int32_t)((int64_t)primitive->row_e[2] +
                               (int64_t)primitive->edge_step_x[2] * dx0 +
                               (int64_t)primitive->edge_step_y[2] * dy0);
    const int32_t e0_step_x = primitive->edge_step_x[0];
    const int32_t e1_step_x = primitive->edge_step_x[1];
    const int32_t e2_step_x = primitive->edge_step_x[2];
    const int32_t e0_step_y = primitive->edge_step_y[0];
    const int32_t e1_step_y = primitive->edge_step_y[1];
    const int32_t e2_step_y = primitive->edge_step_y[2];
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int32_t e0 = row_e0;
        int32_t e1 = row_e1;
        int32_t e2 = row_e2;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = worker->tile_color +
            (size_t)local_y * (GRAPE_GPU_MSAA_TILE_SIZE * 4U) +
            (((uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U)) * 4U);

        for (int32_t x = x0; x <= x1; ++x) {
            if (gpu_all_samples_covered_i32(primitive, e0, e1, e2)) {
                color[0] = packed_color;
                color[1] = packed_color;
                color[2] = packed_color;
                color[3] = packed_color;
                wrote = true;
            } else {
                if (gpu_sample_covered_i32(primitive, 0U, e0, e1, e2)) {
                    color[0] = packed_color;
                    wrote = true;
                }
                if (gpu_sample_covered_i32(primitive, 1U, e0, e1, e2)) {
                    color[1] = packed_color;
                    wrote = true;
                }
                if (gpu_sample_covered_i32(primitive, 2U, e0, e1, e2)) {
                    color[2] = packed_color;
                    wrote = true;
                }
                if (gpu_sample_covered_i32(primitive, 3U, e0, e1, e2)) {
                    color[3] = packed_color;
                    wrote = true;
                }
            }
            color += 4U;
            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
    }
    return wrote;
}

static inline bool gpu_depth_less_write_sample(uint16_t *depth,
                                               uint32_t *color,
                                               uint32_t sample,
                                               int32_t depth_fp,
                                               uint32_t packed_color)
{
    const uint16_t incoming = gpu_depth_fp_to_d16(depth_fp);
    if (incoming >= depth[sample]) {
        return false;
    }
    depth[sample] = incoming;
    color[sample] = packed_color;
    return true;
}

static bool gpu_raster_tile_depth_less_write_i32(grape_gpu_context_t *context,
                                                  grape_gpu_worker_t *worker,
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
    int32_t row_e0 = (int32_t)((int64_t)primitive->row_e[0] +
                               (int64_t)primitive->edge_step_x[0] * dx0 +
                               (int64_t)primitive->edge_step_y[0] * dy0);
    int32_t row_e1 = (int32_t)((int64_t)primitive->row_e[1] +
                               (int64_t)primitive->edge_step_x[1] * dx0 +
                               (int64_t)primitive->edge_step_y[1] * dy0);
    int32_t row_e2 = (int32_t)((int64_t)primitive->row_e[2] +
                               (int64_t)primitive->edge_step_x[2] * dx0 +
                               (int64_t)primitive->edge_step_y[2] * dy0);
    int32_t row_depth = (int32_t)((int64_t)primitive->depth_row_start_fp +
                                  (int64_t)primitive->depth_step_x_fp * dx0 +
                                  (int64_t)primitive->depth_step_y_fp * dy0);
    const int32_t e0_step_x = primitive->edge_step_x[0];
    const int32_t e1_step_x = primitive->edge_step_x[1];
    const int32_t e2_step_x = primitive->edge_step_x[2];
    const int32_t e0_step_y = primitive->edge_step_y[0];
    const int32_t e1_step_y = primitive->edge_step_y[1];
    const int32_t e2_step_y = primitive->edge_step_y[2];
    const int32_t depth_step_x = primitive->depth_step_x_fp;
    const int32_t depth_step_y = primitive->depth_step_y_fp;
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int32_t e0 = row_e0;
        int32_t e1 = row_e1;
        int32_t e2 = row_e2;
        int32_t depth_fp = row_depth;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        const uint32_t local_x0 = (uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = worker->tile_color + (size_t)local_y * tile_stride + local_x0 * samples;
        uint16_t *depth = worker->tile_depth
            ? worker->tile_depth + (size_t)local_y * tile_stride + local_x0 * samples
            : NULL;

        for (int32_t x = x0; x <= x1; ++x) {
            if (gpu_all_samples_covered_i32(primitive, e0, e1, e2)) {
                for (uint32_t sample = 0U; sample < samples; ++sample) {
                    if (gpu_depth_less_write_sample(depth, color, sample,
                                                    depth_fp + primitive->sample_depth_bias_fp[sample],
                                                    packed_color)) {
                        wrote = true;
                        *out_depth_dirty = true;
                    }
                }
            } else {
                for (uint32_t sample = 0U; sample < samples; ++sample) {
                    if (gpu_sample_covered_i32(primitive, sample, e0, e1, e2) &&
                        gpu_depth_less_write_sample(depth, color, sample,
                                                    depth_fp + primitive->sample_depth_bias_fp[sample],
                                                    packed_color)) {
                        wrote = true;
                        *out_depth_dirty = true;
                    }
                }
            }
            color += samples;
            depth += samples;
            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
            depth_fp += depth_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
        row_depth += depth_step_y;
    }
    return wrote;
}

static bool gpu_raster_tile_depth_less_write_4x_i32(grape_gpu_worker_t *worker,
                                                     const grape_gpu_prepared_triangle_t *primitive,
                                                     int32_t x0,
                                                     int32_t y0,
                                                     int32_t x1,
                                                     int32_t y1,
                                                     uint32_t packed_color,
                                                     bool *out_depth_dirty)
{
    const grape_gpu_triangle_setup_t *setup = &primitive->setup;
    const int32_t dx0 = x0 - setup->min_x;
    const int32_t dy0 = y0 - setup->min_y;
    int32_t row_e0 = (int32_t)((int64_t)primitive->row_e[0] +
                               (int64_t)primitive->edge_step_x[0] * dx0 +
                               (int64_t)primitive->edge_step_y[0] * dy0);
    int32_t row_e1 = (int32_t)((int64_t)primitive->row_e[1] +
                               (int64_t)primitive->edge_step_x[1] * dx0 +
                               (int64_t)primitive->edge_step_y[1] * dy0);
    int32_t row_e2 = (int32_t)((int64_t)primitive->row_e[2] +
                               (int64_t)primitive->edge_step_x[2] * dx0 +
                               (int64_t)primitive->edge_step_y[2] * dy0);
    int32_t row_depth = (int32_t)((int64_t)primitive->depth_row_start_fp +
                                  (int64_t)primitive->depth_step_x_fp * dx0 +
                                  (int64_t)primitive->depth_step_y_fp * dy0);
    const int32_t e0_step_x = primitive->edge_step_x[0];
    const int32_t e1_step_x = primitive->edge_step_x[1];
    const int32_t e2_step_x = primitive->edge_step_x[2];
    const int32_t e0_step_y = primitive->edge_step_y[0];
    const int32_t e1_step_y = primitive->edge_step_y[1];
    const int32_t e2_step_y = primitive->edge_step_y[2];
    const int32_t depth_step_x = primitive->depth_step_x_fp;
    const int32_t depth_step_y = primitive->depth_step_y_fp;
    const int32_t depth_bias0 = primitive->sample_depth_bias_fp[0];
    const int32_t depth_bias1 = primitive->sample_depth_bias_fp[1];
    const int32_t depth_bias2 = primitive->sample_depth_bias_fp[2];
    const int32_t depth_bias3 = primitive->sample_depth_bias_fp[3];
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int32_t e0 = row_e0;
        int32_t e1 = row_e1;
        int32_t e2 = row_e2;
        int32_t depth_fp = row_depth;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        const uint32_t local_x0 = (uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = worker->tile_color +
            (size_t)local_y * (GRAPE_GPU_MSAA_TILE_SIZE * 4U) + local_x0 * 4U;
        uint16_t *depth = worker->tile_depth +
            (size_t)local_y * (GRAPE_GPU_MSAA_TILE_SIZE * 4U) + local_x0 * 4U;

        for (int32_t x = x0; x <= x1; ++x) {
            bool pixel_wrote = false;
            if (gpu_all_samples_covered_i32(primitive, e0, e1, e2)) {
                pixel_wrote |= gpu_depth_less_write_sample(depth, color, 0U, depth_fp + depth_bias0, packed_color);
                pixel_wrote |= gpu_depth_less_write_sample(depth, color, 1U, depth_fp + depth_bias1, packed_color);
                pixel_wrote |= gpu_depth_less_write_sample(depth, color, 2U, depth_fp + depth_bias2, packed_color);
                pixel_wrote |= gpu_depth_less_write_sample(depth, color, 3U, depth_fp + depth_bias3, packed_color);
            } else {
                if (gpu_sample_covered_i32(primitive, 0U, e0, e1, e2)) {
                    pixel_wrote |= gpu_depth_less_write_sample(depth, color, 0U, depth_fp + depth_bias0, packed_color);
                }
                if (gpu_sample_covered_i32(primitive, 1U, e0, e1, e2)) {
                    pixel_wrote |= gpu_depth_less_write_sample(depth, color, 1U, depth_fp + depth_bias1, packed_color);
                }
                if (gpu_sample_covered_i32(primitive, 2U, e0, e1, e2)) {
                    pixel_wrote |= gpu_depth_less_write_sample(depth, color, 2U, depth_fp + depth_bias2, packed_color);
                }
                if (gpu_sample_covered_i32(primitive, 3U, e0, e1, e2)) {
                    pixel_wrote |= gpu_depth_less_write_sample(depth, color, 3U, depth_fp + depth_bias3, packed_color);
                }
            }
            if (pixel_wrote) {
                wrote = true;
                *out_depth_dirty = true;
            }
            color += 4U;
            depth += 4U;
            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
            depth_fp += depth_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
        row_depth += depth_step_y;
    }
    return wrote;
}

static bool gpu_raster_tile_depth_generic_i32(grape_gpu_context_t *context,
                                               grape_gpu_worker_t *worker,
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
    int32_t row_e0 = (int32_t)((int64_t)primitive->row_e[0] +
                               (int64_t)primitive->edge_step_x[0] * dx0 +
                               (int64_t)primitive->edge_step_y[0] * dy0);
    int32_t row_e1 = (int32_t)((int64_t)primitive->row_e[1] +
                               (int64_t)primitive->edge_step_x[1] * dx0 +
                               (int64_t)primitive->edge_step_y[1] * dy0);
    int32_t row_e2 = (int32_t)((int64_t)primitive->row_e[2] +
                               (int64_t)primitive->edge_step_x[2] * dx0 +
                               (int64_t)primitive->edge_step_y[2] * dy0);
    int32_t row_depth = (int32_t)((int64_t)primitive->depth_row_start_fp +
                                  (int64_t)primitive->depth_step_x_fp * dx0 +
                                  (int64_t)primitive->depth_step_y_fp * dy0);
    const int32_t e0_step_x = primitive->edge_step_x[0];
    const int32_t e1_step_x = primitive->edge_step_x[1];
    const int32_t e2_step_x = primitive->edge_step_x[2];
    const int32_t e0_step_y = primitive->edge_step_y[0];
    const int32_t e1_step_y = primitive->edge_step_y[1];
    const int32_t e2_step_y = primitive->edge_step_y[2];
    const int32_t depth_step_x = primitive->depth_step_x_fp;
    const int32_t depth_step_y = primitive->depth_step_y_fp;
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int32_t e0 = row_e0;
        int32_t e1 = row_e1;
        int32_t e2 = row_e2;
        int32_t depth_fp = row_depth;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        const uint32_t local_x0 = (uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = worker->tile_color + (size_t)local_y * tile_stride + local_x0 * samples;
        uint16_t *depth = worker->tile_depth
            ? worker->tile_depth + (size_t)local_y * tile_stride + local_x0 * samples
            : NULL;

        for (int32_t x = x0; x <= x1; ++x) {
            const bool all_covered = gpu_all_samples_covered_i32(primitive, e0, e1, e2);
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (all_covered || gpu_sample_covered_i32(primitive, sample, e0, e1, e2)) {
                    const uint16_t incoming = gpu_depth_fp_to_d16(
                        depth_fp + primitive->sample_depth_bias_fp[sample]
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
            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
            depth_fp += depth_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
        row_depth += depth_step_y;
    }
    return wrote;
}

static esp_err_t gpu_raster_tile_shaded_fallback(grape_gpu_context_t *context,
                                                  grape_gpu_worker_t *worker,
                                                  const grape_gpu_prepared_triangle_t *primitive,
                                                  int32_t x0,
                                                  int32_t y0,
                                                  int32_t x1,
                                                  int32_t y1,
                                                  bool *out_wrote,
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
    const grape_gpu_depth_state_t depth_state = primitive->depth;
    bool wrote = false;

    for (int32_t y = y0; y <= y1; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        float depth_f = row_depth;
        const uint32_t local_y = (uint32_t)y & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        const uint32_t local_x0 = (uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U);
        uint32_t *color = worker->tile_color + (size_t)local_y * tile_stride + local_x0 * samples;
        uint16_t *depth = worker->tile_depth
            ? worker->tile_depth + (size_t)local_y * tile_stride + local_x0 * samples
            : NULL;

        for (int32_t x = x0; x <= x1; ++x) {
            uint32_t coverage_mask = 0U;
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + primitive->sample_e0_bias_fallback[sample] >= 0 &&
                    e1 + primitive->sample_e1_bias_fallback[sample] >= 0 &&
                    e2 + primitive->sample_e2_bias_fallback[sample] >= 0) {
                    coverage_mask |= 1U << sample;
                }
            }

            if (coverage_mask != 0U) {
                uint32_t pass_mask = 0U;
                uint16_t incoming[4] = {0U, 0U, 0U, 0U};
                for (uint32_t sample = 0U; sample < samples; ++sample) {
                    const uint32_t bit = 1U << sample;
                    if ((coverage_mask & bit) == 0U) {
                        continue;
                    }
                    incoming[sample] = gpu_depth_to_d16_tile(
                        depth_f + primitive->sample_depth_bias_fallback[sample]
                    );
                    if (!depth_state.test_enable ||
                        gpu_depth_compare_tile(depth_state.compare_op, incoming[sample], depth[sample])) {
                        pass_mask |= bit;
                    }
                }

                if (pass_mask != 0U) {
                    float centroid_x = 0.0f;
                    float centroid_y = 0.0f;
                    gpu_coverage_centroid(
                        coverage_mask, context->sample_count, &centroid_x, &centroid_y
                    );
                    grape_color_t shaded;
                    esp_err_t ret = grape_gpu_shade_fragment(
                        setup,
                        primitive->fragment_program,
                        primitive->texture,
                        &primitive->sampler,
                        (float)x + 0.5f + centroid_x,
                        (float)y + 0.5f + centroid_y,
                        &shaded
                    );
                    if (ret != ESP_OK) {
                        return ret;
                    }
                    const uint32_t packed = gpu_pack_color(shaded);
                    for (uint32_t sample = 0U; sample < samples; ++sample) {
                        const uint32_t bit = 1U << sample;
                        if ((pass_mask & bit) == 0U) {
                            continue;
                        }
                        if (depth_state.write_enable) {
                            depth[sample] = incoming[sample];
                            *out_depth_dirty = true;
                        }
                        color[sample] = packed;
                    }
                    wrote = true;
                }
            }

            color += samples;
            if (depth) depth += samples;
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

    *out_wrote = wrote;
    return ESP_OK;
}

static bool gpu_raster_tile_color_only_fallback(grape_gpu_context_t *context,
                                                 grape_gpu_worker_t *worker,
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
        uint32_t *color = worker->tile_color +
            (size_t)local_y * tile_stride +
            ((uint32_t)x0 & (GRAPE_GPU_MSAA_TILE_SIZE - 1U)) * samples;

        for (int32_t x = x0; x <= x1; ++x) {
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + primitive->sample_e0_bias_fallback[sample] >= 0 &&
                    e1 + primitive->sample_e1_bias_fallback[sample] >= 0 &&
                    e2 + primitive->sample_e2_bias_fallback[sample] >= 0) {
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

static bool gpu_raster_tile_depth_less_write_fallback(grape_gpu_context_t *context,
                                                        grape_gpu_worker_t *worker,
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
        uint32_t *color = worker->tile_color + (size_t)local_y * tile_stride + local_x0 * samples;
        uint16_t *depth = worker->tile_depth
            ? worker->tile_depth + (size_t)local_y * tile_stride + local_x0 * samples
            : NULL;

        for (int32_t x = x0; x <= x1; ++x) {
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + primitive->sample_e0_bias_fallback[sample] >= 0 &&
                    e1 + primitive->sample_e1_bias_fallback[sample] >= 0 &&
                    e2 + primitive->sample_e2_bias_fallback[sample] >= 0) {
                    const uint16_t incoming = gpu_depth_to_d16_tile(
                        depth_f + primitive->sample_depth_bias_fallback[sample]
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

static bool gpu_raster_tile_depth_generic_fallback(grape_gpu_context_t *context,
                                                     grape_gpu_worker_t *worker,
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
        uint32_t *color = worker->tile_color + (size_t)local_y * tile_stride + local_x0 * samples;
        uint16_t *depth = worker->tile_depth
            ? worker->tile_depth + (size_t)local_y * tile_stride + local_x0 * samples
            : NULL;

        for (int32_t x = x0; x <= x1; ++x) {
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + primitive->sample_e0_bias_fallback[sample] >= 0 &&
                    e1 + primitive->sample_e1_bias_fallback[sample] >= 0 &&
                    e2 + primitive->sample_e2_bias_fallback[sample] >= 0) {
                    const uint16_t incoming = gpu_depth_to_d16_tile(
                        depth_f + primitive->sample_depth_bias_fallback[sample]
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
                             grape_gpu_worker_t *worker,
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
        const uint32_t *samples_row = worker->tile_color + (size_t)y * tile_stride;
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
                                 grape_gpu_worker_t *worker,
                                 const grape_gpu_tile_job_t *job)
{
    const uint32_t tile_x = job->tile_x;
    const uint32_t tile_y = job->tile_y;
    const uint32_t ref_begin = job->ref_begin;
    const uint32_t ref_end = job->ref_end;
    const uint32_t x0 = tile_x * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t y0 = tile_y * GRAPE_GPU_MSAA_TILE_SIZE;
    const uint32_t tile_width = x0 + GRAPE_GPU_MSAA_TILE_SIZE <= context->color_attachment->width
        ? GRAPE_GPU_MSAA_TILE_SIZE
        : context->color_attachment->width - x0;
    const uint32_t tile_height = y0 + GRAPE_GPU_MSAA_TILE_SIZE <= context->color_attachment->height
        ? GRAPE_GPU_MSAA_TILE_SIZE
        : context->color_attachment->height - y0;
    const uint32_t depth_stride_values = GRAPE_GPU_MSAA_TILE_SIZE * (uint32_t)context->sample_count;

    gpu_init_color_tile(context, worker, tile_x, tile_y, tile_width, tile_height);

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
                                  worker->tile_depth,
                                  depth_stride_values);
    }

    bool color_dirty = false;
    bool depth_dirty = false;
    const bool profile = context->stats_enabled;
    const int64_t raster_start_us = profile ? esp_timer_get_time() : 0;
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
            const bool four_x = context->sample_count == GRAPE_GPU_SAMPLE_COUNT_4;
            bool wrote = false;
            if (gpu_fragment_is_shaded_tile(primitive->fragment_program)) {
                esp_err_t shaded_ret = primitive->raster_i32_valid
                    ? gpu_raster_tile_shaded_i32(
                        context, worker, primitive, rx0, ry0, rx1, ry1, &wrote, &depth_dirty
                    )
                    : gpu_raster_tile_shaded_fallback(
                        context, worker, primitive, rx0, ry0, rx1, ry1, &wrote, &depth_dirty
                    );
                if (shaded_ret != ESP_OK) {
                    return shaded_ret;
                }
            } else if (!primitive->depth.test_enable && !primitive->depth.write_enable) {
                if (primitive->raster_i32_valid) {
                    wrote = four_x
                        ? gpu_raster_tile_color_only_4x_i32(
                            worker, primitive, rx0, ry0, rx1, ry1, packed
                        )
                        : gpu_raster_tile_color_only_i32(
                            context, worker, primitive, rx0, ry0, rx1, ry1, packed
                        );
                } else {
                    wrote = gpu_raster_tile_color_only_fallback(
                        context, worker, primitive, rx0, ry0, rx1, ry1, packed
                    );
                }
            } else if (primitive->depth.test_enable && primitive->depth.write_enable &&
                       primitive->depth.compare_op == GRAPE_GPU_COMPARE_LESS) {
                if (primitive->raster_i32_valid) {
                    wrote = four_x
                        ? gpu_raster_tile_depth_less_write_4x_i32(
                            worker, primitive, rx0, ry0, rx1, ry1, packed, &depth_dirty
                        )
                        : gpu_raster_tile_depth_less_write_i32(
                            context, worker, primitive, rx0, ry0, rx1, ry1, packed, &depth_dirty
                        );
                } else {
                    wrote = gpu_raster_tile_depth_less_write_fallback(
                        context, worker, primitive, rx0, ry0, rx1, ry1, packed, &depth_dirty
                    );
                }
            } else {
                wrote = primitive->raster_i32_valid
                    ? gpu_raster_tile_depth_generic_i32(
                        context, worker, primitive, rx0, ry0, rx1, ry1, packed, &depth_dirty
                    )
                    : gpu_raster_tile_depth_generic_fallback(
                        context, worker, primitive, rx0, ry0, rx1, ry1, packed, &depth_dirty
                    );
            }
            color_dirty |= wrote;
        }
    }
    if (profile) {
        worker->tile_raster_us +=
            (uint64_t)(esp_timer_get_time() - raster_start_us);
    }

    if (depth_dirty) {
        grape_gpu_depth_store_tile(context->depth_attachment,
                                   tile_x,
                                   tile_y,
                                   worker->tile_depth,
                                   depth_stride_values);
    }

    if (color_dirty) {
        const int64_t resolve_start_us = profile ? esp_timer_get_time() : 0;
        gpu_resolve_tile(context, worker, tile_x, tile_y, tile_width, tile_height);
        if (profile) {
            worker->resolve_us +=
                (uint64_t)(esp_timer_get_time() - resolve_start_us);
        }
        const grape_rect_t dirty = {
            .x = (int32_t)x0,
            .y = (int32_t)y0,
            .width = (int32_t)tile_width,
            .height = (int32_t)tile_height,
        };
        worker->dirty_rect = worker->dirty_valid
            ? grape_rect_union(worker->dirty_rect, dirty) : dirty;
        worker->dirty_valid = true;
    }
    return ESP_OK;
}

static esp_err_t gpu_worker_drain_jobs(grape_gpu_context_t *context,
                                       grape_gpu_worker_t *worker)
{
    grape_gpu_tile_job_t job;
    esp_err_t ret = ESP_OK;
    while (gpu_claim_tile_job(context, &job)) {
        ret = gpu_render_tile(context, worker, &job);
        if (ret != ESP_OK) {
            __atomic_store_n(&context->tile_jobs_cancelled, true, __ATOMIC_RELAXED);
            break;
        }
    }
    return ret;
}

static void gpu_worker_reset_batch(grape_gpu_worker_t *worker)
{
    worker->tile_raster_us = 0U;
    worker->resolve_us = 0U;
    worker->dirty_valid = false;
    worker->result = ESP_OK;
}

static void gpu_worker_merge_batch(grape_gpu_context_t *context,
                                    const grape_gpu_worker_t *worker)
{
    context->current_stats.tile_raster_us += worker->tile_raster_us;
    context->current_stats.resolve_us += worker->resolve_us;
    if (worker->dirty_valid) {
        grape_gpu_dirty_add(context, worker->dirty_rect);
    }
}

#if GRAPE_GPU_MULTICORE
#define GPU_SECONDARY_STACK_BYTES 6144U
#define GPU_SECONDARY_PRIORITY 1U

_Static_assert(__atomic_always_lock_free(sizeof(size_t), 0),
               "GPU job claims require lock-free native atomics");

static void gpu_secondary_task(void *arg)
{
    grape_gpu_context_t *context = arg;
    const SemaphoreHandle_t wake = context->secondary_wake;
    const SemaphoreHandle_t done = context->secondary_done;
    for (;;) {
        xSemaphoreTake(wake, portMAX_DELAY);
        if (context->secondary_stop) {
            /* Last context access. The owner may free it after this ack. */
            xSemaphoreGive(done);
            vTaskDelete(NULL);
            return;
        }
        context->secondary_worker.result =
            gpu_worker_drain_jobs(context, &context->secondary_worker);
        xSemaphoreGive(done);
    }
}

static bool gpu_secondary_prepare(grape_gpu_context_t *context)
{
    /* Allocation is on CPU0, before publishing the batch. Failure leaves a
     * usable single-worker renderer; a later batch may retry. */
    if (gpu_ensure_worker_workspace(context, &context->secondary_worker) != ESP_OK) {
        return false;
    }
    if (context->secondary_task) {
        return true;
    }
    context->secondary_wake = xSemaphoreCreateBinary();
    context->secondary_done = xSemaphoreCreateBinary();
    if (context->secondary_wake && context->secondary_done &&
        xTaskCreatePinnedToCore(gpu_secondary_task, "grape_gpu1",
                               GPU_SECONDARY_STACK_BYTES, context,
                               GPU_SECONDARY_PRIORITY, &context->secondary_task, 1) == pdPASS) {
        return true;
    }
    if (context->secondary_wake) {
        vSemaphoreDelete(context->secondary_wake);
    }
    if (context->secondary_done) {
        vSemaphoreDelete(context->secondary_done);
    }
    context->secondary_wake = NULL;
    context->secondary_done = NULL;
    context->secondary_task = NULL;
    gpu_worker_release(&context->secondary_worker);
    return false;
}

static void gpu_secondary_release(grape_gpu_context_t *context)
{
    if (context->secondary_task) {
        /* Every execute, including errors, joins before returning. Therefore
         * no batch is in flight here and this ack belongs only to shutdown. */
        context->secondary_stop = true;
        xSemaphoreGive(context->secondary_wake);
        xSemaphoreTake(context->secondary_done, portMAX_DELAY);
        vSemaphoreDelete(context->secondary_wake);
        vSemaphoreDelete(context->secondary_done);
        context->secondary_task = NULL;
        context->secondary_wake = NULL;
        context->secondary_done = NULL;
        context->secondary_stop = false;
    }
    gpu_worker_release(&context->secondary_worker);
}
#endif

static void gpu_release_primitive_textures(grape_gpu_context_t *context)
{
    if (!context) {
        return;
    }
    for (size_t i = 0U; i < context->tile_primitive_count; ++i) {
        grape_texture_t *texture = context->tile_primitives[i].texture;
        if (texture && texture->ref_count != 0U) {
            texture->ref_count--;
        }
        context->tile_primitives[i].texture = NULL;
    }
}

esp_err_t grape_gpu_tile_execute(grape_gpu_context_t *context)
{
    GRAPE_TIME_SCOPE(GPU_TILE_EXECUTE);
    if (!context || context->sample_count == GRAPE_GPU_SAMPLE_COUNT_1) {
        return ESP_ERR_INVALID_STATE;
    }
    if (context->tile_primitive_count == 0U) {
        return ESP_OK;
    }

    esp_err_t ret = gpu_build_bins(context);
    if (ret == ESP_OK) {
        ret = gpu_build_jobs(context);
    }
    if (ret != ESP_OK) {
        gpu_release_primitive_textures(context);
        context->tile_primitive_count = 0U;
        context->tile_job_count = 0U;
        context->tile_job_next = 0U;
        return ret;
    }

    gpu_worker_reset_batch(&context->primary_worker);
    __atomic_store_n(&context->tile_job_next, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&context->tile_jobs_cancelled, false, __ATOMIC_RELAXED);
#if GRAPE_GPU_MULTICORE
    const bool use_secondary = context->tile_job_count > 1U && gpu_secondary_prepare(context);
    if (use_secondary) {
        gpu_worker_reset_batch(&context->secondary_worker);
        xSemaphoreGive(context->secondary_wake);
    }
#endif
    ret = gpu_worker_drain_jobs(context, &context->primary_worker);
#if GRAPE_GPU_MULTICORE
    if (use_secondary) {
        /* Queue exhaustion is not completion: Core 1 may still own a tile.
         * Block even on error, before merging, resetting, or releasing inputs. */
        xSemaphoreTake(context->secondary_done, portMAX_DELAY);
        if (ret == ESP_OK) {
            ret = context->secondary_worker.result;
        }
        gpu_worker_merge_batch(context, &context->secondary_worker);
    }
#endif
    gpu_worker_merge_batch(context, &context->primary_worker);

    gpu_release_primitive_textures(context);
    context->tile_primitive_count = 0U;
    context->tile_job_count = 0U;
    context->tile_job_next = 0U;
    return ret;
}

void grape_gpu_tile_release(grape_gpu_context_t *context)
{
    if (!context) {
        return;
    }

#if GRAPE_GPU_MULTICORE
    gpu_secondary_release(context);
#endif
    gpu_release_primitive_textures(context);
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
    free(context->tile_jobs);
    context->tile_jobs = NULL;
    context->tile_job_count = 0U;
    context->tile_job_capacity = 0U;
    context->tile_job_next = 0U;
    gpu_worker_release(&context->primary_worker);
    context->tile_cols = 0U;
    context->tile_rows = 0U;
}
