#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "grape_gpu_internal.h"

static uint32_t gpu_buffer_caps(grape_memory_t memory)
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

esp_err_t grape_gpu_buffer_create(grape_gpu_context_t *context,
                                  const grape_gpu_buffer_desc_t *desc,
                                  grape_gpu_buffer_t **out_buffer)
{
    if (!context || !desc || !out_buffer || desc->size == 0U ||
        desc->usage < GRAPE_GPU_BUFFER_VERTEX ||
        desc->usage > GRAPE_GPU_BUFFER_UNIFORM ||
        desc->memory < GRAPE_MEMORY_DEFAULT ||
        desc->memory > GRAPE_MEMORY_PSRAM) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_gpu_buffer_t *buffer = calloc(1, sizeof(*buffer));
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    buffer->data = heap_caps_malloc(desc->size, gpu_buffer_caps(desc->memory));
    if (!buffer->data) {
        free(buffer);
        return ESP_ERR_NO_MEM;
    }

    buffer->context = context;
    buffer->size = desc->size;
    buffer->usage = desc->usage;
    buffer->memory = desc->memory;
    buffer->next = context->buffers;
    context->buffers = buffer;

    *out_buffer = buffer;
    return ESP_OK;
}

esp_err_t grape_gpu_buffer_destroy(grape_gpu_buffer_t *buffer)
{
    if (!buffer) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_gpu_context_t *context = buffer->context;
    if (context->render_pass_active &&
        (context->bound_vertex_buffer == buffer || context->bound_index_buffer == buffer)) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_gpu_buffer_t **cursor = &context->buffers;
    while (*cursor && *cursor != buffer) {
        cursor = &(*cursor)->next;
    }
    if (*cursor != buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    *cursor = buffer->next;
    if (context->bound_vertex_buffer == buffer) {
        context->bound_vertex_buffer = NULL;
    }
    if (context->bound_index_buffer == buffer) {
        context->bound_index_buffer = NULL;
    }
    heap_caps_free(buffer->data);
    free(buffer);
    return ESP_OK;
}

esp_err_t grape_gpu_buffer_write(grape_gpu_buffer_t *buffer,
                                 size_t offset,
                                 const void *data,
                                 size_t size)
{
    if (!buffer || (!data && size != 0U) || offset > buffer->size ||
        size > buffer->size - offset) {
        return ESP_ERR_INVALID_ARG;
    }

    if (size != 0U) {
        memcpy(buffer->data + offset, data, size);
    }
    return ESP_OK;
}

size_t grape_gpu_buffer_size(const grape_gpu_buffer_t *buffer)
{
    return buffer ? buffer->size : 0U;
}
