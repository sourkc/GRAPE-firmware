#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "grape/grape_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRAPE_GPU_MAX_VERTEX_ATTRIBUTES 8U
#define GRAPE_GPU_MAX_PUSH_CONSTANT_BYTES 128U
#define GRAPE_GPU_MAX_TEXTURE_SLOTS 4U

typedef struct grape_gpu_context grape_gpu_context_t;
typedef struct grape_gpu_buffer grape_gpu_buffer_t;
typedef struct grape_gpu_pipeline grape_gpu_pipeline_t;
typedef struct grape_gpu_depth_buffer grape_gpu_depth_buffer_t;

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
    GRAPE_GPU_INDEX_U16 = 0,
    GRAPE_GPU_INDEX_U32,
} grape_gpu_index_type_t;

typedef enum {
    GRAPE_GPU_TOPOLOGY_TRIANGLE_LIST = 0,
} grape_gpu_primitive_topology_t;

typedef enum {
    GRAPE_GPU_VERTEX_PROGRAM_CLIP_SPACE = 0,
    GRAPE_GPU_VERTEX_PROGRAM_MVP,
} grape_gpu_vertex_program_t;

typedef enum {
    GRAPE_GPU_FRAGMENT_PROGRAM_SOLID_COLOR = 0,
    GRAPE_GPU_FRAGMENT_PROGRAM_PUSH_COLOR,
    GRAPE_GPU_FRAGMENT_PROGRAM_VERTEX_COLOR,
    GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE,
    GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR,
} grape_gpu_fragment_program_t;

typedef enum {
    GRAPE_GPU_FILTER_NEAREST = 0,
    GRAPE_GPU_FILTER_LINEAR,
} grape_gpu_filter_t;

typedef enum {
    GRAPE_GPU_ADDRESS_CLAMP = 0,
    GRAPE_GPU_ADDRESS_REPEAT,
} grape_gpu_address_mode_t;

typedef struct {
    grape_gpu_filter_t filter;
    grape_gpu_address_mode_t address_u;
    grape_gpu_address_mode_t address_v;
} grape_gpu_sampler_desc_t;

typedef enum {
    GRAPE_GPU_CULL_NONE = 0,
    GRAPE_GPU_CULL_FRONT,
    GRAPE_GPU_CULL_BACK,
} grape_gpu_cull_mode_t;

typedef enum {
    GRAPE_GPU_FRONT_FACE_CCW = 0,
    GRAPE_GPU_FRONT_FACE_CW,
} grape_gpu_front_face_t;

typedef enum {
    GRAPE_GPU_COMPARE_LESS = 0,
    GRAPE_GPU_COMPARE_LEQUAL,
    GRAPE_GPU_COMPARE_EQUAL,
    GRAPE_GPU_COMPARE_GEQUAL,
    GRAPE_GPU_COMPARE_GREATER,
    GRAPE_GPU_COMPARE_NEVER,
    GRAPE_GPU_COMPARE_ALWAYS,
} grape_gpu_compare_op_t;

typedef enum {
    GRAPE_GPU_DEPTH_D16 = 0,
} grape_gpu_depth_format_t;

typedef enum {
    GRAPE_GPU_SAMPLE_COUNT_1 = 1,
    GRAPE_GPU_SAMPLE_COUNT_2 = 2,
    GRAPE_GPU_SAMPLE_COUNT_4 = 4,
} grape_gpu_sample_count_t;

typedef enum {
    GRAPE_GPU_LOAD_OP_LOAD = 0,
    GRAPE_GPU_LOAD_OP_CLEAR,
} grape_gpu_load_op_t;

/* Row-major matrix multiplied by a column vector. MVP output uses
 * -w <= x <= w, -w <= y <= w, 0 <= z <= w clip space. */
typedef struct {
    float m[16];
} grape_gpu_mat4_t;

/*
 * Built-in program push-constant ABI.
 *
 * GRAPE_GPU_VERTEX_PROGRAM_MVP reads mvp.
 * GRAPE_GPU_FRAGMENT_PROGRAM_PUSH_COLOR reads color.
 * Applications may still use grape_gpu_set_push_constants() directly.
 */
typedef struct {
    grape_gpu_mat4_t mvp;
    grape_color_t color;
} grape_gpu_builtin_constants_t;

/*
 * Built-in vertex-program ABI locations:
 *   0 - position (F32x2/F32x3/F32x4)
 *   1 - normalized texture coordinates UV (F32x2)
 *   2 - straight normalized vertex color RGBA (F32x4)
 * UV/color are smooth perspective-correct varyings and survive clipping.
 */
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
    uint32_t width;
    uint32_t height;
    grape_gpu_depth_format_t format;
    grape_memory_t memory;
    /* 0 is accepted as GRAPE_GPU_SAMPLE_COUNT_1 for backwards compatibility. */
    grape_gpu_sample_count_t sample_count;
} grape_gpu_depth_buffer_desc_t;

