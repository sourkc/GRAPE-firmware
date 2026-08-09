#include "grape_benchmark_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t surfaces;
    uint32_t rects;
    uint32_t intersect_percent;
} compositor_case_config_t;

typedef struct {
    grape_texture_t *texture;
    grape_surface_t **surfaces;
    uint32_t surface_count;
    grape_rect_t *rects;
    uint32_t rect_count;
    grape_feature_mode_t old_blend;
    grape_rotation_backend_t old_rotation;
} compositor_case_state_t;

static void state_destroy(compositor_case_state_t *state)
{
    if (!state) {
        return;
    }

    if (state->surfaces) {
        for (uint32_t i = 0; i < state->surface_count; ++i) {
            if (state->surfaces[i]) {
                grape_surface_destroy(state->surfaces[i]);
            }
        }
    }

    free(state->surfaces);
    free(state->rects);
    if (state->texture) {
        grape_texture_destroy(state->texture);
    }
    free(state);
}

static void restore_policy(grape_benchmark_runtime_t *runtime,
                           const compositor_case_state_t *state)
{
    if (!runtime || !state) {
        return;
    }

    grape_feature_set_mode(
        runtime->grape,
        GRAPE_FEATURE_PPA_A8_BLEND,
        state->old_blend
    );
    grape_set_rotation_backend(runtime->grape, state->old_rotation);
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    const compositor_case_config_t *config = bench_case->user_data;
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    if (!config || !display || config->rects == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    compositor_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    state->surface_count = config->surfaces;
    state->rect_count = config->rects;
    state->old_rotation = grape_get_rotation_backend(runtime->grape);
    grape_feature_get_mode(
        runtime->grape,
        GRAPE_FEATURE_PPA_A8_BLEND,
        &state->old_blend
    );

    if (config->surfaces > 0) {
        state->surfaces = calloc(config->surfaces, sizeof(*state->surfaces));
    }
    state->rects = calloc(config->rects, sizeof(*state->rects));
    if ((config->surfaces > 0 && !state->surfaces) || !state->rects) {
        state_destroy(state);
        return ESP_ERR_NO_MEM;
    }

    grape_texture_desc_t desc = {
        .width = 1,
        .height = 1,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_INTERNAL,
    };
    esp_err_t ret = grape_texture_create(runtime->grape, &desc, &state->texture);
    if (ret != ESP_OK) {
        state_destroy(state);
        return ret;
    }

    *(uint8_t *)grape_texture_pixels(state->texture) = 255;
    ret = grape_texture_invalidate(state->texture);
    if (ret != ESP_OK) {
        state_destroy(state);
        return ret;
    }

    ret = grape_feature_disable(runtime->grape, GRAPE_FEATURE_PPA_A8_BLEND);
    if (ret == ESP_OK) {
        ret = grape_set_rotation_backend(
            runtime->grape,
            GRAPE_ROTATION_BACKEND_AFFINE
        );
    }
    if (ret != ESP_OK) {
        restore_policy(runtime, state);
        state_destroy(state);
        return ret;
    }

    uint32_t rect_width = display->width < 16U ? display->width : 16U;
    uint32_t rect_height = display->height < 16U ? display->height : 16U;
    grape_rect_t test_rect = {
        .x = (int32_t)((display->width - rect_width) / 2U),
        .y = (int32_t)((display->height - rect_height) / 2U),
        .width = (int32_t)rect_width,
        .height = (int32_t)rect_height,
    };

    /*
     * Deliberately repeat the same small rectangle. This keeps raster/fill work
     * nearly constant while scaling the compositor's rect x surface traversal.
     */
    for (uint32_t r = 0; r < config->rects; ++r) {
        state->rects[r] = test_rect;
    }

    uint32_t intersect_count =
        (config->surfaces * config->intersect_percent + 99U) / 100U;
    for (uint32_t i = 0; i < config->surfaces; ++i) {
        ret = grape_surface_create(
            runtime->grape,
            state->texture,
            &state->surfaces[i]
        );
        if (ret != ESP_OK) {
            restore_policy(runtime, state);
            state_destroy(state);
            return ret;
        }

        if (i < intersect_count) {
            ret = grape_surface_set_position(
                state->surfaces[i],
                (float)test_rect.x,
                (float)test_rect.y
            );
        } else {
            ret = grape_surface_set_position(
                state->surfaces[i],
                -100.0f - (float)i,
                -100.0f
            );
        }
        if (ret != ESP_OK) {
            restore_policy(runtime, state);
            state_destroy(state);
            return ret;
        }
    }

    grape_benchmark_damage_clear(runtime->grape);
    *out_state = state;
    return ESP_OK;
}

static esp_err_t iteration(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void *opaque_state,
                           uint32_t sequence_iteration)
{
    (void)bench_case;
    (void)sequence_iteration;
    compositor_case_state_t *state = opaque_state;
    return grape_benchmark_render_rects(
        runtime->grape,
        state->rects,
        state->rect_count,
        false
    );
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    const compositor_case_config_t *config = bench_case->user_data;
    compositor_case_state_t *state = opaque_state;
    if (capacity < 4) {
        return 0;
    }

    out[0] = (grape_benchmark_metric_t) {
        "surface_rect_tests",
        "",
        (double)state->surface_count * state->rect_count,
    };
    out[1] = (grape_benchmark_metric_t) {
        "intersect_surfaces",
        "",
        (double)((state->surface_count * config->intersect_percent + 99U) / 100U),
    };
    out[2] = (grape_benchmark_metric_t) {
        "rects",
        "",
        state->rect_count,
    };
    out[3] = (grape_benchmark_metric_t) {
        "pixels_per_rect",
        "px",
        (double)state->rects[0].width * state->rects[0].height,
    };
    return 4;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)bench_case;
    compositor_case_state_t *state = opaque_state;
    if (!state) {
        return;
    }

    restore_policy(runtime, state);
    state_destroy(state);
}

