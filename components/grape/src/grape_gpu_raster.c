#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "grape_gpu_internal.h"
#include "grape_internal.h"

#define GRAPE_GPU_SUBPIXEL_BITS 3
#define GRAPE_GPU_SUBPIXEL_SCALE (1 << GRAPE_GPU_SUBPIXEL_BITS)
#define GRAPE_GPU_SUBPIXEL_HALF (GRAPE_GPU_SUBPIXEL_SCALE / 2)

typedef struct {
    int32_t x;
    int32_t y;
} grape_gpu_fixed_point_t;

typedef struct {
    grape_gpu_fixed_point_t fixed[3];
    float sx[3];
    float sy[3];
    float depth[3];
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
    int64_t row_e0;
    int64_t row_e1;
    int64_t row_e2;
    int64_t e0_step_x;
    int64_t e1_step_x;
    int64_t e2_step_x;
    int64_t e0_step_y;
    int64_t e1_step_y;
    int64_t e2_step_y;
    float depth_row_start;
    float depth_step_x;
    float depth_step_y;
} gpu_triangle_setup_t;

static bool gpu_clip_to_screen(const grape_gpu_viewport_t *viewport,
                               const grape_gpu_clip_vertex_t *vertex,
                               float *out_x,
                               float *out_y,
                               float *out_depth)
{
    if (vertex->w <= FLT_EPSILON) {
        return false;
    }

    const float inverse_w = 1.0f / vertex->w;
    const float ndc_x = vertex->x * inverse_w;
    const float ndc_y = vertex->y * inverse_w;
    const float ndc_z = vertex->z * inverse_w;
    if (!isfinite(ndc_x) || !isfinite(ndc_y) || !isfinite(ndc_z)) {
        return false;
    }

    const float screen_x = viewport->x + (ndc_x * 0.5f + 0.5f) * viewport->width;
    const float screen_y = viewport->y + (0.5f - ndc_y * 0.5f) * viewport->height;
    const float depth = viewport->min_depth +
                        ndc_z * (viewport->max_depth - viewport->min_depth);
    if (!isfinite(screen_x) || !isfinite(screen_y) || !isfinite(depth)) {
        return false;
    }

    *out_x = screen_x;
    *out_y = screen_y;
    *out_depth = depth;
    return true;
}

static bool gpu_float_to_fixed(float value, int32_t *out_value)
{
    const double scaled = (double)value * (double)GRAPE_GPU_SUBPIXEL_SCALE;
    if (scaled < (double)INT32_MIN + 1.0 || scaled > (double)INT32_MAX - 1.0) {
        return false;
    }

    *out_value = (int32_t)llround(scaled);
    return true;
}

static int64_t gpu_edge(grape_gpu_fixed_point_t a,
                        grape_gpu_fixed_point_t b,
                        int64_t x,
                        int64_t y)
{
    const int64_t dx = (int64_t)b.x - a.x;
    const int64_t dy = (int64_t)b.y - a.y;
    return dx * (y - a.y) - dy * (x - a.x);
}

static bool gpu_edge_is_top_left(grape_gpu_fixed_point_t a,
                                 grape_gpu_fixed_point_t b)
{
    const int32_t dx = b.x - a.x;
    const int32_t dy = b.y - a.y;
    return dy < 0 || (dy == 0 && dx > 0);
}

static int32_t gpu_floor_to_i32_safe(float value)
{
    if (value <= (float)INT32_MIN) {
        return INT32_MIN;
    }
    if (value >= (float)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)floorf(value);
}

static int32_t gpu_ceil_to_i32_safe(float value)
{
    if (value <= (float)INT32_MIN) {
        return INT32_MIN;
    }
    if (value >= (float)INT32_MAX) {
        return INT32_MAX;
    }
    return (int32_t)ceilf(value);
}

static int32_t gpu_max_i32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static int32_t gpu_min_i32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static grape_color_t gpu_fragment_color(const grape_gpu_context_t *context)
{
    if (context->bound_pipeline->desc.fragment_program == GRAPE_GPU_FRAGMENT_PROGRAM_SOLID_COLOR) {
        return context->bound_pipeline->desc.solid_color;
    }

    grape_color_t color;
    memcpy(&color,
           context->push_constants + offsetof(grape_gpu_builtin_constants_t, color),
           sizeof(color));
    return color;
}

