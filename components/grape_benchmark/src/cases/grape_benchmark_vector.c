#include "grape_benchmark_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define VECTOR_DOMAIN 100.0f
#define VECTOR_PI 3.14159265358979323846f

typedef enum {
    VECTOR_OP_BUILD = 0,
    VECTOR_OP_RASTER,
} vector_operation_t;

typedef enum {
    VECTOR_SHAPE_LINE = 0,
    VECTOR_SHAPE_QUAD,
    VECTOR_SHAPE_CUBIC,
    VECTOR_SHAPE_ARC,
    VECTOR_SHAPE_MIXED,
} vector_shape_t;

typedef struct {
    vector_operation_t operation;
    vector_shape_t shape;
    uint32_t command_count;
    uint32_t raster_size;
    uint8_t samples_per_axis;
} vector_case_config_t;

typedef struct {
    grape_path_t *path;
    grape_path_raster_t raster;
    uint32_t raster_width;
    uint32_t raster_height;
} vector_case_state_t;

static esp_err_t build_path(grape_path_t *path,
                            vector_shape_t shape,
                            uint32_t command_count)
{
    if (!path || command_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = grape_path_move_to(path, 0.0f, 50.0f);
    if (ret != ESP_OK) {
        return ret;
    }

    for (uint32_t i = 0; i < command_count; ++i) {
        float t0 = (float)i / (float)command_count;
        float t1 = (float)(i + 1U) / (float)command_count;
        float x0 = t0 * VECTOR_DOMAIN;
        float x1 = t1 * VECTOR_DOMAIN;
        float y0 = 50.0f + sinf(t0 * VECTOR_PI * 6.0f) * 36.0f;
        float y1 = 50.0f + sinf(t1 * VECTOR_PI * 6.0f) * 36.0f;
        float mid_x = (x0 + x1) * 0.5f;
        float direction = (i & 1U) ? -1.0f : 1.0f;
        vector_shape_t command_shape = shape == VECTOR_SHAPE_MIXED
            ? (vector_shape_t)(i % 4U)
            : shape;

        switch (command_shape) {
            case VECTOR_SHAPE_LINE:
                ret = grape_path_line_to(path, x1, y1);
                break;

            case VECTOR_SHAPE_QUAD:
                ret = grape_path_quad_to(
                    path,
                    mid_x,
                    (y0 + y1) * 0.5f + direction * 24.0f,
                    x1,
                    y1
                );
                break;

            case VECTOR_SHAPE_CUBIC: {
                float dx = (x1 - x0) / 3.0f;
                ret = grape_path_cubic_to(
                    path,
                    x0 + dx,
                    y0 + direction * 28.0f,
                    x1 - dx,
                    y1 - direction * 28.0f,
                    x1,
                    y1
                );
                break;
            }

            case VECTOR_SHAPE_ARC: {
                float radius = fmaxf((x1 - x0) * 1.2f, 2.0f);
                ret = grape_path_arc_to(
                    path,
                    radius,
                    18.0f + (float)(i % 5U) * 2.0f,
                    (float)((i * 13U) % 45U),
                    false,
                    (i & 1U) == 0U,
                    x1,
                    y1
                );
                break;
            }

            case VECTOR_SHAPE_MIXED:
            default:
                ret = ESP_ERR_INVALID_ARG;
                break;
        }

        if (ret != ESP_OK) {
            return ret;
        }
    }

    ret = grape_path_line_to(path, VECTOR_DOMAIN, 96.0f);
    if (ret == ESP_OK) {
        ret = grape_path_line_to(path, 0.0f, 96.0f);
    }
    if (ret == ESP_OK) {
        ret = grape_path_close(path);
    }
    return ret;
}

static void destroy_raster(vector_case_state_t *state)
{
    if (!state || !state->raster.texture) {
        return;
    }
    grape_texture_destroy(state->raster.texture);
    state->raster = (grape_path_raster_t){0};
}

static void destroy_state(vector_case_state_t *state)
{
    if (!state) {
        return;
    }
    destroy_raster(state);
    if (state->path) {
        grape_path_destroy(state->path);
    }
    free(state);
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    (void)runtime;
    const vector_case_config_t *config = bench_case->user_data;
    if (!config || !out_state) {
        return ESP_ERR_INVALID_ARG;
    }

    vector_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = grape_path_create(&state->path);
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    if (config->operation == VECTOR_OP_RASTER) {
        ret = build_path(state->path, config->shape, config->command_count);
        if (ret != ESP_OK) {
            destroy_state(state);
            return ret;
        }
    }

    *out_state = state;
    return ESP_OK;
}

static esp_err_t iteration(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void *opaque_state,
                           uint32_t sequence_iteration)
{
    (void)sequence_iteration;
    const vector_case_config_t *config = bench_case->user_data;
    vector_case_state_t *state = opaque_state;

    switch (config->operation) {
        case VECTOR_OP_BUILD: {
            esp_err_t ret = grape_path_clear(state->path);
            return ret == ESP_OK
                ? build_path(state->path, config->shape, config->command_count)
                : ret;
        }

        case VECTOR_OP_RASTER: {
            grape_path_rasterize_config_t raster_config =
                GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT();
            raster_config.pixels_per_unit =
                (float)config->raster_size / VECTOR_DOMAIN;
            raster_config.samples_per_axis = config->samples_per_axis;
            raster_config.padding_pixels = 2U;
            raster_config.memory = GRAPE_MEMORY_PSRAM;

            esp_err_t ret = grape_path_rasterize_a8(
                runtime->grape,
                state->path,
                &raster_config,
                &state->raster
            );
            if (ret == ESP_OK) {
                state->raster_width = grape_texture_width(state->raster.texture);
                state->raster_height = grape_texture_height(state->raster.texture);
            }
            return ret;
        }

        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static void after_iteration(grape_benchmark_runtime_t *runtime,
                            const grape_benchmark_case_t *bench_case,
                            void *opaque_state,
                            uint32_t sequence_iteration)
{
    (void)runtime;
    (void)sequence_iteration;
    const vector_case_config_t *config = bench_case->user_data;
    if (config->operation == VECTOR_OP_RASTER) {
        destroy_raster(opaque_state);
    }
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    const vector_case_config_t *config = bench_case->user_data;
    vector_case_state_t *state = opaque_state;
    if (capacity < 4U) {
        return 0U;
    }

    out[0] = (grape_benchmark_metric_t){
        "commands", "", config->command_count,
    };
    out[1] = (grape_benchmark_metric_t){
        "raster_width", "px", state->raster_width,
    };
    out[2] = (grape_benchmark_metric_t){
        "raster_height", "px", state->raster_height,
    };
    out[3] = (grape_benchmark_metric_t){
        "raster_pixels", "px",
        (double)state->raster_width * state->raster_height,
    };
    return 4U;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)runtime;
    (void)bench_case;
    destroy_state(opaque_state);
}

#define VECTOR_CASE_CAPACITY 56

static grape_benchmark_case_t s_cases[VECTOR_CASE_CAPACITY];
static vector_case_config_t s_configs[VECTOR_CASE_CAPACITY];
static char s_names[VECTOR_CASE_CAPACITY][64];
static size_t s_case_count;
static bool s_initialized;

static const char *shape_name(vector_shape_t shape)
{
    switch (shape) {
        case VECTOR_SHAPE_LINE: return "line";
        case VECTOR_SHAPE_QUAD: return "quad";
        case VECTOR_SHAPE_CUBIC: return "cubic";
        case VECTOR_SHAPE_ARC: return "arc";
        case VECTOR_SHAPE_MIXED: return "mixed";
        default: return "unknown";
    }
}

static const char *operation_name(vector_operation_t operation)
{
    switch (operation) {
        case VECTOR_OP_BUILD: return "build";
        case VECTOR_OP_RASTER: return "raster";
        default: return "unknown";
    }
}

static void add_case(vector_operation_t operation,
                     vector_shape_t shape,
                     uint32_t command_count,
                     uint32_t raster_size,
                     uint8_t samples_per_axis)
{
    if (s_case_count >= VECTOR_CASE_CAPACITY) {
        return;
    }

    size_t index = s_case_count++;
    s_configs[index] = (vector_case_config_t){
        .operation = operation,
        .shape = shape,
        .command_count = command_count,
        .raster_size = raster_size,
        .samples_per_axis = samples_per_axis,
    };

    if (operation == VECTOR_OP_RASTER) {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s_%s_n%u_s%u_aa%u",
            operation_name(operation), shape_name(shape),
            (unsigned)command_count, (unsigned)raster_size,
            (unsigned)samples_per_axis
        );
    } else {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s_%s_n%u",
            operation_name(operation), shape_name(shape),
            (unsigned)command_count
        );
    }

    uint32_t measured = 32U;
    uint32_t warmup = 6U;
    if (operation == VECTOR_OP_RASTER &&
        (raster_size >= 256U || command_count >= 128U)) {
        measured = 12U;
        warmup = 3U;
    }

    s_cases[index] = (grape_benchmark_case_t){
        .group = operation == VECTOR_OP_BUILD
            ? "vector.build"
            : "vector.raster",
        .name = s_names[index],
        .kind = GRAPE_BENCHMARK_KIND_MICRO,
        .warmup_iterations = warmup,
        .measured_iterations = measured,
        .user_data = &s_configs[index],
        .setup = setup,
        .iteration = iteration,
        .after_iteration = after_iteration,
        .collect_metrics = metrics,
        .teardown = teardown,
        .params = {
            { "operation", operation },
            { "shape", shape },
            { "commands", command_count },
            { "size", raster_size },
            { "aa", samples_per_axis },
        },
    };
}

const grape_benchmark_case_t *grape_benchmark_vector_cases(size_t *out_count)
{
    if (!s_initialized) {
        static const uint32_t counts[] = { 8U, 32U, 128U };
        for (vector_shape_t shape = VECTOR_SHAPE_LINE;
             shape <= VECTOR_SHAPE_MIXED;
             shape = (vector_shape_t)(shape + 1)) {
            for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
                add_case(VECTOR_OP_BUILD, shape, counts[i], 0U, 0U);
                add_case(VECTOR_OP_RASTER, shape, counts[i], 128U, 4U);
            }
        }

        static const uint32_t sizes[] = { 32U, 64U, 128U, 256U };
        static const uint8_t aa[] = { 1U, 2U, 4U };
        for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
            for (size_t ai = 0; ai < sizeof(aa) / sizeof(aa[0]); ++ai) {
                if (sizes[si] == 128U && aa[ai] == 4U) {
                    continue;
                }
                add_case(
                    VECTOR_OP_RASTER,
                    VECTOR_SHAPE_MIXED,
                    32U,
                    sizes[si],
                    aa[ai]
                );
            }
        }

        s_initialized = true;
    }

    if (out_count) {
        *out_count = s_case_count;
    }
    return s_cases;
}
