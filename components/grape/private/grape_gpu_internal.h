#pragma once

#include "grape/grape_gpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_GRAPE_GPU_MULTICORE && !CONFIG_FREERTOS_UNICORE && !CONFIG_GRAPE_FUNCTION_PROFILING
#define GRAPE_GPU_MULTICORE 1
#else
#define GRAPE_GPU_MULTICORE 0
#endif

#define GRAPE_GPU_MAX_CLIPPED_VERTICES 12U
#define GRAPE_GPU_MSAA_TILE_SIZE 16U

typedef struct {
    float x;
    float y;
    float z;
    float w;
    float u;
    float v;
    float color[4];
} grape_gpu_clip_vertex_t;

typedef struct {
    float row_start;
    float step_x;
    float step_y;
} grape_gpu_interp_plane_t;

typedef struct {
    int32_t x;
    int32_t y;
} grape_gpu_fixed_point_t;

typedef struct {
    grape_gpu_fixed_point_t fixed[3];
    float sx[3];
    float sy[3];
    float depth[3];
    int32_t min_x;
    int32_t max_x;
    int32_t min_y;
    int32_t max_y;
    int64_t row_e0;
    int64_t row_e1;
    int64_t row_e2;
    int64_t e0_step_x;
    int64_t e1_step_x;
    int64_t e2_step_x;
    int64_t e0_step_y;
    int64_t e1_step_y;
    int64_t e2_step_y;
    float depth_row_start;
    float depth_step_x;
    float depth_step_y;
    grape_gpu_interp_plane_t inv_w;
    grape_gpu_interp_plane_t u_over_w;
    grape_gpu_interp_plane_t v_over_w;
    grape_gpu_interp_plane_t color_over_w[4];
} grape_gpu_triangle_setup_t;

typedef struct {
    grape_gpu_triangle_setup_t setup;

    int32_t row_e[3];
    int32_t edge_step_x[3];
    int32_t edge_step_y[3];
    int32_t sample_edge_bias[3][4];
    int32_t min_sample_edge_bias[3];

    int32_t depth_row_start_fp;
    int32_t depth_step_x_fp;
    int32_t depth_step_y_fp;
    int32_t sample_depth_bias_fp[4];

    int64_t sample_e0_bias_fallback[4];
    int64_t sample_e1_bias_fallback[4];
    int64_t sample_e2_bias_fallback[4];
    float sample_depth_bias_fallback[4];

    grape_color_t color;
    grape_gpu_depth_state_t depth;
    grape_gpu_fragment_program_t fragment_program;
    grape_texture_t *texture;
    grape_gpu_sampler_desc_t sampler;
    bool raster_i32_valid;
} grape_gpu_prepared_triangle_t;

typedef struct {
    uint32_t tile_x;
    uint32_t tile_y;
    uint32_t ref_begin;
    uint32_t ref_end;
} grape_gpu_tile_job_t;

typedef struct {
    uint32_t *tile_color;
    uint16_t *tile_depth;
    size_t tile_color_capacity;
    size_t tile_depth_capacity;
    /* Written only by this worker; CPU0 merges after the batch fence. */
    uint64_t tile_raster_us;
    uint64_t resolve_us;
    grape_rect_t dirty_rect;
    bool dirty_valid;
    esp_err_t result;
} grape_gpu_worker_t;

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
    grape_gpu_sample_count_t sample_count;
    grape_memory_t memory;
    uint8_t *lazy_tiles;
    uint32_t tile_cols;
    uint32_t tile_rows;
    size_t tile_count;
    uint16_t lazy_clear_value;
    bool lazy_clear_active;
};