static uint16_t gpu_depth_to_d16_fast(float depth)
{
    if (depth <= 0.0f) {
        return 0U;
    }
    if (depth >= 65535.0f) {
        return UINT16_MAX;
    }
    return (uint16_t)(depth + 0.5f);
}

static bool gpu_setup_triangle(grape_gpu_context_t *context,
                               const grape_gpu_clip_vertex_t triangle[3],
                               gpu_triangle_setup_t *setup)
{
    for (size_t i = 0U; i < 3U; ++i) {
        if (!gpu_clip_to_screen(
                &context->viewport,
                &triangle[i],
                &setup->sx[i],
                &setup->sy[i],
                &setup->depth[i])) {
            return false;
        }
    }

    for (size_t i = 0U; i < 3U; ++i) {
        if (!gpu_float_to_fixed(setup->sx[i], &setup->fixed[i].x) ||
            !gpu_float_to_fixed(setup->sy[i], &setup->fixed[i].y)) {
            return false;
        }
    }

    int64_t area = gpu_edge(
        setup->fixed[0],
        setup->fixed[1],
        setup->fixed[2].x,
        setup->fixed[2].y
    );
    if (area == 0) {
        return false;
    }

    if (area < 0) {
        grape_gpu_fixed_point_t temp_fixed = setup->fixed[1];
        setup->fixed[1] = setup->fixed[2];
        setup->fixed[2] = temp_fixed;

        float temp = setup->sx[1];
        setup->sx[1] = setup->sx[2];
        setup->sx[2] = temp;
        temp = setup->sy[1];
        setup->sy[1] = setup->sy[2];
        setup->sy[2] = temp;
        temp = setup->depth[1];
        setup->depth[1] = setup->depth[2];
        setup->depth[2] = temp;
    }

    const float min_sx = fminf(setup->sx[0], fminf(setup->sx[1], setup->sx[2]));
    const float max_sx = fmaxf(setup->sx[0], fmaxf(setup->sx[1], setup->sx[2]));
    const float min_sy = fminf(setup->sy[0], fminf(setup->sy[1], setup->sy[2]));
    const float max_sy = fmaxf(setup->sy[0], fmaxf(setup->sy[1], setup->sy[2]));

    setup->min_x = gpu_floor_to_i32_safe(min_sx);
    setup->max_x = gpu_ceil_to_i32_safe(max_sx) - 1;
    setup->min_y = gpu_floor_to_i32_safe(min_sy);
    setup->max_y = gpu_ceil_to_i32_safe(max_sy) - 1;

    const int32_t viewport_min_x = gpu_floor_to_i32_safe(context->viewport.x);
    const int32_t viewport_max_x = gpu_ceil_to_i32_safe(
        context->viewport.x + context->viewport.width
    ) - 1;
    const int32_t viewport_min_y = gpu_floor_to_i32_safe(context->viewport.y);
    const int32_t viewport_max_y = gpu_ceil_to_i32_safe(
        context->viewport.y + context->viewport.height
    ) - 1;

    grape_texture_t *target = context->color_attachment;
    setup->min_x = gpu_max_i32(setup->min_x, 0);
    setup->min_y = gpu_max_i32(setup->min_y, 0);
    setup->min_x = gpu_max_i32(setup->min_x, viewport_min_x);
    setup->min_y = gpu_max_i32(setup->min_y, viewport_min_y);
    setup->max_x = gpu_min_i32(setup->max_x, (int32_t)target->width - 1);
    setup->max_y = gpu_min_i32(setup->max_y, (int32_t)target->height - 1);
    setup->max_x = gpu_min_i32(setup->max_x, viewport_max_x);
    setup->max_y = gpu_min_i32(setup->max_y, viewport_max_y);

    if (setup->min_x > setup->max_x || setup->min_y > setup->max_y) {
        return false;
    }

    const int64_t bias0 = gpu_edge_is_top_left(setup->fixed[1], setup->fixed[2]) ? 0 : -1;
    const int64_t bias1 = gpu_edge_is_top_left(setup->fixed[2], setup->fixed[0]) ? 0 : -1;
    const int64_t bias2 = gpu_edge_is_top_left(setup->fixed[0], setup->fixed[1]) ? 0 : -1;

    const int64_t sample_x =
        (int64_t)setup->min_x * GRAPE_GPU_SUBPIXEL_SCALE + GRAPE_GPU_SUBPIXEL_HALF;
    const int64_t sample_y =
        (int64_t)setup->min_y * GRAPE_GPU_SUBPIXEL_SCALE + GRAPE_GPU_SUBPIXEL_HALF;

    setup->row_e0 = gpu_edge(setup->fixed[1], setup->fixed[2], sample_x, sample_y) + bias0;
    setup->row_e1 = gpu_edge(setup->fixed[2], setup->fixed[0], sample_x, sample_y) + bias1;
    setup->row_e2 = gpu_edge(setup->fixed[0], setup->fixed[1], sample_x, sample_y) + bias2;

    setup->e0_step_x = -((int64_t)setup->fixed[2].y - setup->fixed[1].y) * GRAPE_GPU_SUBPIXEL_SCALE;
    setup->e1_step_x = -((int64_t)setup->fixed[0].y - setup->fixed[2].y) * GRAPE_GPU_SUBPIXEL_SCALE;
    setup->e2_step_x = -((int64_t)setup->fixed[1].y - setup->fixed[0].y) * GRAPE_GPU_SUBPIXEL_SCALE;
    setup->e0_step_y = ((int64_t)setup->fixed[2].x - setup->fixed[1].x) * GRAPE_GPU_SUBPIXEL_SCALE;
    setup->e1_step_y = ((int64_t)setup->fixed[0].x - setup->fixed[2].x) * GRAPE_GPU_SUBPIXEL_SCALE;
    setup->e2_step_y = ((int64_t)setup->fixed[1].x - setup->fixed[0].x) * GRAPE_GPU_SUBPIXEL_SCALE;

    const float dx10 = setup->sx[1] - setup->sx[0];
    const float dy10 = setup->sy[1] - setup->sy[0];
    const float dx20 = setup->sx[2] - setup->sx[0];
    const float dy20 = setup->sy[2] - setup->sy[0];
    const float dz10 = setup->depth[1] - setup->depth[0];
    const float dz20 = setup->depth[2] - setup->depth[0];
    const float area_float = dx10 * dy20 - dy10 * dx20;
    if (!isfinite(area_float) || fabsf(area_float) <= FLT_EPSILON) {
        return false;
    }

    const float inverse_area = 1.0f / area_float;
    setup->depth_step_x = (dz10 * dy20 - dz20 * dy10) * inverse_area * 65535.0f;
    setup->depth_step_y = (dx10 * dz20 - dx20 * dz10) * inverse_area * 65535.0f;
    setup->depth_row_start =
        (setup->depth[0] * 65535.0f) +
        setup->depth_step_x * (((float)setup->min_x + 0.5f) - setup->sx[0]) +
        setup->depth_step_y * (((float)setup->min_y + 0.5f) - setup->sy[0]);

    return true;
}

