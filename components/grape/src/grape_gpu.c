#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "grape_gpu_internal.h"
#include "grape_internal.h"

static void gpu_unbind_textures(grape_gpu_context_t *context)
{
    if (!context) {
        return;
    }
    for (uint32_t slot = 0U; slot < GRAPE_GPU_MAX_TEXTURE_SLOTS; ++slot) {
        if (context->bound_textures[slot]) {
            if (context->bound_textures[slot]->ref_count != 0U) {
                context->bound_textures[slot]->ref_count--;
            }
            context->bound_textures[slot] = NULL;
        }
    }
}

static bool gpu_fragment_uses_texture(grape_gpu_fragment_program_t program)
{
    return program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE ||
           program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR;
}


grape_gpu_sample_count_t grape_gpu_sample_count_resolve(grape_gpu_sample_count_t sample_count)
{
    return sample_count == 0 ? GRAPE_GPU_SAMPLE_COUNT_1 : sample_count;
}

bool grape_gpu_sample_count_valid(grape_gpu_sample_count_t sample_count)
{
    const grape_gpu_sample_count_t resolved = grape_gpu_sample_count_resolve(sample_count);
    return resolved == GRAPE_GPU_SAMPLE_COUNT_1 ||
           resolved == GRAPE_GPU_SAMPLE_COUNT_2 ||
           resolved == GRAPE_GPU_SAMPLE_COUNT_4;
}

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
    GRAPE_TIME_SCOPE(GPU_COLOR_CLEAR);

    const size_t row_bytes = (size_t)texture->width * 4U;
    if (color.r == 0U && color.g == 0U && color.b == 0U && color.a == 0U) {
        for (uint32_t y = 0U; y < texture->height; ++y) {
            memset(texture->pixels + (size_t)y * texture->stride, 0, row_bytes);
        }
        return;
    }

    const uint8_t rgba[4] = { color.r, color.g, color.b, color.a };
    uint32_t packed;
    memcpy(&packed, rgba, sizeof(packed));
    for (uint32_t y = 0U; y < texture->height; ++y) {
        uint32_t *row = (uint32_t *)(texture->pixels + (size_t)y * texture->stride);
        for (uint32_t x = 0U; x < texture->width; ++x) {
            row[x] = packed;
        }
    }
}

static void gpu_clear_d16(grape_gpu_depth_buffer_t *buffer, float clear_depth)
{
    GRAPE_TIME_SCOPE(GPU_DEPTH_CLEAR);

    const uint16_t value = gpu_depth_to_d16(clear_depth);
    if (value == 0U) {
        memset(buffer->data, 0, buffer->size);
        return;
    }
    if (value == UINT16_MAX) {
        memset(buffer->data, 0xFF, buffer->size);
        return;
    }

    const size_t samples_per_row = (size_t)buffer->width * (size_t)buffer->sample_count;
    for (uint32_t y = 0U; y < buffer->height; ++y) {
        uint16_t *row = (uint16_t *)((uint8_t *)buffer->data + (size_t)y * buffer->stride);
        for (size_t x = 0U; x < samples_per_row; ++x) {
            row[x] = value;
        }
    }
}

static void gpu_init_default_push_constants(grape_gpu_context_t *context)
{
    grape_gpu_builtin_constants_t constants = {
        .mvp = {
            .m = {
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f,
            },
        },
        .color = { .r = 255, .g = 255, .b = 255, .a = 255 },
    };

    memset(context->push_constants, 0, sizeof(context->push_constants));
    memcpy(context->push_constants, &constants, sizeof(constants));
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
    gpu_init_default_push_constants(context);
    *out_context = context;
    return ESP_OK;
}

esp_err_t grape_gpu_context_destroy(grape_gpu_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    if (context->render_pass_active || context->buffers || context->depth_buffers || context->pipelines) {
        return ESP_ERR_INVALID_STATE;
    }

    gpu_unbind_textures(context);
    grape_gpu_tile_release(context);
    free(context);
    return ESP_OK;
}