typedef struct {
    bool test_enable;
    bool write_enable;
    grape_gpu_compare_op_t compare_op;
} grape_gpu_depth_state_t;

typedef struct {
    grape_gpu_vertex_layout_t vertex_layout;
    grape_gpu_primitive_topology_t topology;
    grape_gpu_vertex_program_t vertex_program;
    grape_gpu_fragment_program_t fragment_program;
    grape_color_t solid_color;
    grape_gpu_cull_mode_t cull_mode;
    grape_gpu_front_face_t front_face;
    grape_gpu_depth_state_t depth;
    /*
     * Sampler used by built-in texture fragment programs. UVs are normalized:
     * (0,0) is the top-left texture edge and (1,1) the bottom-right edge.
     */
    grape_gpu_sampler_desc_t sampler;
    /* 0 is accepted as GRAPE_GPU_SAMPLE_COUNT_1 for backwards compatibility. */
    grape_gpu_sample_count_t sample_count;
} grape_gpu_pipeline_desc_t;

typedef struct {
    grape_texture_t *color_attachment;
    grape_gpu_load_op_t color_load_op;
    grape_color_t clear_color;
    grape_gpu_depth_buffer_t *depth_attachment;
    grape_gpu_load_op_t depth_load_op;
    float clear_depth;
    /*
     * Raster sample count. A multisampled pass resolves into color_attachment
     * when grape_gpu_end_render_pass() is called. 0 defaults to the depth
     * attachment sample count when present, otherwise 1x.
     */
    grape_gpu_sample_count_t sample_count;
} grape_gpu_render_pass_desc_t;

typedef struct {
    float x;
    float y;
    float width;
    float height;
    float min_depth;
    float max_depth;
} grape_gpu_viewport_t;

/*
 * Optional per-render-pass GPU profiling. Profiling is disabled by default so
 * normal rendering does not pay timer/counter overhead. When enabled, stats
 * are reset at begin_render_pass() and the most recently completed pass is
 * exposed through grape_gpu_get_stats().
 *
 * Timings are elapsed microseconds spent inside the named GPU stages.
 * tile_raster_us and resolve_us sum per-tile elapsed time across both workers
 * (including preemption); they are not parallel batch wall time. Use pass_us
 * and benchmark work time for single/dual-core speedup comparisons.
 * Structural counters are exact for the submitted pass. triangle_bbox_pixels
 * is deliberately an overdraw/work estimate: it sums the screen-space bounding
 * box area of triangles that reached raster setup and is not a fragment count.
 */
typedef struct {
    uint64_t pass_us;
    uint64_t vertex_transform_us;
    uint64_t clip_us;
    uint64_t triangle_setup_us;
    uint64_t tile_bin_us;
    uint64_t tile_raster_us;
    uint64_t resolve_us;

    uint32_t draw_calls;
    uint32_t input_triangles;
    uint32_t clipped_away_triangles;
    uint32_t post_clip_triangles;
    uint32_t culled_triangles;
    uint32_t degenerate_triangles;
    uint32_t rasterized_triangles;
    uint32_t active_tiles;
    uint32_t tile_references;
    uint64_t triangle_bbox_pixels;
} grape_gpu_stats_t;

/* GPU APIs remain externally serialized on CPU0, including resource mutation,
 * stats access and destruction. Only prepared MSAA tiles run on the helper;
 * end_render_pass joins it before returning, also on failure. */
esp_err_t grape_gpu_context_create(grape_context_t *grape, grape_gpu_context_t **out_context);
esp_err_t grape_gpu_context_destroy(grape_gpu_context_t *context);

/* Experimental accelerator diagnostics are cumulative per GPU context and
 * independent of ordinary pass statistics. Use from the GPU's owning task. */