static esp_err_t gpu_rasterize_color_only(grape_gpu_context_t *context,
                                           const gpu_triangle_setup_t *setup,
                                           grape_color_t color)
{
    grape_texture_t *target = context->color_attachment;
    const int32_t min_x = setup->min_x;
    const int32_t max_x = setup->max_x;
    const int32_t min_y = setup->min_y;
    const int32_t max_y = setup->max_y;
    const int64_t e0_step_x = setup->e0_step_x;
    const int64_t e1_step_x = setup->e1_step_x;
    const int64_t e2_step_x = setup->e2_step_x;
    const int64_t e0_step_y = setup->e0_step_y;
    const int64_t e1_step_y = setup->e1_step_y;
    const int64_t e2_step_y = setup->e2_step_y;
    const size_t target_stride = target->stride;
    bool wrote_pixel = false;
    int64_t row_e0 = setup->row_e0;
    int64_t row_e1 = setup->row_e1;
    int64_t row_e2 = setup->row_e2;

    for (int32_t y = min_y; y <= max_y; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        uint8_t *row = target->pixels + (size_t)y * target_stride;

        for (int32_t x = min_x; x <= max_x; ++x) {
            if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
                uint8_t *pixel = row + (size_t)x * 4U;
                pixel[0] = color.r;
                pixel[1] = color.g;
                pixel[2] = color.b;
                pixel[3] = color.a;
                wrote_pixel = true;
            }
            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
    }

    if (wrote_pixel) {
        grape_gpu_dirty_add(context, (grape_rect_t) {
            .x = min_x,
            .y = min_y,
            .width = max_x - min_x + 1,
            .height = max_y - min_y + 1,
        });
    }

    return ESP_OK;
}

