#include "grape_benchmark_internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    LIFECYCLE_SURFACES = 0,
    LIFECYCLE_TEXTURES,
    LIFECYCLE_OCCUPANCY,
    LIFECYCLE_Z_REORDER,
} lifecycle_operation_t;

typedef struct {
    lifecycle_operation_t operation;
    uint32_t count;
    uint32_t size;
} lifecycle_case_config_t;

typedef struct {
    grape_texture_t *texture;
    grape_surface_t **surfaces;
    grape_texture_t **textures;
    uint32_t object_count;
} lifecycle_case_state_t;

static void destroy_state(lifecycle_case_state_t *state)
{
    if (!state) {
        return;
    }

    if (state->surfaces) {
        for (uint32_t i = 0; i < state->object_count; ++i) {
            if (state->surfaces[i]) {
                grape_surface_destroy(state->surfaces[i]);
            }
        }
    }
    free(state->surfaces);

    if (state->textures) {
        for (uint32_t i = 0; i < state->object_count; ++i) {
            if (state->textures[i]) {
                grape_texture_destroy(state->textures[i]);
            }
        }
    }
    free(state->textures);

    if (state->texture) {
        grape_texture_destroy(state->texture);
    }
    free(state);
}

static esp_err_t make_texture(grape_benchmark_runtime_t *runtime,
                              uint32_t size,
                              grape_texture_t **out_texture)
{
    grape_texture_desc_t desc = {
        .width = size,
        .height = size,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_PSRAM,
    };
    esp_err_t ret = grape_texture_create(runtime->grape, &desc, out_texture);
    if (ret != ESP_OK) {
        return ret;
    }

    memset(
        grape_texture_pixels(*out_texture),
        255,
        grape_texture_stride(*out_texture) * size
    );
    return grape_texture_invalidate(*out_texture);
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    const lifecycle_case_config_t *config = bench_case->user_data;
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    lifecycle_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }
    state->object_count = config->count;

    if (config->operation == LIFECYCLE_SURFACES ||
        config->operation == LIFECYCLE_OCCUPANCY ||
        config->operation == LIFECYCLE_Z_REORDER) {
        esp_err_t ret = make_texture(runtime, config->size, &state->texture);
        if (ret != ESP_OK) {
            destroy_state(state);
            return ret;
        }
    }

    if (config->operation == LIFECYCLE_SURFACES ||
        config->operation == LIFECYCLE_Z_REORDER) {
        state->surfaces = calloc(config->count, sizeof(*state->surfaces));
        if (!state->surfaces) {
            destroy_state(state);
            return ESP_ERR_NO_MEM;
        }
    }

    if (config->operation == LIFECYCLE_TEXTURES) {
        state->textures = calloc(config->count, sizeof(*state->textures));
        if (!state->textures) {
            destroy_state(state);
            return ESP_ERR_NO_MEM;
        }
    }

    if (config->operation == LIFECYCLE_Z_REORDER) {
        for (uint32_t i = 0; i < config->count; ++i) {
            esp_err_t ret = grape_surface_create(
                runtime->grape,
                state->texture,
                &state->surfaces[i]
            );
            if (ret == ESP_OK) {
                ret = grape_surface_set_position(
                    state->surfaces[i],
                    (float)(i % 64U),
                    (float)((i * 7U) % 64U)
                );
            }
            if (ret != ESP_OK) {
                destroy_state(state);
                return ret;
            }
        }
    }

    grape_benchmark_damage_clear(runtime->grape);
    *out_state = state;
    return ESP_OK;
}

static esp_err_t run_surface_create_destroy(
    grape_benchmark_runtime_t *runtime,
    const lifecycle_case_config_t *config,
    lifecycle_case_state_t *state
)
{
    esp_err_t ret = ESP_OK;
    for (uint32_t i = 0; i < config->count && ret == ESP_OK; ++i) {
        ret = grape_surface_create(
            runtime->grape,
            state->texture,
            &state->surfaces[i]
        );
    }

    for (uint32_t i = 0; i < config->count; ++i) {
        if (!state->surfaces[i]) {
            continue;
        }
        esp_err_t destroy_ret = grape_surface_destroy(state->surfaces[i]);
        state->surfaces[i] = NULL;
        if (ret == ESP_OK && destroy_ret != ESP_OK) {
            ret = destroy_ret;
        }
    }
    return ret;
}

static esp_err_t run_texture_create_destroy(
    grape_benchmark_runtime_t *runtime,
    const lifecycle_case_config_t *config,
    lifecycle_case_state_t *state
)
{
    esp_err_t ret = ESP_OK;
    for (uint32_t i = 0; i < config->count && ret == ESP_OK; ++i) {
        grape_texture_desc_t desc = {
            .width = config->size,
            .height = config->size,
            .format = GRAPE_PIXEL_FORMAT_A8,
            .memory = GRAPE_MEMORY_PSRAM,
        };
        ret = grape_texture_create(runtime->grape, &desc, &state->textures[i]);
    }

    for (uint32_t i = 0; i < config->count; ++i) {
        if (!state->textures[i]) {
            continue;
        }
        esp_err_t destroy_ret = grape_texture_destroy(state->textures[i]);
        state->textures[i] = NULL;
        if (ret == ESP_OK && destroy_ret != ESP_OK) {
            ret = destroy_ret;
        }
    }
    return ret;
}

