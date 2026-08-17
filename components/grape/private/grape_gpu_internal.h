#pragma once

#include "grape/grape_gpu.h"

struct grape_gpu_buffer {
    grape_gpu_context_t *context;
    struct grape_gpu_buffer *next;
    uint8_t *data;
    size_t size;
    grape_gpu_buffer_usage_t usage;
    grape_memory_t memory;
};

struct grape_gpu_pipeline {
    grape_gpu_context_t *context;
    struct grape_gpu_pipeline *next;
    grape_gpu_pipeline_desc_t desc;
    uint32_t position_attribute_index;
};

struct grape_gpu_context {
    grape_context_t *grape;
    grape_gpu_buffer_t *buffers;
    grape_gpu_pipeline_t *pipelines;
    grape_texture_t *color_attachment;
    grape_gpu_pipeline_t *bound_pipeline;
    grape_gpu_buffer_t *bound_vertex_buffer;
    grape_gpu_viewport_t viewport;
    grape_rect_t dirty_rect;
    bool render_pass_active;
    bool dirty_valid;
};

size_t grape_gpu_vertex_format_size_internal(grape_gpu_vertex_format_t format);
void grape_gpu_dirty_add(grape_gpu_context_t *context, grape_rect_t rect);
esp_err_t grape_gpu_raster_draw(grape_gpu_context_t *context,
                                uint32_t first_vertex,
                                uint32_t vertex_count);