static bool gpu_depth_compare(grape_gpu_compare_op_t op, uint16_t incoming, uint16_t stored)
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

static esp_err_t gpu_rasterize_depth_generic(grape_gpu_context_t *context,
                                              const gpu_triangle_setup_t *setup,
                                              grape_color_t color)
{
    grape_texture_t *target = context->color_attachment;
    grape_gpu_depth_buffer_t *depth_target = context->depth_attachment;
    const grape_gpu_depth_state_t depth_state = context->bound_pipeline->desc.depth;
    const int32_t min_x = setup->min_x;
    const int32_t max_x = setup->max_x;
    const int32_t min_y = setup->min_y;
    const int32_t max_y = setup->max_y;
    const int64_t e0_step_x = setup->e0_step_x;
    const int64_t e1_step_x = setup->e1_step_x;
    const int64_t e2_step_x = setup->e2_step_x;
    const int64_t e0_step_y = setup->e0_step_y;
    const int64_t e1_step_y = setup->e1_step_y;
    const int64_t e2_step_y = setup->e2_step_y;
    const float depth_step_x = setup->depth_step_x;
    const float depth_step_y = setup->depth_step_y;
    const size_t target_stride = target->stride;
    const size_t depth_stride = depth_target->stride;
    bool wrote_pixel = false;
    int64_t row_e0 = setup->row_e0;
    int64_t row_e1 = setup->row_e1;
    int64_t row_e2 = setup->row_e2;
    float row_depth = setup->depth_row_start;

    for (int32_t y = min_y; y <= max_y; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        float depth_f = row_depth;
        uint8_t *color_row = target->pixels + (size_t)y * target_stride;
        uint16_t *depth_row = (uint16_t *)((uint8_t *)depth_target->data + (size_t)y * depth_stride);

        for (int32_t x = min_x; x <= max_x; ++x) {
            if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
                const uint16_t incoming_depth = gpu_depth_to_d16_fast(depth_f);
                const bool depth_pass = !depth_state.test_enable ||
                    gpu_depth_compare(depth_state.compare_op, incoming_depth, depth_row[x]);
                if (depth_pass) {
                    if (depth_state.write_enable) {
                        depth_row[x] = incoming_depth;
                    }
                    uint8_t *pixel = color_row + (size_t)x * 4U;
                    pixel[0] = color.r;
                    pixel[1] = color.g;
                    pixel[2] = color.b;
                    pixel[3] = color.a;
                    wrote_pixel = true;
                }
            }
            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
            depth_f += depth_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
        row_depth += depth_step_y;
    }

    if (wrote_pixel) {
        grape_gpu_dirty_add(context, (grape_rect_t) {
            .x = min_x,
            .y = min_y,
            .width = max_x - min_x + 1,
            .height = max_y - min_y + 1,
        });
    }

    return ESP_OK;
}