#define COMPOSITOR_CASE_CAPACITY 36

static grape_benchmark_case_t s_cases[COMPOSITOR_CASE_CAPACITY];
static compositor_case_config_t s_configs[COMPOSITOR_CASE_CAPACITY];
static char s_names[COMPOSITOR_CASE_CAPACITY][48];
static size_t s_case_count;
static bool s_initialized;

static void add_case(uint32_t surfaces, uint32_t rects, uint32_t percent)
{
    if (s_case_count >= COMPOSITOR_CASE_CAPACITY) {
        return;
    }

    size_t n = s_case_count++;
    s_configs[n] = (compositor_case_config_t) {
        .surfaces = surfaces,
        .rects = rects,
        .intersect_percent = percent,
    };
    snprintf(
        s_names[n],
        sizeof(s_names[n]),
        "s%u_r%u_i%u",
        (unsigned)surfaces,
        (unsigned)rects,
        (unsigned)percent
    );
    s_cases[n] = (grape_benchmark_case_t) {
        .group = "compositor",
        .name = s_names[n],
        .kind = GRAPE_BENCHMARK_KIND_MICRO,
        .user_data = &s_configs[n],
        .setup = setup,
        .iteration = iteration,
        .collect_metrics = metrics,
        .teardown = teardown,
        .params = {
            { "surfaces", surfaces },
            { "rects", rects },
            { "intersect_pct", percent },
        },
    };
}

const grape_benchmark_case_t *grape_benchmark_compositor_cases(size_t *out_count)
{
    if (!s_initialized) {
        static const uint32_t surfaces[] = {10, 100, 500};
        static const uint32_t rects[] = {1, 4, 16};
        static const uint32_t percentages[] = {0, 50, 100};

        for (size_t r = 0; r < 3; ++r) {
            add_case(0, rects[r], 0);
        }
        for (size_t s = 0; s < 3; ++s) {
            for (size_t r = 0; r < 3; ++r) {
                for (size_t p = 0; p < 3; ++p) {
                    add_case(surfaces[s], rects[r], percentages[p]);
                }
            }
        }
        s_initialized = true;
    }

    if (out_count) {
        *out_count = s_case_count;
    }
    return s_cases;
}
