#include <stdlib.h>

#include "grape_gpu_internal.h"

size_t grape_gpu_vertex_format_size_internal(grape_gpu_vertex_format_t format)
{
    switch (format) {
        case GRAPE_GPU_VERTEX_FORMAT_F32X2:
            return sizeof(float) * 2U;
        case GRAPE_GPU_VERTEX_FORMAT_F32X3:
            return sizeof(float) * 3U;
        case GRAPE_GPU_VERTEX_FORMAT_F32X4:
            return sizeof(float) * 4U;
        default:
            return 0U;
    }
}

esp_err_t grape_gpu_pipeline_create(grape_gpu_context_t *context,
                                    const grape_gpu_pipeline_desc_t *desc,
                                    grape_gpu_pipeline_t **out_pipeline)
{
    if (!context || !desc || !out_pipeline ||
        desc->topology != GRAPE_GPU_TOPOLOGY_TRIANGLE_LIST ||
        desc->vertex_program != GRAPE_GPU_VERTEX_PROGRAM_CLIP_SPACE ||
        desc->fragment_program != GRAPE_GPU_FRAGMENT_PROGRAM_SOLID_COLOR ||
        desc->vertex_layout.stride == 0U ||
        desc->vertex_layout.attribute_count == 0U ||
        desc->vertex_layout.attribute_count > GRAPE_GPU_MAX_VERTEX_ATTRIBUTES) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t position_index = UINT32_MAX;
    for (uint32_t i = 0U; i < desc->vertex_layout.attribute_count; ++i) {
        const grape_gpu_vertex_attribute_t *attribute = &desc->vertex_layout.attributes[i];
        size_t attribute_size = grape_gpu_vertex_format_size_internal(attribute->format);
        if (attribute_size == 0U || attribute->offset > desc->vertex_layout.stride ||
            attribute_size > desc->vertex_layout.stride - attribute->offset) {
            return ESP_ERR_INVALID_ARG;
        }
        if (attribute->location == 0U) {
            if (position_index != UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            position_index = i;
        }
    }

    if (position_index == UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_gpu_pipeline_t *pipeline = calloc(1, sizeof(*pipeline));
    if (!pipeline) {
        return ESP_ERR_NO_MEM;
    }

    pipeline->context = context;
    pipeline->desc = *desc;
    pipeline->position_attribute_index = position_index;
    pipeline->next = context->pipelines;
    context->pipelines = pipeline;

    *out_pipeline = pipeline;
    return ESP_OK;
}

esp_err_t grape_gpu_pipeline_destroy(grape_gpu_pipeline_t *pipeline)
{
    if (!pipeline) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_gpu_context_t *context = pipeline->context;
    if (context->render_pass_active && context->bound_pipeline == pipeline) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_gpu_pipeline_t **cursor = &context->pipelines;
    while (*cursor && *cursor != pipeline) {
        cursor = &(*cursor)->next;
    }
    if (*cursor != pipeline) {
        return ESP_ERR_INVALID_STATE;
    }

    *cursor = pipeline->next;
    if (context->bound_pipeline == pipeline) {
        context->bound_pipeline = NULL;
    }
    free(pipeline);
    return ESP_OK;
}