esp_err_t grape_gpu_begin_render_pass(grape_gpu_context_t *context,
                                      const grape_gpu_render_pass_desc_t *desc)
{
    GRAPE_TIME_SCOPE(GPU_PASS_BEGIN);
    if (!context || !desc || !desc->color_attachment ||
        desc->color_load_op < GRAPE_GPU_LOAD_OP_LOAD ||
        desc->color_load_op > GRAPE_GPU_LOAD_OP_CLEAR ||
        !grape_gpu_sample_count_valid(desc->sample_count)) {
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

    grape_gpu_sample_count_t sample_count = desc->sample_count;
    if (sample_count == 0 && desc->depth_attachment) {
        sample_count = desc->depth_attachment->sample_count;
    }
    sample_count = grape_gpu_sample_count_resolve(sample_count);

    if (desc->depth_attachment) {
        if (desc->depth_attachment->context != context ||
            desc->depth_attachment->width != desc->color_attachment->width ||
            desc->depth_attachment->height != desc->color_attachment->height ||
            desc->depth_attachment->format != GRAPE_GPU_DEPTH_D16 ||
            desc->depth_attachment->sample_count != sample_count ||
            desc->depth_load_op < GRAPE_GPU_LOAD_OP_LOAD ||
            desc->depth_load_op > GRAPE_GPU_LOAD_OP_CLEAR) {
            return ESP_ERR_INVALID_ARG;
        }
        if (desc->depth_load_op == GRAPE_GPU_LOAD_OP_CLEAR &&
            (!isfinite(desc->clear_depth) ||
             desc->clear_depth < 0.0f || desc->clear_depth > 1.0f)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    context->color_attachment = desc->color_attachment;
    context->depth_attachment = desc->depth_attachment;
    context->sample_count = sample_count;
    context->bound_pipeline = NULL;
    context->bound_vertex_buffer = NULL;
    context->bound_index_buffer = NULL;
    gpu_unbind_textures(context);
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

    esp_err_t ret = ESP_OK;
    if (sample_count == GRAPE_GPU_SAMPLE_COUNT_1) {
        if (desc->color_load_op == GRAPE_GPU_LOAD_OP_CLEAR) {
            gpu_clear_rgba8888(desc->color_attachment, desc->clear_color);
            grape_gpu_dirty_add(context, (grape_rect_t) {
                .x = 0,
                .y = 0,
                .width = (int32_t)desc->color_attachment->width,
                .height = (int32_t)desc->color_attachment->height,
            });
        }
    } else {
        ret = grape_gpu_tile_begin(context, desc->color_load_op, desc->clear_color);
        if (ret != ESP_OK) {
            context->render_pass_active = false;
            context->color_attachment = NULL;
            context->depth_attachment = NULL;
            context->sample_count = GRAPE_GPU_SAMPLE_COUNT_1;
            return ret;
        }
        if (desc->color_load_op == GRAPE_GPU_LOAD_OP_CLEAR) {
            gpu_clear_rgba8888(desc->color_attachment, desc->clear_color);
            grape_gpu_dirty_add(context, (grape_rect_t) {
                .x = 0,
                .y = 0,
                .width = (int32_t)desc->color_attachment->width,
                .height = (int32_t)desc->color_attachment->height,
            });
        }
    }

    if (desc->depth_attachment) {
        if (sample_count == GRAPE_GPU_SAMPLE_COUNT_1) {
            if (desc->depth_load_op == GRAPE_GPU_LOAD_OP_CLEAR) {
                gpu_clear_d16(desc->depth_attachment, desc->clear_depth);
            }
        } else {
            ret = grape_gpu_depth_begin_pass(
                desc->depth_attachment,
                desc->depth_load_op,
                desc->clear_depth
            );
            if (ret != ESP_OK) {
                context->render_pass_active = false;
                context->color_attachment = NULL;
                context->depth_attachment = NULL;
                context->sample_count = GRAPE_GPU_SAMPLE_COUNT_1;
                return ret;
            }
        }
    }

    return ESP_OK;
}

esp_err_t grape_gpu_end_render_pass(grape_gpu_context_t *context)
{
    GRAPE_TIME_SCOPE(GPU_PASS_END);
    if (!context || !context->render_pass_active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;
    if (context->sample_count != GRAPE_GPU_SAMPLE_COUNT_1) {
        ret = grape_gpu_tile_execute(context);
    }

    if (ret == ESP_OK && context->dirty_valid) {
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
    context->depth_attachment = NULL;
    context->sample_count = GRAPE_GPU_SAMPLE_COUNT_1;
    context->bound_pipeline = NULL;
    context->bound_vertex_buffer = NULL;
    context->bound_index_buffer = NULL;
    gpu_unbind_textures(context);
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

esp_err_t grape_gpu_set_push_constants(grape_gpu_context_t *context,
                                       uint32_t offset,
                                       const void *data,
                                       size_t size)
{
    if (!context || (!data && size != 0U) || offset > GRAPE_GPU_MAX_PUSH_CONSTANT_BYTES ||
        size > GRAPE_GPU_MAX_PUSH_CONSTANT_BYTES - offset) {
        return ESP_ERR_INVALID_ARG;
    }

    if (size != 0U) {
        memcpy(context->push_constants + offset, data, size);
    }
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
    if (pipeline->desc.sample_count != context->sample_count) {
        return ESP_ERR_INVALID_STATE;
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

esp_err_t grape_gpu_bind_index_buffer(grape_gpu_context_t *context,
                                      grape_gpu_buffer_t *buffer,
                                      grape_gpu_index_type_t index_type)
{
    if (!context || !buffer ||
        index_type < GRAPE_GPU_INDEX_U16 || index_type > GRAPE_GPU_INDEX_U32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->render_pass_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (buffer->context != context || buffer->usage != GRAPE_GPU_BUFFER_INDEX) {
        return ESP_ERR_INVALID_ARG;
    }

    context->bound_index_buffer = buffer;
    context->bound_index_type = index_type;
    return ESP_OK;
}

esp_err_t grape_gpu_bind_texture(grape_gpu_context_t *context,
                                 uint32_t slot,
                                 grape_texture_t *texture)
{
    if (!context || slot >= GRAPE_GPU_MAX_TEXTURE_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->render_pass_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (texture && (texture->context != context->grape || texture == context->color_attachment)) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_texture_t *previous = context->bound_textures[slot];
    if (previous == texture) {
        return ESP_OK;
    }
    if (texture) {
        texture->ref_count++;
    }
    context->bound_textures[slot] = texture;
    if (previous && previous->ref_count != 0U) {
        previous->ref_count--;
    }
    return ESP_OK;
}

static esp_err_t gpu_validate_draw_state(const grape_gpu_context_t *context)
{
    if (!context || !context->render_pass_active ||
        !context->bound_pipeline || !context->bound_vertex_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    const grape_gpu_depth_state_t *depth = &context->bound_pipeline->desc.depth;
    if ((depth->test_enable || depth->write_enable) && !context->depth_attachment) {
        return ESP_ERR_INVALID_STATE;
    }
    if (gpu_fragment_uses_texture(context->bound_pipeline->desc.fragment_program) &&
        !context->bound_textures[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t grape_gpu_draw(grape_gpu_context_t *context,
                         uint32_t first_vertex,
                         uint32_t vertex_count)
{
    GRAPE_TIME_SCOPE(GPU_DRAW);
    esp_err_t ret = gpu_validate_draw_state(context);
    if (ret != ESP_OK) {
        return ret;
    }
    if (vertex_count == 0U || vertex_count % 3U != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    return grape_gpu_raster_draw(context, first_vertex, vertex_count);
}

esp_err_t grape_gpu_draw_indexed(grape_gpu_context_t *context,
                                 uint32_t first_index,
                                 uint32_t index_count,
                                 int32_t vertex_offset)
{
    GRAPE_TIME_SCOPE(GPU_DRAW);
    esp_err_t ret = gpu_validate_draw_state(context);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!context->bound_index_buffer) {
        return ESP_ERR_INVALID_STATE;
    }
    if (index_count == 0U || index_count % 3U != 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    return grape_gpu_raster_draw_indexed(context, first_index, index_count, vertex_offset);
}
