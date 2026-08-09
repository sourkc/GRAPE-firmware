#include "grape_benchmark_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

typedef enum {
    MARK_PATTERN_EMPTY = 0,
    MARK_PATTERN_OPAQUE,
    MARK_PATTERN_SPARSE25,
    MARK_PATTERN_HALF,
    MARK_PATTERN_DENSE_MINUS_ONE,
} mark_pattern_t;

typedef struct {
    uint32_t size;
    uint32_t surfaces;
    mark_pattern_t pattern;
    bool invalidate_texture;
} mark_case_config_t;

typedef struct {
    grape_texture_t *texture;
    grape_surface_t **surfaces;
    uint32_t surface_count;
    grape_benchmark_occupancy_info_t occupancy;
} mark_case_state_t;

static void fill_pattern(grape_texture_t *texture, mark_pattern_t pattern)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);
    memset(pixels, 0, stride * height);

    if (pattern == MARK_PATTERN_OPAQUE) {
        for (uint32_t y = 0; y < height; ++y) {
            memset(pixels + (size_t)y * stride, 255, width);
        }
        return;
    }
    if (pattern == MARK_PATTERN_EMPTY) {
        return;
    }

    const uint32_t cell = CONFIG_GRAPE_TEXTURE_OCCUPANCY_CELL_SIZE;
    uint32_t columns = (width + cell - 1U) / cell;
    uint32_t rows = (height + cell - 1U) / cell;
    uint32_t total = columns * rows;

    for (uint32_t cy = 0; cy < rows; ++cy) {
        for (uint32_t cx = 0; cx < columns; ++cx) {
            uint32_t index = cy * columns + cx;
            bool occupied = false;
            switch (pattern) {
                case MARK_PATTERN_SPARSE25:
                    occupied = (grape_benchmark_hash_u32(index) & 3U) == 0U;
                    break;
                case MARK_PATTERN_HALF:
                    occupied = (grape_benchmark_hash_u32(index) & 1U) == 0U;
                    break;
                case MARK_PATTERN_DENSE_MINUS_ONE:
                    occupied = index + 1U < total;
                    break;
                default:
                    break;
            }
            if (occupied) {
                uint32_t x = cx * cell;
                uint32_t y = cy * cell;
                if (x < width && y < height) {
                    pixels[(size_t)y * stride + x] = 255;
                }
            }
        }
    }
}

static void mark_state_destroy(mark_case_state_t *state)
{
    if (!state) return;
    if (state->surfaces) {
        for (uint32_t i = 0; i < state->surface_count; ++i) {
            if (state->surfaces[i]) grape_surface_destroy(state->surfaces[i]);
        }
    }
    free(state->surfaces);
    if (state->texture) grape_texture_destroy(state->texture);
    free(state);
}

static esp_err_t mark_setup(grape_benchmark_runtime_t *runtime,
                            const grape_benchmark_case_t *bench_case,
                            void **out_state)
{
    const mark_case_config_t *config = bench_case->user_data;
    if (!runtime || !config || !out_state) return ESP_ERR_INVALID_ARG;

    mark_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) return ESP_ERR_NO_MEM;

    state->surface_count = config->surfaces;
    state->surfaces = calloc(config->surfaces, sizeof(*state->surfaces));
    if (!state->surfaces) {
        mark_state_destroy(state);
        return ESP_ERR_NO_MEM;
    }

    grape_texture_desc_t desc = {
        .width = config->size,
        .height = config->size,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    esp_err_t ret = grape_texture_create(runtime->grape, &desc, &state->texture);
    if (ret != ESP_OK) {
        mark_state_destroy(state);
        return ret;
    }

    fill_pattern(state->texture, config->pattern);
    ret = grape_texture_invalidate(state->texture);
    if (ret != ESP_OK) {
        mark_state_destroy(state);
        return ret;
    }

    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    if (!display) {
        mark_state_destroy(state);
        return ESP_ERR_INVALID_STATE;
    }

    for (uint32_t i = 0; i < config->surfaces; ++i) {
        ret = grape_surface_create(runtime->grape, state->texture, &state->surfaces[i]);
        if (ret != ESP_OK) {
            mark_state_destroy(state);
            return ret;
        }
        float x = 32.0f + fmodf((float)(i * 79U), (float)(display->width - 64U));
        float y = 32.0f + fmodf((float)(i * 131U), (float)(display->height - 64U));
        ret = grape_surface_set_origin(state->surfaces[i], config->size * 0.5f, config->size * 0.5f);
        if (ret == ESP_OK) ret = grape_surface_set_position(state->surfaces[i], x, y);
        if (ret == ESP_OK) ret = grape_surface_set_rotation(state->surfaces[i], 0.31f + 0.003f * i);
        if (ret != ESP_OK) {
            mark_state_destroy(state);
            return ret;
        }
    }

    grape_benchmark_damage_clear(runtime->grape);
    grape_benchmark_texture_occupancy_info(state->texture, &state->occupancy);
    *out_state = state;
    return ESP_OK;
}

