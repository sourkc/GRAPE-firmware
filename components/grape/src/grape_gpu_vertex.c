#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "grape_gpu_internal.h"

static esp_err_t gpu_fetch_attribute(const grape_gpu_pipeline_t *pipeline,
                                     const grape_gpu_buffer_t *buffer,
                                     uint32_t attribute_index,
                                     uint32_t vertex_index,
                                     float values[4])
{
    if (attribute_index == UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const grape_gpu_vertex_layout_t *layout = &pipeline->desc.vertex_layout;
    const grape_gpu_vertex_attribute_t *attribute = &layout->attributes[attribute_index];
    if (vertex_index > SIZE_MAX / layout->stride) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t vertex_offset = (size_t)vertex_index * layout->stride;
    const size_t attribute_size = grape_gpu_vertex_format_size_internal(attribute->format);
    if (vertex_offset > buffer->size ||
        attribute->offset > buffer->size - vertex_offset ||
        attribute_size > buffer->size - vertex_offset - attribute->offset) {
        return ESP_ERR_INVALID_SIZE;
    }

    values[0] = 0.0f;
    values[1] = 0.0f;
    values[2] = 0.0f;
    values[3] = 1.0f;
    memcpy(values, buffer->data + vertex_offset + attribute->offset, attribute_size);

    for (size_t i = 0U; i < attribute_size / sizeof(float); ++i) {
        if (!isfinite(values[i])) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

static esp_err_t gpu_fetch_vertex(const grape_gpu_pipeline_t *pipeline,
                                  const grape_gpu_buffer_t *buffer,
                                  uint32_t vertex_index,
                                  grape_gpu_clip_vertex_t *out_vertex)
{
    float position[4];
    esp_err_t ret = gpu_fetch_attribute(
        pipeline, buffer, pipeline->position_attribute_index, vertex_index, position
    );
    if (ret != ESP_OK) {
        return ret;
    }

    *out_vertex = (grape_gpu_clip_vertex_t) {
        .x = position[0],
        .y = position[1],
        .z = position[2],
        .w = pipeline->desc.vertex_layout.attributes[pipeline->position_attribute_index].format ==
                GRAPE_GPU_VERTEX_FORMAT_F32X4 ? position[3] : 1.0f,
        .u = 0.0f,
        .v = 0.0f,
        .color = {1.0f, 1.0f, 1.0f, 1.0f},
    };

    if (pipeline->texcoord_attribute_index != UINT32_MAX) {
        float uv[4];
        ret = gpu_fetch_attribute(
            pipeline, buffer, pipeline->texcoord_attribute_index, vertex_index, uv
        );
        if (ret != ESP_OK) {
            return ret;
        }
        out_vertex->u = uv[0];
        out_vertex->v = uv[1];
    }

    if (pipeline->color_attribute_index != UINT32_MAX) {
        float color[4];
        ret = gpu_fetch_attribute(
            pipeline, buffer, pipeline->color_attribute_index, vertex_index, color
        );
        if (ret != ESP_OK) {
            return ret;
        }
        for (uint32_t i = 0U; i < 4U; ++i) {
            out_vertex->color[i] = color[i];
        }
    }
    return ESP_OK;
}

static esp_err_t gpu_apply_mvp(const grape_gpu_context_t *context,
                               const grape_gpu_clip_vertex_t *input,
                               grape_gpu_clip_vertex_t *output)
{
    grape_gpu_mat4_t matrix;
    memcpy(&matrix,
           context->push_constants + offsetof(grape_gpu_builtin_constants_t, mvp),
           sizeof(matrix));

    const float x = input->x;
    const float y = input->y;
    const float z = input->z;
    const float w = input->w;
    const float *m = matrix.m;

    *output = *input;
    output->x = m[0] * x + m[1] * y + m[2] * z + m[3] * w;
    output->y = m[4] * x + m[5] * y + m[6] * z + m[7] * w;
    output->z = m[8] * x + m[9] * y + m[10] * z + m[11] * w;
    output->w = m[12] * x + m[13] * y + m[14] * z + m[15] * w;

    if (!isfinite(output->x) || !isfinite(output->y) ||
        !isfinite(output->z) || !isfinite(output->w)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t grape_gpu_vertex_fetch_transform(const grape_gpu_context_t *context,
                                           uint32_t vertex_index,
                                           grape_gpu_clip_vertex_t *out_vertex)
{
    if (!context || !context->bound_pipeline || !context->bound_vertex_buffer || !out_vertex) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_gpu_clip_vertex_t input;
    esp_err_t ret = gpu_fetch_vertex(
        context->bound_pipeline,
        context->bound_vertex_buffer,
        vertex_index,
        &input
    );
    if (ret != ESP_OK) {
        return ret;
    }

    switch (context->bound_pipeline->desc.vertex_program) {
        case GRAPE_GPU_VERTEX_PROGRAM_CLIP_SPACE:
            *out_vertex = input;
            return ESP_OK;
        case GRAPE_GPU_VERTEX_PROGRAM_MVP:
            return gpu_apply_mvp(context, &input, out_vertex);
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

typedef enum {
    GPU_CLIP_LEFT = 0,
    GPU_CLIP_RIGHT,
    GPU_CLIP_BOTTOM,
    GPU_CLIP_TOP,
    GPU_CLIP_NEAR,
    GPU_CLIP_FAR,
    GPU_CLIP_PLANE_COUNT,
} gpu_clip_plane_t;

static float gpu_clip_distance(const grape_gpu_clip_vertex_t *v, gpu_clip_plane_t plane)
{
    switch (plane) {
        case GPU_CLIP_LEFT:
            return v->x + v->w;
        case GPU_CLIP_RIGHT:
            return v->w - v->x;
        case GPU_CLIP_BOTTOM:
            return v->y + v->w;
        case GPU_CLIP_TOP:
            return v->w - v->y;
        case GPU_CLIP_NEAR:
            return v->z;
        case GPU_CLIP_FAR:
            return v->w - v->z;
        default:
            return -1.0f;
    }
}

static grape_gpu_clip_vertex_t gpu_clip_lerp(const grape_gpu_clip_vertex_t *a,
                                              const grape_gpu_clip_vertex_t *b,
                                              float t)
{
    grape_gpu_clip_vertex_t out = {
        .x = a->x + (b->x - a->x) * t,
        .y = a->y + (b->y - a->y) * t,
        .z = a->z + (b->z - a->z) * t,
        .w = a->w + (b->w - a->w) * t,
        .u = a->u + (b->u - a->u) * t,
        .v = a->v + (b->v - a->v) * t,
    };
    for (uint32_t i = 0U; i < 4U; ++i) {
        out.color[i] = a->color[i] + (b->color[i] - a->color[i]) * t;
    }
    return out;
}

uint32_t grape_gpu_clip_triangle(const grape_gpu_clip_vertex_t input[3],
                                 grape_gpu_clip_vertex_t output[GRAPE_GPU_MAX_CLIPPED_VERTICES])
{
    grape_gpu_clip_vertex_t a[GRAPE_GPU_MAX_CLIPPED_VERTICES];
    grape_gpu_clip_vertex_t b[GRAPE_GPU_MAX_CLIPPED_VERTICES];
    memcpy(a, input, sizeof(grape_gpu_clip_vertex_t) * 3U);
    uint32_t input_count = 3U;

    grape_gpu_clip_vertex_t *src = a;
    grape_gpu_clip_vertex_t *dst = b;

    for (gpu_clip_plane_t plane = GPU_CLIP_LEFT;
         plane < GPU_CLIP_PLANE_COUNT;
         plane = (gpu_clip_plane_t)(plane + 1)) {
        if (input_count == 0U) {
            break;
        }

        uint32_t output_count = 0U;
        grape_gpu_clip_vertex_t previous = src[input_count - 1U];
        float previous_distance = gpu_clip_distance(&previous, plane);
        bool previous_inside = previous_distance >= 0.0f;

        for (uint32_t i = 0U; i < input_count; ++i) {
            grape_gpu_clip_vertex_t current = src[i];
            const float current_distance = gpu_clip_distance(&current, plane);
            const bool current_inside = current_distance >= 0.0f;

            if (current_inside != previous_inside) {
                const float denominator = previous_distance - current_distance;
                float t = fabsf(denominator) > FLT_EPSILON
                    ? previous_distance / denominator
                    : 0.0f;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                if (output_count >= GRAPE_GPU_MAX_CLIPPED_VERTICES) {
                    return 0U;
                }
                dst[output_count++] = gpu_clip_lerp(&previous, &current, t);
            }

            if (current_inside) {
                if (output_count >= GRAPE_GPU_MAX_CLIPPED_VERTICES) {
                    return 0U;
                }
                dst[output_count++] = current;
            }

            previous = current;
            previous_distance = current_distance;
            previous_inside = current_inside;
        }

        input_count = output_count;
        grape_gpu_clip_vertex_t *temp = src;
        src = dst;
        dst = temp;
    }

    if (input_count != 0U) {
        memcpy(output, src, sizeof(grape_gpu_clip_vertex_t) * input_count);
    }
    return input_count;
}

bool grape_gpu_triangle_culled(const grape_gpu_pipeline_t *pipeline,
                               const grape_gpu_clip_vertex_t triangle[3])
{
    float ndc_x[3];
    float ndc_y[3];
    for (uint32_t i = 0U; i < 3U; ++i) {
        if (triangle[i].w <= FLT_EPSILON) {
            return true;
        }
        const float inverse_w = 1.0f / triangle[i].w;
        ndc_x[i] = triangle[i].x * inverse_w;
        ndc_y[i] = triangle[i].y * inverse_w;
    }

    const float signed_area =
        (ndc_x[1] - ndc_x[0]) * (ndc_y[2] - ndc_y[0]) -
        (ndc_y[1] - ndc_y[0]) * (ndc_x[2] - ndc_x[0]);
    if (!isfinite(signed_area) || fabsf(signed_area) <= FLT_EPSILON) {
        return true;
    }

    const bool front_facing = pipeline->desc.front_face == GRAPE_GPU_FRONT_FACE_CCW
        ? signed_area > 0.0f
        : signed_area < 0.0f;

    switch (pipeline->desc.cull_mode) {
        case GRAPE_GPU_CULL_NONE:
            return false;
        case GRAPE_GPU_CULL_FRONT:
            return front_facing;
        case GRAPE_GPU_CULL_BACK:
            return !front_facing;
        default:
            return true;
    }
}
