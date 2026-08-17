#include <float.h>
#include <math.h>
#include <string.h>

#include "grape_gpu_internal.h"
#include "grape_internal.h"

#define GRAPE_GPU_SUBPIXEL_BITS 3
#define GRAPE_GPU_SUBPIXEL_SCALE (1 << GRAPE_GPU_SUBPIXEL_BITS)
#define GRAPE_GPU_SUBPIXEL_HALF (GRAPE_GPU_SUBPIXEL_SCALE / 2)

typedef struct {
    float x;
    float y;
    float z;
    float w;
} grape_gpu_clip_vertex_t;

typedef struct {
    int32_t x;
    int32_t y;
} grape_gpu_fixed_point_t;

static esp_err_t gpu_fetch_clip_vertex(const grape_gpu_pipeline_t *pipeline,
                                       const grape_gpu_buffer_t *buffer,
                                       uint32_t vertex_index,
                                       grape_gpu_clip_vertex_t *out_vertex)
{
    const grape_gpu_vertex_layout_t *layout = &pipeline->desc.vertex_layout;
    const grape_gpu_vertex_attribute_t *attribute =
        &layout->attributes[pipeline->position_attribute_index];

    if (vertex_index > SIZE_MAX / layout->stride) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t vertex_offset = (size_t)vertex_index * layout->stride;
    size_t attribute_size = grape_gpu_vertex_format_size_internal(attribute->format);
    if (vertex_offset > buffer->size ||
        attribute->offset > buffer->size - vertex_offset ||
        attribute_size > buffer->size - vertex_offset - attribute->offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *source = buffer->data + vertex_offset + attribute->offset;
    float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    memcpy(values, source, attribute_size);

    for (size_t i = 0U; i < attribute_size / sizeof(float); ++i) {
        if (!isfinite(values[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    *out_vertex = (grape_gpu_clip_vertex_t) {
        .x = values[0],
        .y = values[1],
        .z = values[2],
        .w = attribute->format == GRAPE_GPU_VERTEX_FORMAT_F32X4 ? values[3] : 1.0f,
    };
    return ESP_OK;
}

static bool gpu_clip_to_screen(const grape_gpu_viewport_t *viewport,
                               const grape_gpu_clip_vertex_t *vertex,
                               float *out_x,
                               float *out_y)
{
    if (vertex->w <= FLT_EPSILON) {
        return false;
    }

    float inverse_w = 1.0f / vertex->w;
    float ndc_x = vertex->x * inverse_w;
    float ndc_y = vertex->y * inverse_w;
    if (!isfinite(ndc_x) || !isfinite(ndc_y)) {
        return false;
    }

    float screen_x = viewport->x + (ndc_x * 0.5f + 0.5f) * viewport->width;
    float screen_y = viewport->y + (0.5f - ndc_y * 0.5f) * viewport->height;
    if (!isfinite(screen_x) || !isfinite(screen_y)) {
        return false;
    }

    *out_x = screen_x;
    *out_y = screen_y;
    return true;
}

static bool gpu_float_to_fixed(float value, int32_t *out_value)
{
    double scaled = (double)value * (double)GRAPE_GPU_SUBPIXEL_SCALE;
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
    int64_t dx = (int64_t)b.x - a.x;
    int64_t dy = (int64_t)b.y - a.y;
    return dx * (y - a.y) - dy * (x - a.x);
}

static bool gpu_edge_is_top_left(grape_gpu_fixed_point_t a,
                                 grape_gpu_fixed_point_t b)
{
    int32_t dx = b.x - a.x;
    int32_t dy = b.y - a.y;
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

static esp_err_t gpu_rasterize_triangle_rgba8888(grape_gpu_context_t *context,
                                                  const grape_gpu_clip_vertex_t clip[3],
                                                  grape_color_t color)
{
    float sx[3];
    float sy[3];
    for (size_t i = 0U; i < 3U; ++i) {
        if (!gpu_clip_to_screen(&context->viewport, &clip[i], &sx[i], &sy[i])) {
            return ESP_OK;
        }
    }

    grape_gpu_fixed_point_t fixed[3];
    for (size_t i = 0U; i < 3U; ++i) {
        if (!gpu_float_to_fixed(sx[i], &fixed[i].x) ||
            !gpu_float_to_fixed(sy[i], &fixed[i].y)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    int64_t area = gpu_edge(fixed[0], fixed[1], fixed[2].x, fixed[2].y);
    if (area == 0) {
        return ESP_OK;
    }

    if (area < 0) {
        grape_gpu_fixed_point_t temp_fixed = fixed[1];
        fixed[1] = fixed[2];
        fixed[2] = temp_fixed;
        float temp_x = sx[1];
        sx[1] = sx[2];
        sx[2] = temp_x;
        float temp_y = sy[1];
        sy[1] = sy[2];
        sy[2] = temp_y;
    }

    float min_sx = fminf(sx[0], fminf(sx[1], sx[2]));
    float max_sx = fmaxf(sx[0], fmaxf(sx[1], sx[2]));
    float min_sy = fminf(sy[0], fminf(sy[1], sy[2]));
    float max_sy = fmaxf(sy[0], fmaxf(sy[1], sy[2]));

    int32_t min_x = gpu_floor_to_i32_safe(min_sx);
    int32_t max_x = gpu_ceil_to_i32_safe(max_sx) - 1;
    int32_t min_y = gpu_floor_to_i32_safe(min_sy);
    int32_t max_y = gpu_ceil_to_i32_safe(max_sy) - 1;

    int32_t viewport_min_x = gpu_floor_to_i32_safe(context->viewport.x);
    int32_t viewport_max_x = gpu_ceil_to_i32_safe(
        context->viewport.x + context->viewport.width
    ) - 1;
    int32_t viewport_min_y = gpu_floor_to_i32_safe(context->viewport.y);
    int32_t viewport_max_y = gpu_ceil_to_i32_safe(
        context->viewport.y + context->viewport.height
    ) - 1;

    grape_texture_t *target = context->color_attachment;
    min_x = gpu_max_i32(min_x, 0);
    min_y = gpu_max_i32(min_y, 0);
    min_x = gpu_max_i32(min_x, viewport_min_x);
    min_y = gpu_max_i32(min_y, viewport_min_y);
    max_x = gpu_min_i32(max_x, (int32_t)target->width - 1);
    max_y = gpu_min_i32(max_y, (int32_t)target->height - 1);
    max_x = gpu_min_i32(max_x, viewport_max_x);
    max_y = gpu_min_i32(max_y, viewport_max_y);

    if (min_x > max_x || min_y > max_y) {
        return ESP_OK;
    }

    const int64_t bias0 = gpu_edge_is_top_left(fixed[1], fixed[2]) ? 0 : -1;
    const int64_t bias1 = gpu_edge_is_top_left(fixed[2], fixed[0]) ? 0 : -1;
    const int64_t bias2 = gpu_edge_is_top_left(fixed[0], fixed[1]) ? 0 : -1;

    int64_t sample_x = (int64_t)min_x * GRAPE_GPU_SUBPIXEL_SCALE + GRAPE_GPU_SUBPIXEL_HALF;
    int64_t sample_y = (int64_t)min_y * GRAPE_GPU_SUBPIXEL_SCALE + GRAPE_GPU_SUBPIXEL_HALF;

    int64_t row_e0 = gpu_edge(fixed[1], fixed[2], sample_x, sample_y) + bias0;
    int64_t row_e1 = gpu_edge(fixed[2], fixed[0], sample_x, sample_y) + bias1;
    int64_t row_e2 = gpu_edge(fixed[0], fixed[1], sample_x, sample_y) + bias2;

    int64_t e0_step_x = -((int64_t)fixed[2].y - fixed[1].y) * GRAPE_GPU_SUBPIXEL_SCALE;
    int64_t e1_step_x = -((int64_t)fixed[0].y - fixed[2].y) * GRAPE_GPU_SUBPIXEL_SCALE;
    int64_t e2_step_x = -((int64_t)fixed[1].y - fixed[0].y) * GRAPE_GPU_SUBPIXEL_SCALE;
    int64_t e0_step_y = ((int64_t)fixed[2].x - fixed[1].x) * GRAPE_GPU_SUBPIXEL_SCALE;
    int64_t e1_step_y = ((int64_t)fixed[0].x - fixed[2].x) * GRAPE_GPU_SUBPIXEL_SCALE;
    int64_t e2_step_y = ((int64_t)fixed[1].x - fixed[0].x) * GRAPE_GPU_SUBPIXEL_SCALE;

    bool wrote_pixel = false;
    for (int32_t y = min_y; y <= max_y; ++y) {
        int64_t e0 = row_e0;
        int64_t e1 = row_e1;
        int64_t e2 = row_e2;
        uint8_t *row = target->pixels + (size_t)y * target->stride;

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

esp_err_t grape_gpu_raster_draw(grape_gpu_context_t *context,
                                uint32_t first_vertex,
                                uint32_t vertex_count)
{
    const grape_gpu_pipeline_t *pipeline = context->bound_pipeline;
    const grape_gpu_buffer_t *buffer = context->bound_vertex_buffer;
    const uint32_t stride = pipeline->desc.vertex_layout.stride;

    uint64_t end_vertex = (uint64_t)first_vertex + vertex_count;
    if (end_vertex > SIZE_MAX / stride || (size_t)end_vertex * stride > buffer->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (uint32_t base = 0U; base < vertex_count; base += 3U) {
        grape_gpu_clip_vertex_t clip[3];
        for (uint32_t i = 0U; i < 3U; ++i) {
            esp_err_t ret = gpu_fetch_clip_vertex(
                pipeline,
                buffer,
                first_vertex + base + i,
                &clip[i]
            );
            if (ret != ESP_OK) {
                return ret;
            }
        }

        esp_err_t ret = gpu_rasterize_triangle_rgba8888(
            context,
            clip,
            pipeline->desc.solid_color
        );
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}