static esp_err_t gpu_rasterize_depth_less_write(grape_gpu_context_t *context,
                                                 const gpu_triangle_setup_t *setup,
                                                 grape_color_t color)
{
    grape_texture_t *target = context->color_attachment;
    grape_gpu_depth_buffer_t *depth_target = context->depth_attachment;
    const int32_t min_x = setup->min_x;
    const int32_t max_x = setup->max_x;
    const int32_t min_y = setup->min_y;
    const int32_t max_y = setup->max_y;
    const int64_t e0_step_x = setup->e0_step_x;
    const int64_t e1_step_x = setup->e1_step_x;
    const int64_t e2_step_x = setup->e2_step_x;
    const int64_t e0_step_y = setup->e0_step_y;
    const int64_t e1_step_y = setup->e1_step_y;
    const int64_t e2_step_y = setup->e2_step_y;
    const float depth_step_x = setup->depth_step_x;
    const float depth_step_y = setup->depth_step_y;
    const size_t target_stride = target->stride;
    const size_t depth_stride = depth_target->stride;
    bool wrote_pixel = false;
    int64_t row_e0 = setup->row_e0;
    int64_t row_e1 = setup->row_e1;
    int64_t row_e2 = setup->row_e2;
    float row_depth = setup->depth_row_start;

    for (int32_t y = min_y; y <= max_y; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        float depth_f = row_depth;
        uint8_t *color_row = target->pixels + (size_t)y * target_stride;
        uint16_t *depth_row = (uint16_t *)((uint8_t *)depth_target->data + (size_t)y * depth_stride);

        for (int32_t x = min_x; x <= max_x; ++x) {
            if (e0 >= 0 && e1 >= 0 && e2 >= 0) {
                const uint16_t incoming_depth = gpu_depth_to_d16_fast(depth_f);
                if (incoming_depth < depth_row[x]) {
                    depth_row[x] = incoming_depth;
                    uint8_t *pixel = color_row + (size_t)x * 4U;
                    pixel[0] = color.r;
                    pixel[1] = color.g;
                    pixel[2] = color.b;
                    pixel[3] = color.a;
                    wrote_pixel = true;
                }
            }
            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
            depth_f += depth_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
        row_depth += depth_step_y;
    }

    if (wrote_pixel) {
        grape_gpu_dirty_add(context, (grape_rect_t) {
            .x = min_x,
            .y = min_y,
            .width = max_x - min_x + 1,
            .height = max_y - min_y + 1,
        });
    }

    return ESP_OK;
}


typedef struct {
    int64_t e0[4];
    int64_t e1[4];
    int64_t e2[4];
    float depth[4];
} gpu_msaa_sample_bias_t;

static void gpu_msaa_sample_offsets(grape_gpu_sample_count_t sample_count,
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

static void gpu_msaa_build_biases(const gpu_triangle_setup_t *setup,
                                  grape_gpu_sample_count_t sample_count,
                                  gpu_msaa_sample_bias_t *bias)
{
    const int8_t *sample_x = NULL;
    const int8_t *sample_y = NULL;
    gpu_msaa_sample_offsets(sample_count, &sample_x, &sample_y);

    const int64_t e0_subpixel_x = setup->e0_step_x / GRAPE_GPU_SUBPIXEL_SCALE;
    const int64_t e1_subpixel_x = setup->e1_step_x / GRAPE_GPU_SUBPIXEL_SCALE;
    const int64_t e2_subpixel_x = setup->e2_step_x / GRAPE_GPU_SUBPIXEL_SCALE;
    const int64_t e0_subpixel_y = setup->e0_step_y / GRAPE_GPU_SUBPIXEL_SCALE;
    const int64_t e1_subpixel_y = setup->e1_step_y / GRAPE_GPU_SUBPIXEL_SCALE;
    const int64_t e2_subpixel_y = setup->e2_step_y / GRAPE_GPU_SUBPIXEL_SCALE;
    const float inverse_subpixel = 1.0f / (float)GRAPE_GPU_SUBPIXEL_SCALE;

    for (uint32_t sample = 0U; sample < (uint32_t)sample_count; ++sample) {
        const int32_t dx = (int32_t)sample_x[sample] - GRAPE_GPU_SUBPIXEL_HALF;
        const int32_t dy = (int32_t)sample_y[sample] - GRAPE_GPU_SUBPIXEL_HALF;
        bias->e0[sample] = e0_subpixel_x * dx + e0_subpixel_y * dy;
        bias->e1[sample] = e1_subpixel_x * dx + e1_subpixel_y * dy;
        bias->e2[sample] = e2_subpixel_x * dx + e2_subpixel_y * dy;
        bias->depth[sample] =
            setup->depth_step_x * ((float)dx * inverse_subpixel) +
            setup->depth_step_y * ((float)dy * inverse_subpixel);
    }
}

static esp_err_t gpu_rasterize_msaa_color_only(grape_gpu_context_t *context,
                                                const gpu_triangle_setup_t *setup,
                                                grape_color_t color)
{
    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t width = context->color_attachment->width;
    const int32_t min_x = setup->min_x;
    const int32_t max_x = setup->max_x;
    const int32_t min_y = setup->min_y;
    const int32_t max_y = setup->max_y;
    const int64_t e0_step_x = setup->e0_step_x;
    const int64_t e1_step_x = setup->e1_step_x;
    const int64_t e2_step_x = setup->e2_step_x;
    const int64_t e0_step_y = setup->e0_step_y;
    const int64_t e1_step_y = setup->e1_step_y;
    const int64_t e2_step_y = setup->e2_step_y;
    const uint8_t rgba[4] = { color.r, color.g, color.b, color.a };
    gpu_msaa_sample_bias_t bias;
    gpu_msaa_build_biases(setup, context->sample_count, &bias);

    bool wrote_sample = false;
    int64_t row_e0 = setup->row_e0;
    int64_t row_e1 = setup->row_e1;
    int64_t row_e2 = setup->row_e2;

    for (int32_t y = min_y; y <= max_y; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;

        for (int32_t x = min_x; x <= max_x; ++x) {
            uint8_t *sample_base = context->msaa_color +
                (((size_t)y * width + (uint32_t)x) * samples) * 4U;
            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + bias.e0[sample] >= 0 &&
                    e1 + bias.e1[sample] >= 0 &&
                    e2 + bias.e2[sample] >= 0) {
                    memcpy(sample_base + (size_t)sample * 4U, rgba, sizeof(rgba));
                    wrote_sample = true;
                }
            }
            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
    }

    if (wrote_sample) {
        grape_gpu_dirty_add(context, (grape_rect_t) {
            .x = min_x,
            .y = min_y,
            .width = max_x - min_x + 1,
            .height = max_y - min_y + 1,
        });
    }

    return ESP_OK;
}

static esp_err_t gpu_rasterize_msaa_depth_generic(grape_gpu_context_t *context,
                                                   const gpu_triangle_setup_t *setup,
                                                   grape_color_t color)
{
    const grape_gpu_depth_state_t depth_state = context->bound_pipeline->desc.depth;
    grape_gpu_depth_buffer_t *depth_target = context->depth_attachment;
    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t width = context->color_attachment->width;
    const int32_t min_x = setup->min_x;
    const int32_t max_x = setup->max_x;
    const int32_t min_y = setup->min_y;
    const int32_t max_y = setup->max_y;
    const int64_t e0_step_x = setup->e0_step_x;
    const int64_t e1_step_x = setup->e1_step_x;
    const int64_t e2_step_x = setup->e2_step_x;
    const int64_t e0_step_y = setup->e0_step_y;
    const int64_t e1_step_y = setup->e1_step_y;
    const int64_t e2_step_y = setup->e2_step_y;
    const float depth_step_x = setup->depth_step_x;
    const float depth_step_y = setup->depth_step_y;
    const size_t depth_stride = depth_target->stride;
    const uint8_t rgba[4] = { color.r, color.g, color.b, color.a };
    gpu_msaa_sample_bias_t bias;
    gpu_msaa_build_biases(setup, context->sample_count, &bias);

    bool wrote_sample = false;
    int64_t row_e0 = setup->row_e0;
    int64_t row_e1 = setup->row_e1;
    int64_t row_e2 = setup->row_e2;
    float row_depth = setup->depth_row_start;

    for (int32_t y = min_y; y <= max_y; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        float depth_f = row_depth;
        uint16_t *depth_row = (uint16_t *)((uint8_t *)depth_target->data + (size_t)y * depth_stride);

        for (int32_t x = min_x; x <= max_x; ++x) {
            uint8_t *sample_base = context->msaa_color +
                (((size_t)y * width + (uint32_t)x) * samples) * 4U;
            uint16_t *depth_base = depth_row + (size_t)x * samples;

            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + bias.e0[sample] >= 0 &&
                    e1 + bias.e1[sample] >= 0 &&
                    e2 + bias.e2[sample] >= 0) {
                    const uint16_t incoming_depth = gpu_depth_to_d16_fast(
                        depth_f + bias.depth[sample]
                    );
                    const bool depth_pass = !depth_state.test_enable ||
                        gpu_depth_compare(depth_state.compare_op,
                                          incoming_depth,
                                          depth_base[sample]);
                    if (depth_pass) {
                        if (depth_state.write_enable) {
                            depth_base[sample] = incoming_depth;
                        }
                        memcpy(sample_base + (size_t)sample * 4U, rgba, sizeof(rgba));
                        wrote_sample = true;
                    }
                }
            }

            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
            depth_f += depth_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
        row_depth += depth_step_y;
    }

    if (wrote_sample) {
        grape_gpu_dirty_add(context, (grape_rect_t) {
            .x = min_x,
            .y = min_y,
            .width = max_x - min_x + 1,
            .height = max_y - min_y + 1,
        });
    }

    return ESP_OK;
}

