#include "grape_benchmark_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t rects;
} fragmentation_case_config_t;

typedef struct {
    grape_texture_t *texture;
    grape_surface_t *surfaces[10];
    grape_rect_t rects[16];
    uint32_t rect_count;
    uint64_t pixels;
} fragmentation_case_state_t;

static void destroy_state(fragmentation_case_state_t *state)
{
    if (!state) {
        return;
    }
    for (size_t i = 0; i < 10; ++i) {
        if (state->surfaces[i]) {
            grape_surface_destroy(state->surfaces[i]);
        }
    }
    if (state->texture) {
        grape_texture_destroy(state->texture);
    }
    free(state);
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    const fragmentation_case_config_t *config = bench_case->user_data;
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    if (!config || !display || config->rects == 0 || config->rects > 16) {
        return ESP_ERR_INVALID_ARG;
    }

    fragmentation_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }
    state->rect_count = config->rects;

    grape_texture_desc_t desc = {
        .width = 32,
        .height = 32,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_INTERNAL,
    };
    esp_err_t ret = grape_texture_create(runtime->grape, &desc, &state->texture);
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    memset(
        grape_texture_pixels(state->texture),
        255,
        grape_texture_stride(state->texture) * 32U
    );
    ret = grape_texture_invalidate(state->texture);
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    uint32_t region_width = display->width < 128U ? display->width : 128U;
    uint32_t region_height = display->height < 1024U ? display->height : 1024U;
    region_height -= region_height % config->rects;
    if (region_height == 0) {
        region_height = config->rects;
    }

    uint32_t region_x = (display->width - region_width) / 2U;
    uint32_t region_y = (display->height - region_height) / 2U;
    uint32_t slice_height = region_height / config->rects;

    for (uint32_t i = 0; i < config->rects; ++i) {
        state->rects[i] = (grape_rect_t) {
            .x = (int32_t)region_x,
            .y = (int32_t)(region_y + i * slice_height),
            .width = (int32_t)region_width,
            .height = (int32_t)slice_height,
        };
        state->pixels += (uint64_t)region_width * slice_height;
    }

    for (uint32_t i = 0; i < 10; ++i) {
        ret = grape_surface_create(
            runtime->grape,
            &GRAPE_SURFACE_DESC_TEXTURE(state->texture),
            &state->surfaces[i]
        );
        if (ret != ESP_OK) {
            destroy_state(state);
            return ret;
        }

        float x = (float)(region_x + (i * 29U) % (region_width ? region_width : 1U));
        float y = (float)(region_y + (i * 97U) % (region_height ? region_height : 1U));
        ret = grape_surface_set_position(state->surfaces[i], x, y);
        if (ret != ESP_OK) {
            destroy_state(state);
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
    fragmentation_case_state_t *state = opaque_state;
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
    (void)bench_case;
    fragmentation_case_state_t *state = opaque_state;
    if (capacity < 2) {
        return 0;
    }

    out[0] = (grape_benchmark_metric_t) {
        "render_pixels",
        "px",
        (double)state->pixels,
    };
    out[1] = (grape_benchmark_metric_t) {
        "rects",
        "",
        state->rect_count,
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

static fragmentation_case_config_t s_configs[] = {
    {1}, {2}, {4}, {8}, {16},
};
static grape_benchmark_case_t s_cases[5];
static char s_names[5][24];
static bool s_initialized;

const grape_benchmark_case_t *grape_benchmark_fragmentation_cases(size_t *out_count)
{
    if (!s_initialized) {
        for (size_t i = 0; i < 5; ++i) {
            snprintf(
                s_names[i],
                sizeof(s_names[i]),
                "rects_%u",
                (unsigned)s_configs[i].rects
            );
            s_cases[i] = (grape_benchmark_case_t) {
                .group = "fragmentation",
                .name = s_names[i],
                .kind = GRAPE_BENCHMARK_KIND_PIPELINE,
                .user_data = &s_configs[i],
                .setup = setup,
                .iteration = iteration,
                .collect_metrics = metrics,
                .teardown = teardown,
                .params = {
                    { "rects", s_configs[i].rects },
                    { "target_width", 128 },
                    { "target_height", 1024 },
                },
            };
        }
        s_initialized = true;
    }

    if (out_count) {
        *out_count = 5;
    }
    return s_cases;
}
