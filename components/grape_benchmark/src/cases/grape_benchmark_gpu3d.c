#include "grape_benchmark_internal.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"

#define GPU_BENCH_WIDTH 320U
#define GPU_BENCH_HEIGHT 240U
#define GPU_BENCH_NEAR 0.35f
#define GPU_BENCH_FAR 20.0f
#define GPU_BENCH_FOV_Y 1.1344640138f
#define GPU_BENCH_TEXTURE_SIZE 64U
#define GPU_BENCH_INDEX_COUNT 36U

typedef struct {
    grape_gpu_sample_count_t sample_count;
    uint32_t instances;
    uint32_t shape; /* 0 cube/grid, 1 thin, 2 overdraw, 3 small, 4 near clip */
    bool present;
    bool alternate_load;
} gpu_case_config_t;

typedef struct {
    grape_texture_t *color_target;
    grape_surface_t *surface;
    grape_texture_t *texture;
    grape_gpu_context_t *gpu;
    grape_gpu_depth_buffer_t *depth;
    grape_gpu_buffer_t *vertex_buffer;
    grape_gpu_buffer_t *index_buffer;
    grape_gpu_pipeline_t *pipeline;
    grape_gpu_mat4_t projection;
    const gpu_case_config_t *config;
    grape_gpu_stats_t stats_sum;
    uint32_t measured_passes;
    bool collecting;
} gpu_case_state_t;

typedef struct {
    float x;
    float y;
    float z;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
} gpu_bench_vertex_t;

static grape_gpu_mat4_t mat4_identity(void)
{
    return (grape_gpu_mat4_t) {
        .m = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        },
    };
}

static grape_gpu_mat4_t mat4_multiply(grape_gpu_mat4_t a, grape_gpu_mat4_t b)
{
    grape_gpu_mat4_t result = {0};
    for (uint32_t row = 0U; row < 4U; ++row) {
        for (uint32_t column = 0U; column < 4U; ++column) {
            for (uint32_t k = 0U; k < 4U; ++k) {
                result.m[row * 4U + column] +=
                    a.m[row * 4U + k] * b.m[k * 4U + column];
            }
        }
    }
    return result;
}

static grape_gpu_mat4_t mat4_rotation_x(float angle)
{
    const float c = cosf(angle);
    const float s = sinf(angle);
    grape_gpu_mat4_t result = mat4_identity();
    result.m[5] = c;
    result.m[6] = -s;
    result.m[9] = s;
    result.m[10] = c;
    return result;
}

static grape_gpu_mat4_t mat4_rotation_y(float angle)
{
    const float c = cosf(angle);
    const float s = sinf(angle);
    grape_gpu_mat4_t result = mat4_identity();
    result.m[0] = c;
    result.m[2] = s;
    result.m[8] = -s;
    result.m[10] = c;
    return result;
}

static grape_gpu_mat4_t mat4_translation(float x, float y, float z)
{
    grape_gpu_mat4_t result = mat4_identity();
    result.m[3] = x;
    result.m[7] = y;
    result.m[11] = z;
    return result;
}

static grape_gpu_mat4_t mat4_perspective_lh(float fov_y,
                                             float aspect,
                                             float near_z,
                                             float far_z)
{
    const float f = 1.0f / tanf(fov_y * 0.5f);
    const float depth_scale = far_z / (far_z - near_z);
    return (grape_gpu_mat4_t) {
        .m = {
            f / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, f, 0.0f, 0.0f,
            0.0f, 0.0f, depth_scale, -near_z * depth_scale,
            0.0f, 0.0f, 1.0f, 0.0f,
        },
    };
}

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3U) << 11U) |
                      ((uint16_t)(g >> 2U) << 5U) |
                      (uint16_t)(b >> 3U));
}

static void build_debug_texture(grape_texture_t *texture)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    const size_t stride = grape_texture_stride(texture);
    for (uint32_t y = 0U; y < GPU_BENCH_TEXTURE_SIZE; ++y) {
        uint16_t *row = (uint16_t *)(pixels + (size_t)y * stride);
        for (uint32_t x = 0U; x < GPU_BENCH_TEXTURE_SIZE; ++x) {
            const bool checker = (((x / 8U) ^ (y / 8U)) & 1U) != 0U;
            uint8_t r = checker ? 166U : 239U;
            uint8_t g = checker ? 209U : 159U;
            uint8_t b = checker ? 137U : 118U;

            if (x < 8U && y < 8U) {
                r = 231U; g = 130U; b = 132U;
            } else if (x >= 56U && y < 8U) {
                r = 140U; g = 170U; b = 238U;
            } else if (x < 8U && y >= 56U) {
                r = 229U; g = 200U; b = 144U;
            } else if (x >= 56U && y >= 56U) {
                r = 202U; g = 158U; b = 230U;
            }
            if (x == 31U || x == 32U || y == 31U || y == 32U) {
                r = 198U; g = 208U; b = 245U;
            }
            row[x] = pack_rgb565(r, g, b);
        }
    }
}