static esp_err_t gpu_rasterize_msaa_depth_less_write(grape_gpu_context_t *context,
                                                      const gpu_triangle_setup_t *setup,
                                                      grape_color_t color)
{
    grape_gpu_depth_buffer_t *depth_target = context->depth_attachment;
    const uint32_t samples = (uint32_t)context->sample_count;
    const uint32_t width = context->color_attachment->width;
    const int32_t min_x = setup->min_x;
    const int32_t max_x = setup->max_x;
    const int32_t min_y = setup->min_y;
    const int32_t max_y = setup->max_y;
    const int64_t e0_step_x = setup->e0_step_x;
    const int64_t e1_step_x = setup->e1_step_x;
    const int64_t e2_step_x = setup->e2_step_x;
    const int64_t e0_step_y = setup->e0_step_y;
    const int64_t e1_step_y = setup->e1_step_y;
    const int64_t e2_step_y = setup->e2_step_y;
    const float depth_step_x = setup->depth_step_x;
    const float depth_step_y = setup->depth_step_y;
    const size_t depth_stride = depth_target->stride;
    const uint8_t rgba[4] = { color.r, color.g, color.b, color.a };
    gpu_msaa_sample_bias_t bias;
    gpu_msaa_build_biases(setup, context->sample_count, &bias);

    bool wrote_sample = false;
    int64_t row_e0 = setup->row_e0;
    int64_t row_e1 = setup->row_e1;
    int64_t row_e2 = setup->row_e2;
    float row_depth = setup->depth_row_start;

    for (int32_t y = min_y; y <= max_y; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        float depth_f = row_depth;
        uint16_t *depth_row = (uint16_t *)((uint8_t *)depth_target->data + (size_t)y * depth_stride);

        for (int32_t x = min_x; x <= max_x; ++x) {
            uint8_t *sample_base = context->msaa_color +
                (((size_t)y * width + (uint32_t)x) * samples) * 4U;
            uint16_t *depth_base = depth_row + (size_t)x * samples;

            for (uint32_t sample = 0U; sample < samples; ++sample) {
                if (e0 + bias.e0[sample] >= 0 &&
                    e1 + bias.e1[sample] >= 0 &&
                    e2 + bias.e2[sample] >= 0) {
                    const uint16_t incoming_depth = gpu_depth_to_d16_fast(
                        depth_f + bias.depth[sample]
                    );
                    if (incoming_depth < depth_base[sample]) {
                        depth_base[sample] = incoming_depth;
                        memcpy(sample_base + (size_t)sample * 4U, rgba, sizeof(rgba));
                        wrote_sample = true;
                    }
                }
            }

            e0 += e0_step_x;
            e1 += e1_step_x;
            e2 += e2_step_x;
            depth_f += depth_step_x;
        }

        row_e0 += e0_step_y;
        row_e1 += e1_step_y;
        row_e2 += e2_step_y;
        row_depth += depth_step_y;
    }

    if (wrote_sample) {
        grape_gpu_dirty_add(context, (grape_rect_t) {
            .x = min_x,
            .y = min_y,
            .width = max_x - min_x + 1,
            .height = max_y - min_y + 1,
        });
    }

    return ESP_OK;
}