static esp_err_t mark_iteration(grape_benchmark_runtime_t *runtime,
                                const grape_benchmark_case_t *bench_case,
                                void *opaque_state,
                                uint32_t sequence_iteration)
{
    const mark_case_config_t *config = bench_case->user_data;
    mark_case_state_t *state = opaque_state;

    if (config->invalidate_texture) {
        return grape_texture_invalidate(state->texture);
    }

    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    float t = grape_benchmark_fixed_time_s(runtime, sequence_iteration);
    for (uint32_t i = 0; i < state->surface_count; ++i) {
        float phase = (float)i * 0.37f;
        float x = (float)display->width * 0.5f + sinf(t * 1.7f + phase) * (float)display->width * 0.35f;
        float y = (float)display->height * 0.5f + cosf(t * 1.31f + phase) * (float)display->height * 0.35f;
        grape_transform_t transform = *grape_surface_transform(state->surfaces[i]);
        transform.x = x;
        transform.y = y;
        transform.rotation = 0.31f + t * 0.9f + 0.007f * (float)i;
        esp_err_t ret = grape_surface_set_transform(state->surfaces[i], &transform);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

static void mark_after(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void *state,
                       uint32_t sequence_iteration)
{
    (void)bench_case;
    (void)state;
    (void)sequence_iteration;
    grape_benchmark_damage_clear(runtime->grape);
}

static size_t mark_metrics(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void *opaque_state,
                           grape_benchmark_metric_t *out,
                           size_t capacity)
{
    (void)runtime;
    (void)bench_case;
    mark_case_state_t *state = opaque_state;
    if (capacity < 5) return 0;
    out[0] = (grape_benchmark_metric_t){ "occupancy_cells", "", (double)state->occupancy.columns * state->occupancy.rows };
    out[1] = (grape_benchmark_metric_t){ "occupied_cells", "", (double)state->occupancy.occupied_cells };
    out[2] = (grape_benchmark_metric_t){ "occupancy_bytes", "B", (double)state->occupancy.bitmap_size };
    out[3] = (grape_benchmark_metric_t){ "occupancy_all_full", "", state->occupancy.all_full ? 1.0 : 0.0 };
    out[4] = (grape_benchmark_metric_t){ "occupancy_all_empty", "", state->occupancy.all_empty ? 1.0 : 0.0 };
    return 5;
}

static void mark_teardown(grape_benchmark_runtime_t *runtime,
                          const grape_benchmark_case_t *bench_case,
                          void *state)
{
    (void)runtime;
    (void)bench_case;
    mark_state_destroy(state);
}

#define MARK_CASE_CAPACITY 64
static grape_benchmark_case_t s_cases[MARK_CASE_CAPACITY];
static mark_case_config_t s_configs[MARK_CASE_CAPACITY];
static char s_names[MARK_CASE_CAPACITY][48];
static size_t s_case_count;
static bool s_initialized;

static const char *pattern_name(mark_pattern_t pattern)
{
    switch (pattern) {
        case MARK_PATTERN_EMPTY: return "empty";
        case MARK_PATTERN_OPAQUE: return "opaque";
        case MARK_PATTERN_SPARSE25: return "cells25";
        case MARK_PATTERN_HALF: return "cells50";
        case MARK_PATTERN_DENSE_MINUS_ONE: return "dense_minus_one";
        default: return "unknown";
    }
}

static void add_case(mark_pattern_t pattern, uint32_t size, uint32_t surfaces, bool invalidation)
{
    if (s_case_count >= MARK_CASE_CAPACITY) return;
    size_t n = s_case_count++;
    s_configs[n] = (mark_case_config_t){ size, surfaces, pattern, invalidation };
    snprintf(s_names[n], sizeof(s_names[n]), "%s_%u_s%u%s",
             pattern_name(pattern), (unsigned)size, (unsigned)surfaces,
             invalidation ? "_invalidate" : "");
    s_cases[n] = (grape_benchmark_case_t) {
        .group = "damage_mark",
        .name = s_names[n],
        .kind = GRAPE_BENCHMARK_KIND_MICRO,
        .user_data = &s_configs[n],
        .setup = mark_setup,
        .iteration = mark_iteration,
        .after_iteration = mark_after,
        .collect_metrics = mark_metrics,
        .teardown = mark_teardown,
        .params = {
            { "pattern", (double)pattern },
            { "size", (double)size },
            { "surfaces", (double)surfaces },
            { "invalidate", invalidation ? 1.0 : 0.0 },
        },
    };
}

const grape_benchmark_case_t *grape_benchmark_damage_mark_cases(size_t *out_count)
{
    if (!s_initialized) {
        static const uint32_t sizes[] = {64, 128, 256, 512};
        static const uint32_t counts[] = {1, 10, 50};
        for (int pattern = MARK_PATTERN_EMPTY; pattern <= MARK_PATTERN_DENSE_MINUS_ONE; ++pattern) {
            for (size_t si = 0; si < sizeof(sizes)/sizeof(sizes[0]); ++si) {
                for (size_t ci = 0; ci < sizeof(counts)/sizeof(counts[0]); ++ci) {
                    add_case((mark_pattern_t)pattern, sizes[si], counts[ci], false);
                }
            }
        }
        static const uint32_t fanout[] = {1, 10, 50, 100};
        for (size_t i = 0; i < sizeof(fanout)/sizeof(fanout[0]); ++i) {
            add_case(MARK_PATTERN_HALF, 128, fanout[i], true);
        }
        s_initialized = true;
    }
    if (out_count) *out_count = s_case_count;
    return s_cases;
}
