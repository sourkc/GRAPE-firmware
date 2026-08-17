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

size_t grape_gpu_index_type_size_internal(grape_gpu_index_type_t type)
{
    switch (type) {
        case GRAPE_GPU_INDEX_U16:
            return sizeof(uint16_t);
        case GRAPE_GPU_INDEX_U32:
            return sizeof(uint32_t);
        default:
            return 0U;
    }
}

static bool gpu_fragment_uses_texture(grape_gpu_fragment_program_t program)
{
    return program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE ||
           program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR;
}

static bool gpu_fragment_uses_vertex_color(grape_gpu_fragment_program_t program)
{
    return program == GRAPE_GPU_FRAGMENT_PROGRAM_VERTEX_COLOR ||
           program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR;
}

esp_err_t grape_gpu_pipeline_create(grape_gpu_context_t *context,
                                    const grape_gpu_pipeline_desc_t *desc,
                                    grape_gpu_pipeline_t **out_pipeline)
{
    if (!context || !desc || !out_pipeline ||
        desc->topology != GRAPE_GPU_TOPOLOGY_TRIANGLE_LIST ||
        desc->vertex_program < GRAPE_GPU_VERTEX_PROGRAM_CLIP_SPACE ||
        desc->vertex_program > GRAPE_GPU_VERTEX_PROGRAM_MVP ||
        desc->fragment_program < GRAPE_GPU_FRAGMENT_PROGRAM_SOLID_COLOR ||
        desc->fragment_program > GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR ||
        desc->cull_mode < GRAPE_GPU_CULL_NONE || desc->cull_mode > GRAPE_GPU_CULL_BACK ||
        desc->front_face < GRAPE_GPU_FRONT_FACE_CCW || desc->front_face > GRAPE_GPU_FRONT_FACE_CW ||
        desc->depth.compare_op < GRAPE_GPU_COMPARE_LESS ||
        desc->depth.compare_op > GRAPE_GPU_COMPARE_ALWAYS ||
        desc->sampler.filter < GRAPE_GPU_FILTER_NEAREST ||
        desc->sampler.filter > GRAPE_GPU_FILTER_LINEAR ||
        desc->sampler.address_u < GRAPE_GPU_ADDRESS_CLAMP ||
        desc->sampler.address_u > GRAPE_GPU_ADDRESS_REPEAT ||
        desc->sampler.address_v < GRAPE_GPU_ADDRESS_CLAMP ||
        desc->sampler.address_v > GRAPE_GPU_ADDRESS_REPEAT ||
        !grape_gpu_sample_count_valid(desc->sample_count) ||
        desc->vertex_layout.stride == 0U ||
        desc->vertex_layout.attribute_count == 0U ||
        desc->vertex_layout.attribute_count > GRAPE_GPU_MAX_VERTEX_ATTRIBUTES) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t position_index = UINT32_MAX;
    uint32_t texcoord_index = UINT32_MAX;
    uint32_t color_index = UINT32_MAX;
    for (uint32_t i = 0U; i < desc->vertex_layout.attribute_count; ++i) {
        const grape_gpu_vertex_attribute_t *attribute = &desc->vertex_layout.attributes[i];
        const size_t attribute_size = grape_gpu_vertex_format_size_internal(attribute->format);
        if (attribute_size == 0U || attribute->offset > desc->vertex_layout.stride ||
            attribute_size > desc->vertex_layout.stride - attribute->offset) {
            return ESP_ERR_INVALID_ARG;
        }

        uint32_t *known_index = NULL;
        if (attribute->location == 0U) {
            known_index = &position_index;
        } else if (attribute->location == 1U) {
            known_index = &texcoord_index;
        } else if (attribute->location == 2U) {
            known_index = &color_index;
        }
        if (known_index) {
            if (*known_index != UINT32_MAX) {
                return ESP_ERR_INVALID_ARG;
            }
            *known_index = i;
        }
    }

    if (position_index == UINT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (gpu_fragment_uses_texture(desc->fragment_program)) {
        if (texcoord_index == UINT32_MAX ||
            desc->vertex_layout.attributes[texcoord_index].format != GRAPE_GPU_VERTEX_FORMAT_F32X2) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (gpu_fragment_uses_vertex_color(desc->fragment_program)) {
        if (color_index == UINT32_MAX ||
            desc->vertex_layout.attributes[color_index].format != GRAPE_GPU_VERTEX_FORMAT_F32X4) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    grape_gpu_pipeline_t *pipeline = calloc(1, sizeof(*pipeline));
    if (!pipeline) {
        return ESP_ERR_NO_MEM;
    }

    pipeline->context = context;
    pipeline->desc = *desc;
    pipeline->desc.sample_count = grape_gpu_sample_count_resolve(desc->sample_count);
    pipeline->position_attribute_index = position_index;
    pipeline->texcoord_attribute_index = texcoord_index;
    pipeline->color_attribute_index = color_index;
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