esp_err_t grape_gpu_raster_triangle(grape_gpu_context_t *context,
                                    const grape_gpu_clip_vertex_t triangle[3])
{
    if (grape_gpu_triangle_culled(context->bound_pipeline, triangle)) {
        return ESP_OK;
    }

    gpu_triangle_setup_t setup;
    if (!gpu_setup_triangle(context, triangle, &setup)) {
        return ESP_OK;
    }

    const grape_color_t color = gpu_fragment_color(context);
    const grape_gpu_depth_state_t *depth = &context->bound_pipeline->desc.depth;

    if (context->sample_count != GRAPE_GPU_SAMPLE_COUNT_1) {
        if (!depth->test_enable && !depth->write_enable) {
            return gpu_rasterize_msaa_color_only(context, &setup, color);
        }
        if (depth->test_enable && depth->write_enable &&
            depth->compare_op == GRAPE_GPU_COMPARE_LESS) {
            return gpu_rasterize_msaa_depth_less_write(context, &setup, color);
        }
        return gpu_rasterize_msaa_depth_generic(context, &setup, color);
    }

    if (!depth->test_enable && !depth->write_enable) {
        return gpu_rasterize_color_only(context, &setup, color);
    }

    if (depth->test_enable && depth->write_enable &&
        depth->compare_op == GRAPE_GPU_COMPARE_LESS) {
        return gpu_rasterize_depth_less_write(context, &setup, color);
    }

    return gpu_rasterize_depth_generic(context, &setup, color);
}