struct grape_gpu_pipeline {
    grape_gpu_context_t *context;
    struct grape_gpu_pipeline *next;
    grape_gpu_pipeline_desc_t desc;
    uint32_t position_attribute_index;
    uint32_t texcoord_attribute_index;
    uint32_t color_attribute_index;
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
    grape_texture_t *bound_textures[GRAPE_GPU_MAX_TEXTURE_SLOTS];
    grape_gpu_viewport_t viewport;
    uint8_t push_constants[GRAPE_GPU_MAX_PUSH_CONSTANT_BYTES];
    grape_gpu_prepared_triangle_t *tile_primitives;
    size_t tile_primitive_count;
    size_t tile_primitive_capacity;
    uint32_t *tile_counts;
    uint32_t *tile_offsets;
    uint32_t *tile_refs;
    grape_gpu_tile_job_t *tile_jobs;
    size_t tile_meta_capacity;
    size_t tile_ref_capacity;
    size_t tile_job_count;
    size_t tile_job_capacity;
    size_t tile_job_next; /* Atomic while a batch is active. */
    bool tile_jobs_cancelled; /* Atomic; stops new claims after an error. */
    uint32_t tile_cols;
    uint32_t tile_rows;
    grape_gpu_worker_t primary_worker;
#if GRAPE_GPU_MULTICORE
    grape_gpu_worker_t secondary_worker;
    TaskHandle_t secondary_task;
    SemaphoreHandle_t secondary_wake;
    SemaphoreHandle_t secondary_done;
    bool secondary_stop; /* Published through secondary_wake. */
#endif
    grape_gpu_load_op_t tile_color_load_op;
    grape_color_t tile_clear_color;
    grape_gpu_sample_count_t sample_count;
    grape_rect_t dirty_rect;
    grape_gpu_stats_t current_stats;
    grape_gpu_stats_t last_stats;
    int64_t stats_pass_start_us;
    bool stats_enabled;
    bool render_pass_active;
    bool dirty_valid;
};

size_t grape_gpu_vertex_format_size_internal(grape_gpu_vertex_format_t format);
size_t grape_gpu_index_type_size_internal(grape_gpu_index_type_t type);
grape_gpu_sample_count_t grape_gpu_sample_count_resolve(grape_gpu_sample_count_t sample_count);
bool grape_gpu_sample_count_valid(grape_gpu_sample_count_t sample_count);
void grape_gpu_dirty_add(grape_gpu_context_t *context, grape_rect_t rect);

esp_err_t grape_gpu_tile_begin(grape_gpu_context_t *context,
                               grape_gpu_load_op_t load_op,
                               grape_color_t clear_color);
esp_err_t grape_gpu_tile_enqueue(grape_gpu_context_t *context,
                                 const grape_gpu_triangle_setup_t *setup,
                                 grape_color_t color,
                                 grape_gpu_depth_state_t depth,
                                 grape_gpu_fragment_program_t fragment_program,
                                 grape_texture_t *texture,
                                 grape_gpu_sampler_desc_t sampler);
esp_err_t grape_gpu_tile_execute(grape_gpu_context_t *context);
void grape_gpu_tile_release(grape_gpu_context_t *context);

esp_err_t grape_gpu_depth_begin_pass(grape_gpu_depth_buffer_t *buffer,
                                     grape_gpu_load_op_t load_op,
                                     float clear_depth);
void grape_gpu_depth_load_tile(const grape_gpu_depth_buffer_t *buffer,
                               uint32_t tile_x,
                               uint32_t tile_y,
                               uint16_t *dst,
                               uint32_t dst_stride_values);
void grape_gpu_depth_store_tile(grape_gpu_depth_buffer_t *buffer,
                                uint32_t tile_x,
                                uint32_t tile_y,
                                const uint16_t *src,
                                uint32_t src_stride_values);

esp_err_t grape_gpu_shade_fragment(const grape_gpu_triangle_setup_t *setup,
                                  grape_gpu_fragment_program_t program,
                                  const grape_texture_t *texture,
                                  const grape_gpu_sampler_desc_t *sampler,
                                  float screen_x,
                                  float screen_y,
                                  grape_color_t *out_color);

esp_err_t grape_gpu_sample_texture(const grape_texture_t *texture,
                                   const grape_gpu_sampler_desc_t *sampler,
                                   float u,
                                   float v,
                                   grape_color_t *out_color);

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
