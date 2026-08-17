#pragma once

#include "grape/grape_gpu.h"

#define GRAPE_GPU_MAX_CLIPPED_VERTICES 12U

typedef struct {
    float x;
    float y;
    float z;
    float w;
} grape_gpu_clip_vertex_t;

struct grape_gpu_buffer {
    grape_gpu_context_t *context;
    struct grape_gpu_buffer *next;
    uint8_t *data;
    size_t size;
    grape_gpu_buffer_usage_t usage;
    grape_memory_t memory;
};

struct grape_gpu_depth_buffer {
    grape_gpu_context_t *context;
    struct grape_gpu_depth_buffer *next;
    uint16_t *data;
    size_t stride;
    size_t size;
    uint32_t width;
    uint32_t height;
    grape_gpu_depth_format_t format;
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
    grape_gpu_depth_buffer_t *depth_buffers;
    grape_gpu_pipeline_t *pipelines;
    grape_texture_t *color_attachment;
    grape_gpu_depth_buffer_t *depth_attachment;
    grape_gpu_pipeline_t *bound_pipeline;
    grape_gpu_buffer_t *bound_vertex_buffer;
    grape_gpu_buffer_t *bound_index_buffer;
    grape_gpu_index_type_t bound_index_type;
    grape_gpu_viewport_t viewport;
    uint8_t push_constants[GRAPE_GPU_MAX_PUSH_CONSTANT_BYTES];
    grape_rect_t dirty_rect;
    bool render_pass_active;
    bool dirty_valid;
};

size_t grape_gpu_vertex_format_size_internal(grape_gpu_vertex_format_t format);
size_t grape_gpu_index_type_size_internal(grape_gpu_index_type_t type);
void grape_gpu_dirty_add(grape_gpu_context_t *context, grape_rect_t rect);

esp_err_t grape_gpu_vertex_fetch_transform(const grape_gpu_context_t *context,
                                           uint32_t vertex_index,
                                           grape_gpu_clip_vertex_t *out_vertex);
uint32_t grape_gpu_clip_triangle(const grape_gpu_clip_vertex_t input[3],
                                 grape_gpu_clip_vertex_t output[GRAPE_GPU_MAX_CLIPPED_VERTICES]);
bool grape_gpu_triangle_culled(const grape_gpu_pipeline_t *pipeline,
                               const grape_gpu_clip_vertex_t triangle[3]);

esp_err_t grape_gpu_raster_triangle(grape_gpu_context_t *context,
                                    const grape_gpu_clip_vertex_t triangle[3]);
esp_err_t grape_gpu_raster_draw(grape_gpu_context_t *context,
                                uint32_t first_vertex,
                                uint32_t vertex_count);
esp_err_t grape_gpu_raster_draw_indexed(grape_gpu_context_t *context,
                                        uint32_t first_index,
                                        uint32_t index_count,
                                        int32_t vertex_offset);
