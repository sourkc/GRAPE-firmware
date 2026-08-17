#pragma once

#include <stddef.h>
#include <stdint.h>

#include "grape/grape_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRAPE_GPU_MAX_VERTEX_ATTRIBUTES 8U

typedef struct grape_gpu_context grape_gpu_context_t;
typedef struct grape_gpu_buffer grape_gpu_buffer_t;
typedef struct grape_gpu_pipeline grape_gpu_pipeline_t;

typedef enum {
    GRAPE_GPU_BUFFER_VERTEX = 0,
    GRAPE_GPU_BUFFER_INDEX,
    GRAPE_GPU_BUFFER_UNIFORM,
} grape_gpu_buffer_usage_t;

typedef enum {
    GRAPE_GPU_VERTEX_FORMAT_F32X2 = 0,
    GRAPE_GPU_VERTEX_FORMAT_F32X3,
    GRAPE_GPU_VERTEX_FORMAT_F32X4,
} grape_gpu_vertex_format_t;

typedef enum {
    GRAPE_GPU_TOPOLOGY_TRIANGLE_LIST = 0,
} grape_gpu_primitive_topology_t;

typedef enum {
    GRAPE_GPU_VERTEX_PROGRAM_CLIP_SPACE = 0,
} grape_gpu_vertex_program_t;

typedef enum {
    GRAPE_GPU_FRAGMENT_PROGRAM_SOLID_COLOR = 0,
} grape_gpu_fragment_program_t;

typedef enum {
    GRAPE_GPU_LOAD_OP_LOAD = 0,
    GRAPE_GPU_LOAD_OP_CLEAR,
} grape_gpu_load_op_t;

typedef struct {
    uint32_t location;
    grape_gpu_vertex_format_t format;
    uint32_t offset;
} grape_gpu_vertex_attribute_t;

typedef struct {
    uint32_t stride;
    uint32_t attribute_count;
    grape_gpu_vertex_attribute_t attributes[GRAPE_GPU_MAX_VERTEX_ATTRIBUTES];
} grape_gpu_vertex_layout_t;

typedef struct {
    size_t size;
    grape_gpu_buffer_usage_t usage;
    grape_memory_t memory;
} grape_gpu_buffer_desc_t;

typedef struct {
    grape_gpu_vertex_layout_t vertex_layout;
    grape_gpu_primitive_topology_t topology;
    grape_gpu_vertex_program_t vertex_program;
    grape_gpu_fragment_program_t fragment_program;
    grape_color_t solid_color;
} grape_gpu_pipeline_desc_t;

typedef struct {
    grape_texture_t *color_attachment;
    grape_gpu_load_op_t color_load_op;
    grape_color_t clear_color;
} grape_gpu_render_pass_desc_t;

typedef struct {
    float x;
    float y;
    float width;
    float height;
    float min_depth;
    float max_depth;
} grape_gpu_viewport_t;

esp_err_t grape_gpu_context_create(grape_context_t *grape, grape_gpu_context_t **out_context);
esp_err_t grape_gpu_context_destroy(grape_gpu_context_t *context);

esp_err_t grape_gpu_buffer_create(grape_gpu_context_t *context,
                                  const grape_gpu_buffer_desc_t *desc,
                                  grape_gpu_buffer_t **out_buffer);
esp_err_t grape_gpu_buffer_destroy(grape_gpu_buffer_t *buffer);
esp_err_t grape_gpu_buffer_write(grape_gpu_buffer_t *buffer,
                                 size_t offset,
                                 const void *data,
                                 size_t size);
size_t grape_gpu_buffer_size(const grape_gpu_buffer_t *buffer);

esp_err_t grape_gpu_pipeline_create(grape_gpu_context_t *context,
                                    const grape_gpu_pipeline_desc_t *desc,
                                    grape_gpu_pipeline_t **out_pipeline);
esp_err_t grape_gpu_pipeline_destroy(grape_gpu_pipeline_t *pipeline);

esp_err_t grape_gpu_begin_render_pass(grape_gpu_context_t *context,
                                      const grape_gpu_render_pass_desc_t *desc);
esp_err_t grape_gpu_end_render_pass(grape_gpu_context_t *context);
esp_err_t grape_gpu_set_viewport(grape_gpu_context_t *context,
                                 const grape_gpu_viewport_t *viewport);
esp_err_t grape_gpu_bind_pipeline(grape_gpu_context_t *context,
                                  grape_gpu_pipeline_t *pipeline);
esp_err_t grape_gpu_bind_vertex_buffer(grape_gpu_context_t *context,
                                       grape_gpu_buffer_t *buffer);
esp_err_t grape_gpu_draw(grape_gpu_context_t *context,
                         uint32_t first_vertex,
                         uint32_t vertex_count);

#ifdef __cplusplus
}
#endif