static esp_err_t gpu_process_triangle(grape_gpu_context_t *context,
                                      const uint32_t indices[3])
{
    grape_gpu_clip_vertex_t input[3];
    for (uint32_t i = 0U; i < 3U; ++i) {
        esp_err_t ret = grape_gpu_vertex_fetch_transform(context, indices[i], &input[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    grape_gpu_clip_vertex_t clipped[GRAPE_GPU_MAX_CLIPPED_VERTICES];
    const uint32_t clipped_count = grape_gpu_clip_triangle(input, clipped);
    if (clipped_count < 3U) {
        return ESP_OK;
    }

    for (uint32_t i = 1U; i + 1U < clipped_count; ++i) {
        const grape_gpu_clip_vertex_t triangle[3] = {
            clipped[0],
            clipped[i],
            clipped[i + 1U],
        };
        esp_err_t ret = grape_gpu_raster_triangle(context, triangle);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

esp_err_t grape_gpu_raster_draw(grape_gpu_context_t *context,
                                uint32_t first_vertex,
                                uint32_t vertex_count)
{
    const grape_gpu_pipeline_t *pipeline = context->bound_pipeline;
    const grape_gpu_buffer_t *buffer = context->bound_vertex_buffer;
    const uint32_t stride = pipeline->desc.vertex_layout.stride;

    const uint64_t end_vertex = (uint64_t)first_vertex + vertex_count;
    if (end_vertex > SIZE_MAX / stride || (size_t)end_vertex * stride > buffer->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint32_t base = 0U; base < vertex_count; base += 3U) {
        const uint32_t indices[3] = {
            first_vertex + base,
            first_vertex + base + 1U,
            first_vertex + base + 2U,
        };
        esp_err_t ret = gpu_process_triangle(context, indices);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t gpu_read_index(const grape_gpu_context_t *context,
                                uint32_t index,
                                uint32_t *out_index)
{
    const size_t index_size = grape_gpu_index_type_size_internal(context->bound_index_type);
    if (index_size == 0U || index > SIZE_MAX / index_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t offset = (size_t)index * index_size;
    if (offset > context->bound_index_buffer->size ||
        index_size > context->bound_index_buffer->size - offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (context->bound_index_type == GRAPE_GPU_INDEX_U16) {
        uint16_t value;
        memcpy(&value, context->bound_index_buffer->data + offset, sizeof(value));
        *out_index = value;
    } else {
        uint32_t value;
        memcpy(&value, context->bound_index_buffer->data + offset, sizeof(value));
        *out_index = value;
    }
    return ESP_OK;
}

esp_err_t grape_gpu_raster_draw_indexed(grape_gpu_context_t *context,
                                        uint32_t first_index,
                                        uint32_t index_count,
                                        int32_t vertex_offset)
{
    const size_t index_size = grape_gpu_index_type_size_internal(context->bound_index_type);
    const uint64_t end_index = (uint64_t)first_index + index_count;
    if (index_size == 0U || end_index > SIZE_MAX / index_size ||
        (size_t)end_index * index_size > context->bound_index_buffer->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint32_t base = 0U; base < index_count; base += 3U) {
        uint32_t indices[3];
        for (uint32_t i = 0U; i < 3U; ++i) {
            uint32_t raw_index;
            esp_err_t ret = gpu_read_index(context, first_index + base + i, &raw_index);
            if (ret != ESP_OK) {
                return ret;
            }

            const int64_t adjusted = (int64_t)raw_index + vertex_offset;
            if (adjusted < 0 || adjusted > UINT32_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            indices[i] = (uint32_t)adjusted;
        }

        esp_err_t ret = gpu_process_triangle(context, indices);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}