#define V(px, py, pz, tu, tv, cr, cg, cb) \
    { (px), (py), (pz), (tu), (tv), (cr), (cg), (cb), 1.0f }

static const gpu_bench_vertex_t s_vertices[] = {
    V(-0.8f,-0.8f,-0.8f, 0.0f,2.0f, 1.00f,0.95f,0.95f),
    V( 0.8f,-0.8f,-0.8f, 2.0f,2.0f, 0.85f,1.00f,0.95f),
    V( 0.8f, 0.8f,-0.8f, 2.0f,0.0f, 0.95f,0.95f,1.00f),
    V(-0.8f, 0.8f,-0.8f, 0.0f,0.0f, 1.00f,1.00f,1.00f),
    V(-0.8f,-0.8f, 0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
    V( 0.8f,-0.8f, 0.8f, 2.0f,2.0f, 0.90f,1.00f,0.90f),
    V( 0.8f, 0.8f, 0.8f, 2.0f,0.0f, 0.90f,0.95f,1.00f),
    V(-0.8f, 0.8f, 0.8f, 0.0f,0.0f, 1.00f,0.90f,0.95f),
    V(-0.8f,-0.8f,-0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
    V(-0.8f,-0.8f, 0.8f, 2.0f,2.0f, 0.90f,1.00f,1.00f),
    V(-0.8f, 0.8f, 0.8f, 2.0f,0.0f, 1.00f,0.90f,1.00f),
    V(-0.8f, 0.8f,-0.8f, 0.0f,0.0f, 1.00f,1.00f,0.90f),
    V( 0.8f,-0.8f,-0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
    V( 0.8f, 0.8f,-0.8f, 0.0f,0.0f, 0.90f,1.00f,1.00f),
    V( 0.8f, 0.8f, 0.8f, 2.0f,0.0f, 1.00f,0.90f,1.00f),
    V( 0.8f,-0.8f, 0.8f, 2.0f,2.0f, 1.00f,1.00f,0.90f),
    V(-0.8f,-0.8f,-0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
    V( 0.8f,-0.8f,-0.8f, 2.0f,2.0f, 0.95f,1.00f,0.90f),
    V( 0.8f,-0.8f, 0.8f, 2.0f,0.0f, 0.90f,0.95f,1.00f),
    V(-0.8f,-0.8f, 0.8f, 0.0f,0.0f, 1.00f,0.90f,0.95f),
    V(-0.8f, 0.8f,-0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
    V(-0.8f, 0.8f, 0.8f, 0.0f,0.0f, 0.90f,1.00f,0.95f),
    V( 0.8f, 0.8f, 0.8f, 2.0f,0.0f, 0.95f,0.90f,1.00f),
    V( 0.8f, 0.8f,-0.8f, 2.0f,2.0f, 1.00f,0.95f,0.90f),
};
#undef V

static const uint16_t s_indices[] = {
     0, 2, 1,   0, 3, 2,
     4, 5, 6,   4, 6, 7,
     8, 9,10,   8,10,11,
    12,13,14,  12,14,15,
    16,17,18,  16,18,19,
    20,21,22,  20,22,23,
};

static void destroy_state(gpu_case_state_t *state)
{
    if (!state) {
        return;
    }
    if (state->surface) grape_surface_destroy(state->surface);
    if (state->pipeline) {
        grape_gpu_pipeline_destroy(state->pipeline);
    }
    if (state->index_buffer) {
        grape_gpu_buffer_destroy(state->index_buffer);
    }
    if (state->vertex_buffer) {
        grape_gpu_buffer_destroy(state->vertex_buffer);
    }
    if (state->depth) {
        grape_gpu_depth_buffer_destroy(state->depth);
    }
    if (state->gpu) {
        grape_gpu_context_destroy(state->gpu);
    }
    if (state->texture) {
        grape_texture_destroy(state->texture);
    }
    if (state->color_target) {
        grape_texture_destroy(state->color_target);
    }
    free(state);
}

static esp_err_t gpu_setup(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void **out_state)
{
    if (!runtime || !bench_case || !bench_case->user_data || !out_state) {
        return ESP_ERR_INVALID_ARG;
    }

    const gpu_case_config_t *config = bench_case->user_data;
    gpu_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }
    state->config = config;

    const grape_texture_desc_t target_desc = {
        .width = GPU_BENCH_WIDTH,
        .height = GPU_BENCH_HEIGHT,
        .format = GRAPE_PIXEL_FORMAT_RGBA8888,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    esp_err_t ret = grape_texture_create(runtime->grape, &target_desc, &state->color_target);
    if (ret != ESP_OK) goto fail;

    const grape_texture_desc_t texture_desc = {
        .width = GPU_BENCH_TEXTURE_SIZE,
        .height = GPU_BENCH_TEXTURE_SIZE,
        .format = GRAPE_PIXEL_FORMAT_RGB565,
        .memory = GRAPE_MEMORY_INTERNAL,
    };
    ret = grape_texture_create(runtime->grape, &texture_desc, &state->texture);
    if (ret != ESP_OK) goto fail;
    build_debug_texture(state->texture);
    ret = grape_texture_invalidate(state->texture);
    if (ret != ESP_OK) goto fail;

    ret = grape_gpu_context_create(runtime->grape, &state->gpu);
    if (ret != ESP_OK) goto fail;
    grape_gpu_set_stats_enabled(state->gpu, GRAPE_BENCHMARK_GPU_STATS != 0);

    const grape_gpu_depth_buffer_desc_t depth_desc = {
        .width = GPU_BENCH_WIDTH,
        .height = GPU_BENCH_HEIGHT,
        .format = GRAPE_GPU_DEPTH_D16,
        .memory = GRAPE_MEMORY_PSRAM,
        .sample_count = config->sample_count,
    };
    ret = grape_gpu_depth_buffer_create(state->gpu, &depth_desc, &state->depth);
    if (ret != ESP_OK) goto fail;

    const grape_gpu_buffer_desc_t vertex_desc = {
        .size = sizeof(s_vertices),
        .usage = GRAPE_GPU_BUFFER_VERTEX,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    ret = grape_gpu_buffer_create(state->gpu, &vertex_desc, &state->vertex_buffer);
    if (ret != ESP_OK) goto fail;
    ret = grape_gpu_buffer_write(state->vertex_buffer, 0U, s_vertices, sizeof(s_vertices));
    if (ret != ESP_OK) goto fail;

    const grape_gpu_buffer_desc_t index_desc = {
        .size = sizeof(s_indices),
        .usage = GRAPE_GPU_BUFFER_INDEX,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    ret = grape_gpu_buffer_create(state->gpu, &index_desc, &state->index_buffer);
    if (ret != ESP_OK) goto fail;
    ret = grape_gpu_buffer_write(state->index_buffer, 0U, s_indices, sizeof(s_indices));
    if (ret != ESP_OK) goto fail;

    const grape_gpu_pipeline_desc_t pipeline_desc = {
        .vertex_layout = {
            .stride = sizeof(gpu_bench_vertex_t),
            .attribute_count = 3U,
            .attributes = {
                { .location = 0U, .format = GRAPE_GPU_VERTEX_FORMAT_F32X3, .offset = offsetof(gpu_bench_vertex_t, x) },
                { .location = 1U, .format = GRAPE_GPU_VERTEX_FORMAT_F32X2, .offset = offsetof(gpu_bench_vertex_t, u) },
                { .location = 2U, .format = GRAPE_GPU_VERTEX_FORMAT_F32X4, .offset = offsetof(gpu_bench_vertex_t, r) },
            },
        },
        .topology = GRAPE_GPU_TOPOLOGY_TRIANGLE_LIST,
        .vertex_program = GRAPE_GPU_VERTEX_PROGRAM_MVP,
        .fragment_program = GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR,
        .solid_color = {255U,255U,255U,255U},
        .cull_mode = GRAPE_GPU_CULL_BACK,
        .front_face = GRAPE_GPU_FRONT_FACE_CW,
        .depth = {
            .test_enable = true,
            .write_enable = true,
            .compare_op = GRAPE_GPU_COMPARE_LESS,
        },
        .sampler = {
            .filter = GRAPE_GPU_FILTER_LINEAR,
            .address_u = GRAPE_GPU_ADDRESS_REPEAT,
            .address_v = GRAPE_GPU_ADDRESS_REPEAT,
        },
        .sample_count = config->sample_count,
    };
    ret = grape_gpu_pipeline_create(state->gpu, &pipeline_desc, &state->pipeline);
    if (ret != ESP_OK) goto fail;

    state->projection = mat4_perspective_lh(
        GPU_BENCH_FOV_Y,
        (float)GPU_BENCH_WIDTH / (float)GPU_BENCH_HEIGHT,
        GPU_BENCH_NEAR,
        GPU_BENCH_FAR
    );

    if (config->present) {
        grape_surface_desc_t desc = GRAPE_SURFACE_DESC_TEXTURE(state->color_target);
        desc.transform.scale_x = 2.0f;
        desc.transform.scale_y = 2.0f;
        ret = grape_surface_create(runtime->grape, &desc, &state->surface);
        if (ret != ESP_OK) goto fail;
        for (unsigned i = 0; i < 2; ++i) {
            ret = grape_invalidate_all(runtime->grape);
            if (ret == ESP_OK) ret = grape_present(runtime->grape);
            if (ret != ESP_OK) goto fail;
        }
    }
    *out_state = state;
    return ESP_OK;

fail:
    destroy_state(state);
    return ret;
}

static void stats_accumulate(grape_gpu_stats_t *sum, const grape_gpu_stats_t *value)
{
    sum->pass_us += value->pass_us;
    sum->vertex_transform_us += value->vertex_transform_us;
    sum->clip_us += value->clip_us;
    sum->triangle_setup_us += value->triangle_setup_us;
    sum->tile_bin_us += value->tile_bin_us;
    sum->tile_raster_us += value->tile_raster_us;
    sum->resolve_us += value->resolve_us;
    sum->draw_calls += value->draw_calls;
    sum->input_triangles += value->input_triangles;
    sum->clipped_away_triangles += value->clipped_away_triangles;
    sum->post_clip_triangles += value->post_clip_triangles;
    sum->culled_triangles += value->culled_triangles;
    sum->degenerate_triangles += value->degenerate_triangles;
    sum->rasterized_triangles += value->rasterized_triangles;
    sum->active_tiles += value->active_tiles;
    sum->tile_references += value->tile_references;
    sum->triangle_bbox_pixels += value->triangle_bbox_pixels;
}

static esp_err_t gpu_before_measurement(grape_benchmark_runtime_t *runtime,
                                        const grape_benchmark_case_t *bench_case,
                                        void *opaque)
{
    (void)runtime;
    (void)bench_case;
    gpu_case_state_t *state = opaque;
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(&state->stats_sum, 0, sizeof(state->stats_sum));
    state->measured_passes = 0U;
    state->collecting = true;
    return ESP_OK;
}

static esp_err_t gpu_iteration(grape_benchmark_runtime_t *runtime,
                               const grape_benchmark_case_t *bench_case,
                               void *opaque,
                               uint32_t sequence_iteration)
{
    (void)runtime;
    (void)bench_case;
    gpu_case_state_t *state = opaque;
    if (!state || !state->config) {
        return ESP_ERR_INVALID_ARG;
    }

    const grape_gpu_render_pass_desc_t pass = {
        .color_attachment = state->color_target,
        .color_load_op = state->config->alternate_load && (sequence_iteration & 1U)
            ? GRAPE_GPU_LOAD_OP_LOAD : GRAPE_GPU_LOAD_OP_CLEAR,
        .clear_color = {0U,0U,0U,0U},
        .depth_attachment = state->depth,
        .depth_load_op = state->config->alternate_load && (sequence_iteration & 1U)
            ? GRAPE_GPU_LOAD_OP_LOAD : GRAPE_GPU_LOAD_OP_CLEAR,
        .clear_depth = 1.0f,
        .sample_count = state->config->sample_count,
    };
    esp_err_t ret = grape_gpu_begin_render_pass(state->gpu, &pass);
    if (ret != ESP_OK) return ret;

    ret = grape_gpu_bind_pipeline(state->gpu, state->pipeline);
    if (ret == ESP_OK) ret = grape_gpu_bind_vertex_buffer(state->gpu, state->vertex_buffer);
    if (ret == ESP_OK) ret = grape_gpu_bind_index_buffer(
        state->gpu, state->index_buffer, GRAPE_GPU_INDEX_U16
    );
    if (ret == ESP_OK) ret = grape_gpu_bind_texture(state->gpu, 0U, state->texture);

    const float t = (float)sequence_iteration * 0.071f;
    for (uint32_t instance = 0U; ret == ESP_OK && instance < state->config->instances; ++instance) {
        float x = 0.0f;
        float y = 0.0f;
        float z = 3.40f;
        if (state->config->instances > 1U) {
            const uint32_t column = instance % 4U;
            const uint32_t row = instance / 4U;
            x = ((float)column - 1.5f) * 1.75f;
            y = ((float)row - 1.0f) * 1.75f;
            z = 5.35f + 0.12f * (float)(instance % 3U);
        }

        if (state->config->shape == 2U) {
            x = y = 0.0f; z = 3.4f + instance * 0.02f;
        } else if (state->config->shape == 3U) {
            x = ((float)(instance % 8U) - 3.5f) * 1.1f;
            y = ((float)(instance / 8U) - 2.5f) * 1.1f;
            z = 10.0f;
        } else if (state->config->shape == 4U) {
            z = 1.15f;
        }
        const float phase = t + (float)instance * 0.173f;
        const grape_gpu_mat4_t rotation = mat4_multiply(
            mat4_rotation_y(phase * 0.91f),
            mat4_rotation_x(phase * 1.17f)
        );
        grape_gpu_mat4_t scale = mat4_identity();
        if (state->config->shape == 1U) { scale.m[0] = 0.025f; scale.m[5] = 2.0f; }
        if (state->config->shape == 3U) { scale.m[0] = scale.m[5] = scale.m[10] = 0.25f; }
        const grape_gpu_mat4_t model = state->config->shape == 1U || state->config->shape == 3U
            ? mat4_multiply(mat4_translation(x, y, z), mat4_multiply(rotation, scale))
            : mat4_multiply(mat4_translation(x, y, z), rotation);
        const grape_gpu_mat4_t mvp = mat4_multiply(state->projection, model);

        ret = grape_gpu_set_push_constants(
            state->gpu,
            offsetof(grape_gpu_builtin_constants_t, mvp),
            &mvp,
            sizeof(mvp)
        );
        if (ret == ESP_OK) {
            ret = grape_gpu_draw_indexed(state->gpu, 0U, GPU_BENCH_INDEX_COUNT, 0);
        }
    }

    const esp_err_t end_ret = grape_gpu_end_render_pass(state->gpu);
    if (ret != ESP_OK) {
        return ret;
    }
    if (end_ret != ESP_OK) {
        return end_ret;
    }

    if (!GRAPE_BENCHMARK_GPU_STATS) return ESP_OK;
    grape_gpu_stats_t stats;
    ret = grape_gpu_get_stats(state->gpu, &stats);
    if (ret != ESP_OK) {
        return ret;
    }
    if (state->collecting) {
        stats_accumulate(&state->stats_sum, &stats);
        ++state->measured_passes;
    }
    return ESP_OK;
}

static size_t gpu_collect_metrics(grape_benchmark_runtime_t *runtime,
                                  const grape_benchmark_case_t *bench_case,
                                  void *opaque,
                                  grape_benchmark_metric_t *out_metrics,
                                  size_t capacity)
{
    (void)runtime;
    (void)bench_case;
    gpu_case_state_t *state = opaque;
    if (!state || !out_metrics || capacity == 0U || state->measured_passes == 0U) {
        return 0U;
    }

    const double n = (double)state->measured_passes;
#define METRIC(field, metric_name, metric_unit) \
    { (metric_name), (metric_unit), (double)state->stats_sum.field / n }
    const grape_benchmark_metric_t metrics[] = {
        METRIC(pass_us, "gpu_pass", "us"),
        METRIC(vertex_transform_us, "gpu_vertex", "us"),
        METRIC(clip_us, "gpu_clip", "us"),
        METRIC(triangle_setup_us, "gpu_setup", "us"),
        METRIC(tile_bin_us, "gpu_bin", "us"),
        METRIC(tile_raster_us, "gpu_raster", "us"),
        METRIC(resolve_us, "gpu_resolve", "us"),
        METRIC(input_triangles, "input_tri", ""),
        METRIC(post_clip_triangles, "postclip_tri", ""),
        METRIC(rasterized_triangles, "raster_tri", ""),
        METRIC(active_tiles, "active_tiles", ""),
        METRIC(tile_references, "tile_refs", ""),
    };
#undef METRIC

    size_t count = sizeof(metrics) / sizeof(metrics[0]);
    if (count > capacity) {
        count = capacity;
    }
    memcpy(out_metrics, metrics, count * sizeof(metrics[0]));
    return count;
}

static void gpu_teardown(grape_benchmark_runtime_t *runtime,
                         const grape_benchmark_case_t *bench_case,
                         void *opaque)
{
    (void)runtime;
    (void)bench_case;
    destroy_state(opaque);
}

static esp_err_t gpu_capture(grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bc, void *opaque, uint32_t frame)
{
    gpu_case_state_t *state = opaque;
    esp_err_t ret = grape_benchmark_report_reference(runtime, bc, frame, "color", "rgba8888",
        GPU_BENCH_WIDTH, GPU_BENCH_HEIGHT, 1, grape_texture_pixels(state->color_target),
        grape_texture_stride(state->color_target), GPU_BENCH_WIDTH * 4U);
    if (ret != ESP_OK) return ret;
    size_t stride = GPU_BENCH_WIDTH * (size_t)state->config->sample_count * 2U;
    size_t size = stride * GPU_BENCH_HEIGHT;
    void *depth = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!depth) return ESP_ERR_NO_MEM;
    ret = grape_benchmark_copy_depth(state->depth, depth, size);
    if (ret == ESP_OK) ret = grape_benchmark_report_reference(runtime, bc, frame, "depth", "d16le",
        GPU_BENCH_WIDTH, GPU_BENCH_HEIGHT, (uint32_t)state->config->sample_count, depth, stride, stride);
    heap_caps_free(depth);
    if (ret == ESP_OK && state->config->present)
        ret = grape_benchmark_capture_display(runtime, bc, opaque, frame);
    return ret;
}

#define GPU_CASE(n, samples_, instances_, shape_, present_, load_) \
    { .group="gpu3d", .name=n, .kind=GRAPE_BENCHMARK_KIND_PIPELINE, \
      .flags=(present_ ? GRAPE_BENCHMARK_CASE_PRESENT : 0), \
      .user_data=&(const gpu_case_config_t){samples_, instances_, shape_, present_, load_}, \
      .setup=gpu_setup, .iteration=gpu_iteration, .before_measurement=gpu_before_measurement, \
      .collect_metrics=gpu_collect_metrics, .teardown=gpu_teardown, .capture_reference=gpu_capture, \
      .params={{"width",GPU_BENCH_WIDTH},{"height",GPU_BENCH_HEIGHT},{"samples",samples_}, \
               {"instances",instances_},{"shape",shape_},{"alternate_load",load_}} }
static const grape_benchmark_case_t s_cases[] = {
    GPU_CASE("textured_cube_1x", GRAPE_GPU_SAMPLE_COUNT_1,1,0,false,false),
    GPU_CASE("textured_cube_2x", GRAPE_GPU_SAMPLE_COUNT_2,1,0,false,false),
    GPU_CASE("textured_cube_4x_msaa", GRAPE_GPU_SAMPLE_COUNT_4,1,0,false,false),
    GPU_CASE("textured_grid12_4x_msaa", GRAPE_GPU_SAMPLE_COUNT_4,12,0,false,false),
    GPU_CASE("thin_grid12_4x", GRAPE_GPU_SAMPLE_COUNT_4,12,1,false,false),
    GPU_CASE("overdraw12_4x", GRAPE_GPU_SAMPLE_COUNT_4,12,2,false,false),
    GPU_CASE("small_grid48_4x", GRAPE_GPU_SAMPLE_COUNT_4,48,3,false,false),
    GPU_CASE("near_clip_4x", GRAPE_GPU_SAMPLE_COUNT_4,1,4,false,false),
    GPU_CASE("alternating_load_4x", GRAPE_GPU_SAMPLE_COUNT_4,1,0,false,true),
    GPU_CASE("cube_4x_present_2x", GRAPE_GPU_SAMPLE_COUNT_4,1,0,true,false),
};
#undef GPU_CASE

const grape_benchmark_case_t *grape_benchmark_gpu3d_cases(size_t *out_count)
{
    if (out_count) *out_count = sizeof(s_cases) / sizeof(s_cases[0]);
    return s_cases;
}
