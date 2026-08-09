#include "grape_benchmark_internal.h"

#include <stdlib.h>

typedef enum {
    PRESENT_BAND = 0,
    PRESENT_NEAR_PAIR,
    PRESENT_FAR_PAIR,
} presentation_pattern_t;

typedef struct {
    presentation_pattern_t pattern;
    uint32_t height;
} presentation_case_config_t;

typedef struct {
    grape_rect_t rects[2];
    uint32_t rect_count;
    uint32_t dirty_y_span;
    uint64_t render_pixels;
} presentation_case_state_t;

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    const presentation_case_config_t *config = bench_case->user_data;
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    if (!config || !display) {
        return ESP_ERR_INVALID_ARG;
    }

    presentation_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    /* Start presentation cases with both physical framebuffers identical. */
    grape_rect_t full_screen = {
        .x = 0,
        .y = 0,
        .width = (int32_t)display->width,
        .height = (int32_t)display->height,
    };
    for (int i = 0; i < 2; ++i) {
        esp_err_t sync_ret = grape_benchmark_render_rects(
            runtime->grape,
            &full_screen,
            1,
            true
        );
        if (sync_ret != ESP_OK) {
            free(state);
            return sync_ret;
        }
    }

    if (config->pattern == PRESENT_BAND) {
        uint32_t height = config->height > display->height
            ? display->height
            : config->height;
        state->rect_count = 1;
        state->rects[0] = (grape_rect_t) {
            .x = 0,
            .y = (int32_t)((display->height - height) / 2U),
            .width = (int32_t)display->width,
            .height = (int32_t)height,
        };
    } else {
        uint32_t width = display->width < 64U ? display->width : 64U;
        uint32_t height = display->height < 64U ? display->height : 64U;
        uint32_t x = (display->width - width) / 2U;

        state->rect_count = 2;
        if (config->pattern == PRESENT_NEAR_PAIR) {
            uint32_t gap = display->height >= height * 2U + 16U ? 16U : 0U;
            uint32_t pair_height = height * 2U + gap;
            uint32_t y = display->height > pair_height
                ? (display->height - pair_height) / 2U
                : 0U;
            state->rects[0] = (grape_rect_t) {
                (int32_t)x,
                (int32_t)y,
                (int32_t)width,
                (int32_t)height,
            };
            state->rects[1] = (grape_rect_t) {
                (int32_t)x,
                (int32_t)(y + height + gap),
                (int32_t)width,
                (int32_t)height,
            };
        } else {
            state->rects[0] = (grape_rect_t) {
                (int32_t)x,
                0,
                (int32_t)width,
                (int32_t)height,
            };
            state->rects[1] = (grape_rect_t) {
                (int32_t)x,
                (int32_t)(display->height - height),
                (int32_t)width,
                (int32_t)height,
            };
        }
    }

    int32_t y_min = state->rects[0].y;
    int32_t y_max = state->rects[0].y + state->rects[0].height;
    for (uint32_t i = 0; i < state->rect_count; ++i) {
        if (state->rects[i].y < y_min) {
            y_min = state->rects[i].y;
        }
        int32_t y1 = state->rects[i].y + state->rects[i].height;
        if (y1 > y_max) {
            y_max = y1;
        }
        state->render_pixels +=
            (uint64_t)state->rects[i].width * state->rects[i].height;
    }
    state->dirty_y_span = (uint32_t)(y_max - y_min);

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
    presentation_case_state_t *state = opaque_state;
    return grape_benchmark_render_rects(
        runtime->grape,
        state->rects,
        state->rect_count,
        true
    );
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    (void)bench_case;
    presentation_case_state_t *state = opaque_state;
    if (capacity < 3) {
        return 0;
    }

    out[0] = (grape_benchmark_metric_t) {
        "render_pixels",
        "px",
        (double)state->render_pixels,
    };
    out[1] = (grape_benchmark_metric_t) {
        "dirty_y_span",
        "rows",
        state->dirty_y_span,
    };
    out[2] = (grape_benchmark_metric_t) {
        "rects",
        "",
        state->rect_count,
    };
    return 3;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)runtime;
    (void)bench_case;
    free(opaque_state);
}

static presentation_case_config_t s_configs[] = {
    {PRESENT_BAND, 16},
    {PRESENT_BAND, 64},
    {PRESENT_BAND, 256},
    {PRESENT_BAND, 640},
    {PRESENT_BAND, 1280},
    {PRESENT_NEAR_PAIR, 0},
    {PRESENT_FAR_PAIR, 0},
};

static const char *s_names[] = {
    "band_16",
    "band_64",
    "band_256",
    "band_640",
    "band_full",
    "pair_near",
    "pair_far",
};

static grape_benchmark_case_t s_cases[7];
static bool s_initialized;

const grape_benchmark_case_t *grape_benchmark_presentation_cases(size_t *out_count)
{
    if (!s_initialized) {
        for (size_t i = 0; i < 7; ++i) {
            s_cases[i] = (grape_benchmark_case_t) {
                .group = "presentation",
                .name = s_names[i],
                .kind = GRAPE_BENCHMARK_KIND_PIPELINE,
                .flags = GRAPE_BENCHMARK_CASE_CAPTURE_REFRESH_WAIT,
                .warmup_iterations = 4,
                .measured_iterations = 24,
                .user_data = &s_configs[i],
                .setup = setup,
                .iteration = iteration,
                .collect_metrics = metrics,
                .teardown = teardown,
                .params = {
                    { "pattern", s_configs[i].pattern },
                    { "height", s_configs[i].height },
                },
            };
        }
        s_initialized = true;
    }

    if (out_count) {
        *out_count = 7;
    }
    return s_cases;
}
