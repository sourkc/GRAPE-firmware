#include <math.h>
#include <stdlib.h>

#include "grape_gpu_internal.h"
#include "grape_internal.h"

static bool gpu_viewport_valid(const grape_gpu_viewport_t *viewport)
{
    return viewport &&
           isfinite(viewport->x) &&
           isfinite(viewport->y) &&
           isfinite(viewport->width) &&
           isfinite(viewport->height) &&
           isfinite(viewport->min_depth) &&
           isfinite(viewport->max_depth) &&
           viewport->width > 0.0f &&
           viewport->height > 0.0f &&
           viewport->min_depth >= 0.0f &&
           viewport->max_depth <= 1.0f &&
           viewport->min_depth <= viewport->max_depth;
}

static bool gpu_color_format_supported(grape_pixel_format_t format)
{
    return format == GRAPE_PIXEL_FORMAT_RGBA8888;
}

void grape_gpu_dirty_add(grape_gpu_context_t *context, grape_rect_t rect)
{
    if (rect.width <= 0 || rect.height <= 0) {
        return;
    }

    if (!context->dirty_valid) {
        context->dirty_rect = rect;
        context->dirty_valid = true;
        return;
    }

    context->dirty_rect = grape_rect_union(context->dirty_rect, rect);
}

static void gpu_clear_rgba8888(grape_texture_t *texture, grape_color_t color)
{
    for (uint32_t y = 0U; y < texture->height; ++y) {
        uint8_t *row = texture->pixels + (size_t)y * texture->stride;
        for (uint32_t x = 0U; x < texture->width; ++x) {
            uint8_t *pixel = row + (size_t)x * 4U;
            pixel[0] = color.r;
            pixel[1] = color.g;
            pixel[2] = color.b;
            pixel[3] = color.a;
        }
    }
}

esp_err_t grape_gpu_context_create(grape_context_t *grape, grape_gpu_context_t **out_context)
{
    if (!grape || !out_context) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_gpu_context_t *context = calloc(1, sizeof(*context));
    if (!context) {
        return ESP_ERR_NO_MEM;
    }

    context->grape = grape;
    *out_context = context;
    return ESP_OK;
}

esp_err_t grape_gpu_context_destroy(grape_gpu_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    if (context->render_pass_active || context->buffers || context->pipelines) {
        return ESP_ERR_INVALID_STATE;
    }

    free(context);
    return ESP_OK;
}

esp_err_t grape_gpu_begin_render_pass(grape_gpu_context_t *context,
                                      const grape_gpu_render_pass_desc_t *desc)
{
    if (!context || !desc || !desc->color_attachment ||
        desc->color_load_op < GRAPE_GPU_LOAD_OP_LOAD ||
        desc->color_load_op > GRAPE_GPU_LOAD_OP_CLEAR) {
        return ESP_ERR_INVALID_ARG;
    }
    if (context->render_pass_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (desc->color_attachment->context != context->grape ||
        desc->color_attachment->width > INT32_MAX ||
        desc->color_attachment->height > INT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!gpu_color_format_supported(desc->color_attachment->format)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    context->color_attachment = desc->color_attachment;
    context->bound_pipeline = NULL;
    context->bound_vertex_buffer = NULL;
    context->dirty_valid = false;
    context->viewport = (grape_gpu_viewport_t) {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)desc->color_attachment->width,
        .height = (float)desc->color_attachment->height,
        .min_depth = 0.0f,
        .max_depth = 1.0f,
    };
    context->render_pass_active = true;

    if (desc->color_load_op == GRAPE_GPU_LOAD_OP_CLEAR) {
        gpu_clear_rgba8888(desc->color_attachment, desc->clear_color);
        grape_gpu_dirty_add(context, (grape_rect_t) {
            .x = 0,
            .y = 0,
            .width = (int32_t)desc->color_attachment->width,
            .height = (int32_t)desc->color_attachment->height,
        });
    }

    return ESP_OK;
}

esp_err_t grape_gpu_end_render_pass(grape_gpu_context_t *context)
{
    if (!context || !context->render_pass_active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    if (context->dirty_valid) {
        grape_rect_t dirty = context->dirty_rect;
        ret = grape_texture_invalidate_rect(
            context->color_attachment,
            (uint32_t)dirty.x,
            (uint32_t)dirty.y,
            (uint32_t)dirty.width,
            (uint32_t)dirty.height
        );
    }

    context->render_pass_active = false;
    context->color_attachment = NULL;
    context->bound_pipeline = NULL;
    context->bound_vertex_buffer = NULL;
    context->dirty_valid = false;
    return ret;
}

esp_err_t grape_gpu_set_viewport(grape_gpu_context_t *context,
                                 const grape_gpu_viewport_t *viewport)
{
    if (!context || !viewport) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->render_pass_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!gpu_viewport_valid(viewport)) {
        return ESP_ERR_INVALID_ARG;
    }

    context->viewport = *viewport;
    return ESP_OK;
}

esp_err_t grape_gpu_bind_pipeline(grape_gpu_context_t *context,
                                  grape_gpu_pipeline_t *pipeline)
{
    if (!context || !pipeline) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->render_pass_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pipeline->context != context) {
        return ESP_ERR_INVALID_ARG;
    }

    context->bound_pipeline = pipeline;
    return ESP_OK;
}

esp_err_t grape_gpu_bind_vertex_buffer(grape_gpu_context_t *context,
                                       grape_gpu_buffer_t *buffer)
{
    if (!context || !buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->render_pass_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer->context != context || buffer->usage != GRAPE_GPU_BUFFER_VERTEX) {
        return ESP_ERR_INVALID_ARG;
    }

    context->bound_vertex_buffer = buffer;
    return ESP_OK;
}

esp_err_t grape_gpu_draw(grape_gpu_context_t *context,
                         uint32_t first_vertex,
                         uint32_t vertex_count)
{
    if (!context || !context->render_pass_active ||
        !context->bound_pipeline || !context->bound_vertex_buffer) {
        return ESP_ERR_INVALID_STATE;
    }
    if (vertex_count == 0U || vertex_count % 3U != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    return grape_gpu_raster_draw(context, first_vertex, vertex_count);
}