typedef struct {
    uint64_t triangles_seen;
    uint64_t triangles_completed;
    uint64_t tiles_computed; /* CSC/PPA block pairs, including enlarged blocks. */
    uint64_t large_tiles_computed; /* 32x32 or 64x64 block pairs. */
    uint64_t pixels_computed; /* Includes padded pixels, excludes device probes. */
    uint64_t cpu_tiles_computed; /* Blocks assigned to CPU while transfers are pending. */
    uint64_t cpu_pixels_computed; /* Bounding-box pixels handled by CPU, including uncovered. */
    uint64_t overlap_triangles; /* Completed triangles using both CPU and hardware blocks. */
    uint64_t prepare_us; /* Coefficient preflight and coordinate preparation, excludes probes. */
    uint64_t driver_us; /* Submission and cache maintenance, excludes waits and CPU rendering. */
    uint64_t wait_us; /* Time inside blocking completion waits. */
    uint64_t cpu_render_us; /* CPU raster chunks, including finishing a partial block. */
    uint64_t validation_us;
    uint64_t commit_us;
    uint64_t fallback_unsupported;
    uint64_t fallback_coefficients;
    uint64_t pixels_validated;
    uint64_t validation_mismatches;
    uint64_t compute_us; /* Completed jobs: hardware wait, validation, CPU commit.
                         * Excludes initial probe and coefficient preflight. */
    uint32_t errors;
    uint32_t timeouts;
    esp_err_t last_error;
    int round_bias;
    bool ready;
} grape_gpu_ppa_triangle_stats_t;

/* Feature must be enabled. Call outside a render pass; otherwise the first
 * eligible triangle runs this initialization/probe automatically. */
esp_err_t grape_gpu_ppa_triangle_self_test(grape_gpu_context_t *context);
esp_err_t grape_gpu_get_ppa_triangle_stats(const grape_gpu_context_t *context,
                                           grape_gpu_ppa_triangle_stats_t *out);

void grape_gpu_set_stats_enabled(grape_gpu_context_t *context, bool enabled);
bool grape_gpu_stats_enabled(const grape_gpu_context_t *context);
esp_err_t grape_gpu_get_stats(const grape_gpu_context_t *context,
                              grape_gpu_stats_t *out_stats);

esp_err_t grape_gpu_buffer_create(grape_gpu_context_t *context,
                                  const grape_gpu_buffer_desc_t *desc,
                                  grape_gpu_buffer_t **out_buffer);
esp_err_t grape_gpu_buffer_destroy(grape_gpu_buffer_t *buffer);
esp_err_t grape_gpu_buffer_write(grape_gpu_buffer_t *buffer,
                                 size_t offset,
                                 const void *data,
                                 size_t size);
size_t grape_gpu_buffer_size(const grape_gpu_buffer_t *buffer);

esp_err_t grape_gpu_depth_buffer_create(grape_gpu_context_t *context,
                                        const grape_gpu_depth_buffer_desc_t *desc,
                                        grape_gpu_depth_buffer_t **out_buffer);
esp_err_t grape_gpu_depth_buffer_destroy(grape_gpu_depth_buffer_t *buffer);
uint32_t grape_gpu_depth_buffer_width(const grape_gpu_depth_buffer_t *buffer);
uint32_t grape_gpu_depth_buffer_height(const grape_gpu_depth_buffer_t *buffer);
grape_gpu_sample_count_t grape_gpu_depth_buffer_sample_count(const grape_gpu_depth_buffer_t *buffer);

esp_err_t grape_gpu_pipeline_create(grape_gpu_context_t *context,
                                    const grape_gpu_pipeline_desc_t *desc,
                                    grape_gpu_pipeline_t **out_pipeline);
esp_err_t grape_gpu_pipeline_destroy(grape_gpu_pipeline_t *pipeline);

esp_err_t grape_gpu_begin_render_pass(grape_gpu_context_t *context,
                                      const grape_gpu_render_pass_desc_t *desc);
esp_err_t grape_gpu_end_render_pass(grape_gpu_context_t *context);
esp_err_t grape_gpu_set_viewport(grape_gpu_context_t *context,
                                 const grape_gpu_viewport_t *viewport);
esp_err_t grape_gpu_set_push_constants(grape_gpu_context_t *context,
                                       uint32_t offset,
                                       const void *data,
                                       size_t size);
esp_err_t grape_gpu_bind_pipeline(grape_gpu_context_t *context,
                                  grape_gpu_pipeline_t *pipeline);
esp_err_t grape_gpu_bind_vertex_buffer(grape_gpu_context_t *context,
                                       grape_gpu_buffer_t *buffer);
esp_err_t grape_gpu_bind_index_buffer(grape_gpu_context_t *context,
                                      grape_gpu_buffer_t *buffer,
                                      grape_gpu_index_type_t index_type);
/* Built-in texture fragment programs currently sample slot 0. */
esp_err_t grape_gpu_bind_texture(grape_gpu_context_t *context,
                                 uint32_t slot,
                                 grape_texture_t *texture);
esp_err_t grape_gpu_draw(grape_gpu_context_t *context,
                         uint32_t first_vertex,
                         uint32_t vertex_count);
esp_err_t grape_gpu_draw_indexed(grape_gpu_context_t *context,
                                 uint32_t first_index,
                                 uint32_t index_count,
                                 int32_t vertex_offset);

#ifdef __cplusplus
}
#endif