static esp_err_t run_occupancy_rebuild(
    const lifecycle_case_config_t *config,
    lifecycle_case_state_t *state,
    uint32_t sequence_iteration
)
{
    uint8_t *pixels = grape_texture_pixels(state->texture);
    size_t stride = grape_texture_stride(state->texture);

    for (uint32_t y = 0; y < config->size; ++y) {
        for (uint32_t x = 0; x < config->size; ++x) {
            pixels[(size_t)y * stride + x] =
                ((x + y + sequence_iteration) & 3U) ? 255 : 0;
        }
    }
    return grape_texture_invalidate(state->texture);
}

static esp_err_t run_z_reorder(
    const lifecycle_case_config_t *config,
    lifecycle_case_state_t *state,
    uint32_t sequence_iteration
)
{
    for (uint32_t i = 0; i < state->object_count; ++i) {
        int32_t z = (sequence_iteration & 1U)
            ? (int32_t)(state->object_count - i)
            : (int32_t)i;
        esp_err_t ret = grape_surface_set_z(state->surfaces[i], z);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    (void)config;
    return ESP_OK;
}

static esp_err_t iteration(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void *opaque_state,
                           uint32_t sequence_iteration)
{
    const lifecycle_case_config_t *config = bench_case->user_data;
    lifecycle_case_state_t *state = opaque_state;

    switch (config->operation) {
        case LIFECYCLE_SURFACES:
            return run_surface_create_destroy(runtime, config, state);
        case LIFECYCLE_TEXTURES:
            return run_texture_create_destroy(runtime, config, state);
        case LIFECYCLE_OCCUPANCY:
            return run_occupancy_rebuild(config, state, sequence_iteration);
        case LIFECYCLE_Z_REORDER:
            return run_z_reorder(config, state, sequence_iteration);
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static void after_iteration(grape_benchmark_runtime_t *runtime,
                            const grape_benchmark_case_t *bench_case,
                            void *opaque_state,
                            uint32_t sequence_iteration)
{
    (void)bench_case;
    (void)opaque_state;
    (void)sequence_iteration;
    grape_benchmark_damage_clear(runtime->grape);
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    (void)opaque_state;
    const lifecycle_case_config_t *config = bench_case->user_data;
    if (capacity < 2) {
        return 0;
    }

    out[0] = (grape_benchmark_metric_t) {
        "operations",
        "",
        config->count,
    };
    out[1] = (grape_benchmark_metric_t) {
        "texture_size",
        "px",
        config->size,
    };
    return 2;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)runtime;
    (void)bench_case;
    destroy_state(opaque_state);
}

#define LIFECYCLE_CASE_CAPACITY 24

static grape_benchmark_case_t s_cases[LIFECYCLE_CASE_CAPACITY];
static lifecycle_case_config_t s_configs[LIFECYCLE_CASE_CAPACITY];
static char s_names[LIFECYCLE_CASE_CAPACITY][48];
static size_t s_case_count;
static bool s_initialized;

static const char *operation_name(lifecycle_operation_t operation)
{
    switch (operation) {
        case LIFECYCLE_SURFACES: return "surface_create_destroy";
        case LIFECYCLE_TEXTURES: return "texture_create_destroy";
        case LIFECYCLE_OCCUPANCY: return "occupancy_rebuild";
        case LIFECYCLE_Z_REORDER: return "z_reorder";
        default: return "unknown";
    }
}

static void add_case(lifecycle_operation_t operation,
                     uint32_t count,
                     uint32_t size)
{
    if (s_case_count >= LIFECYCLE_CASE_CAPACITY) {
        return;
    }

    size_t index = s_case_count++;
    s_configs[index] = (lifecycle_case_config_t) {
        .operation = operation,
        .count = count,
        .size = size,
    };
    snprintf(
        s_names[index],
        sizeof(s_names[index]),
        "%s_n%u_s%u",
        operation_name(operation),
        (unsigned)count,
        (unsigned)size
    );
    s_cases[index] = (grape_benchmark_case_t) {
        .group = "lifecycle",
        .name = s_names[index],
        .kind = GRAPE_BENCHMARK_KIND_LIFECYCLE,
        .warmup_iterations = 2,
        .measured_iterations = 16,
        .user_data = &s_configs[index],
        .setup = setup,
        .iteration = iteration,
        .after_iteration = after_iteration,
        .collect_metrics = metrics,
        .teardown = teardown,
        .params = {
            { "operation", operation },
            { "count", count },
            { "size", size },
        },
    };
}

const grape_benchmark_case_t *grape_benchmark_lifecycle_cases(size_t *out_count)
{
    if (!s_initialized) {
        add_case(LIFECYCLE_SURFACES, 1, 32);
        add_case(LIFECYCLE_SURFACES, 10, 32);
        add_case(LIFECYCLE_SURFACES, 100, 32);

        add_case(LIFECYCLE_TEXTURES, 1, 64);
        add_case(LIFECYCLE_TEXTURES, 4, 128);
        add_case(LIFECYCLE_TEXTURES, 8, 256);

        add_case(LIFECYCLE_OCCUPANCY, 1, 64);
        add_case(LIFECYCLE_OCCUPANCY, 1, 128);
        add_case(LIFECYCLE_OCCUPANCY, 1, 256);
        add_case(LIFECYCLE_OCCUPANCY, 1, 512);

        add_case(LIFECYCLE_Z_REORDER, 10, 8);
        add_case(LIFECYCLE_Z_REORDER, 100, 8);
        add_case(LIFECYCLE_Z_REORDER, 500, 8);
        s_initialized = true;
    }

    if (out_count) {
        *out_count = s_case_count;
    }
    return s_cases;
}
