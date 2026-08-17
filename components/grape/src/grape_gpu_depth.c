#include <stdlib.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "grape_gpu_internal.h"

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

esp_err_t grape_gpu_depth_buffer_create(grape_gpu_context_t *context,
                                        const grape_gpu_depth_buffer_desc_t *desc,
                                        grape_gpu_depth_buffer_t **out_buffer)
{
    if (!context || !desc || !out_buffer || desc->width == 0U || desc->height == 0U ||
        desc->format != GRAPE_GPU_DEPTH_D16 ||
        desc->memory < GRAPE_MEMORY_DEFAULT || desc->memory > GRAPE_MEMORY_PSRAM) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t stride = (size_t)desc->width * sizeof(uint16_t);
    if (stride / sizeof(uint16_t) != desc->width) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t size = stride * (size_t)desc->height;
    if (size / stride != desc->height) {
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

    buffer->context = context;
    buffer->stride = stride;
    buffer->size = size;
    buffer->width = desc->width;
    buffer->height = desc->height;
    buffer->format = desc->format;
    buffer->memory = desc->memory;
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
