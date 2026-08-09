#include "grape_benchmark_internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    PLAN_PATTERN_SOLID = 0,
    PLAN_PATTERN_H_STRIPES,
    PLAN_PATTERN_V_STRIPES,
    PLAN_PATTERN_CHECKER,
    PLAN_PATTERN_NOISE,
    PLAN_PATTERN_ISLANDS,
} plan_pattern_t;

typedef struct {
    plan_pattern_t pattern;
    uint32_t amount;
} plan_case_config_t;

typedef struct {
    uint8_t *bitmap;
    size_t bitmap_size;
    grape_benchmark_damage_grid_info_t grid;
    grape_benchmark_damage_plan_result_t last;
} plan_case_state_t;

static void set_tile(plan_case_state_t *state, uint32_t x, uint32_t y)
{
    if (x >= state->grid.tile_columns || y >= state->grid.tile_rows) return;
    size_t index = (size_t)y * state->grid.tile_columns + x;
    state->bitmap[index >> 3U] |= (uint8_t)(1U << (index & 7U));
}

static void fill_pattern(plan_case_state_t *state, const plan_case_config_t *config)
{
    memset(state->bitmap, 0, state->bitmap_size);
    uint32_t w = state->grid.tile_columns;
    uint32_t h = state->grid.tile_rows;

    switch (config->pattern) {
        case PLAN_PATTERN_SOLID: {
            uint32_t target_w = (w * config->amount + 99U) / 100U;
            if (target_w == 0) target_w = 1;
            if (target_w > w) target_w = w;
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < target_w; ++x)
                    set_tile(state, x, y);
            break;
        }
        case PLAN_PATTERN_H_STRIPES:
            for (uint32_t y = 0; y < h; y += 2U)
                for (uint32_t x = 0; x < w; ++x)
                    set_tile(state, x, y);
            break;
        case PLAN_PATTERN_V_STRIPES:
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; x += 2U)
                    set_tile(state, x, y);
            break;
        case PLAN_PATTERN_CHECKER:
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    if (((x + y) & 1U) == 0U) set_tile(state, x, y);
            break;
        case PLAN_PATTERN_NOISE:
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    uint32_t index = y * w + x;
                    if ((grape_benchmark_hash_u32(0x9e3779b9U ^ index) % 100U) < config->amount)
                        set_tile(state, x, y);
                }
            }
            break;
        case PLAN_PATTERN_ISLANDS: {
            uint32_t count = config->amount;
            for (uint32_t i = 0; i < count; ++i) {
                uint32_t x = count > 1 ? (i * (w - 1U)) / (count - 1U) : w / 2U;
                uint32_t perm = grape_benchmark_hash_u32(i * 0x45d9f3bU);
                uint32_t y = h ? perm % h : 0;
                set_tile(state, x, y);
                if (x + 1U < w) set_tile(state, x + 1U, y);
                if (y + 1U < h) set_tile(state, x, y + 1U);
            }
            break;
        }
    }
}

static esp_err_t plan_setup(grape_benchmark_runtime_t *runtime,
                            const grape_benchmark_case_t *bench_case,
                            void **out_state)
{
    const plan_case_config_t *config = bench_case->user_data;
    plan_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) return ESP_ERR_NO_MEM;

    esp_err_t ret = grape_benchmark_damage_grid_info(runtime->grape, &state->grid);
    if (ret != ESP_OK) { free(state); return ret; }
    state->bitmap_size = state->grid.bitmap_size;
    state->bitmap = calloc(1, state->bitmap_size);
    if (!state->bitmap) { free(state); return ESP_ERR_NO_MEM; }
    fill_pattern(state, config);
    *out_state = state;
    return ESP_OK;
}

static esp_err_t plan_iteration(grape_benchmark_runtime_t *runtime,
                                const grape_benchmark_case_t *bench_case,
                                void *opaque_state,
                                uint32_t sequence_iteration)
{
    (void)bench_case;
    (void)sequence_iteration;
    plan_case_state_t *state = opaque_state;
    return grape_benchmark_damage_plan_bitmap(
        runtime->grape,
        state->bitmap,
        state->bitmap_size,
        &state->last
    );
}

static size_t plan_metrics(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void *opaque_state,
                           grape_benchmark_metric_t *out,
                           size_t capacity)
{
    (void)runtime; (void)bench_case;
    if (capacity < 8) return 0;
    plan_case_state_t *state = opaque_state;
    double dirty_pixel_equiv = (double)state->last.dirty_tiles *
        (double)state->grid.tile_size * (double)state->grid.tile_size;
    double overdraw = dirty_pixel_equiv > 0.0
        ? (double)state->last.final_pixels / dirty_pixel_equiv : 0.0;
    out[0] = (grape_benchmark_metric_t){ "dirty_tiles", "", state->last.dirty_tiles };
    out[1] = (grape_benchmark_metric_t){ "total_tiles", "", state->last.total_tiles };
    out[2] = (grape_benchmark_metric_t){ "splits", "", state->last.planner_splits };
    out[3] = (grape_benchmark_metric_t){ "candidates", "", state->last.split_candidates };
    out[4] = (grape_benchmark_metric_t){ "final_rects", "", state->last.final_rects };
    out[5] = (grape_benchmark_metric_t){ "final_pixels", "px", (double)state->last.final_pixels };
    out[6] = (grape_benchmark_metric_t){ "overdraw_ratio", "x", overdraw };
    out[7] = (grape_benchmark_metric_t){ "fullscreen", "", state->last.full_screen ? 1.0 : 0.0 };
    return 8;
}

static void plan_teardown(grape_benchmark_runtime_t *runtime,
                          const grape_benchmark_case_t *bench_case,
                          void *opaque_state)
{
    (void)runtime; (void)bench_case;
    plan_case_state_t *state = opaque_state;
    if (!state) return;
    free(state->bitmap);
    free(state);
}

#define PLAN_CASE_CAPACITY 32
static grape_benchmark_case_t s_cases[PLAN_CASE_CAPACITY];
static plan_case_config_t s_configs[PLAN_CASE_CAPACITY];
static char s_names[PLAN_CASE_CAPACITY][48];
static size_t s_count;
static bool s_initialized;

static const char *pattern_name(plan_pattern_t pattern)
{
    switch (pattern) {
        case PLAN_PATTERN_SOLID: return "solid";
        case PLAN_PATTERN_H_STRIPES: return "h_stripes";
        case PLAN_PATTERN_V_STRIPES: return "v_stripes";
        case PLAN_PATTERN_CHECKER: return "checker";
        case PLAN_PATTERN_NOISE: return "noise";
        case PLAN_PATTERN_ISLANDS: return "islands";
        default: return "unknown";
    }
}

static void add_case(plan_pattern_t pattern, uint32_t amount)
{
    if (s_count >= PLAN_CASE_CAPACITY) {
        return;
    }
    size_t n = s_count++;
    s_configs[n] = (plan_case_config_t){ pattern, amount };
    snprintf(s_names[n], sizeof(s_names[n]), "%s_%u", pattern_name(pattern), (unsigned)amount);
    s_cases[n] = (grape_benchmark_case_t){
        .group = "damage_plan",
        .name = s_names[n],
        .kind = GRAPE_BENCHMARK_KIND_MICRO,
        .user_data = &s_configs[n],
        .setup = plan_setup,
        .iteration = plan_iteration,
        .collect_metrics = plan_metrics,
        .teardown = plan_teardown,
        .params = { { "pattern", (double)pattern }, { "amount", (double)amount } },
    };
}

const grape_benchmark_case_t *grape_benchmark_damage_plan_cases(size_t *out_count)
{
    if (!s_initialized) {
        static const uint32_t densities[] = {5, 10, 25, 50, 75, 100};
        for (size_t i = 0; i < sizeof(densities)/sizeof(densities[0]); ++i)
            add_case(PLAN_PATTERN_SOLID, densities[i]);
        add_case(PLAN_PATTERN_H_STRIPES, 50);
        add_case(PLAN_PATTERN_V_STRIPES, 50);
        add_case(PLAN_PATTERN_CHECKER, 50);
        static const uint32_t noise[] = {5, 10, 25, 50};
        for (size_t i = 0; i < sizeof(noise)/sizeof(noise[0]); ++i)
            add_case(PLAN_PATTERN_NOISE, noise[i]);
        static const uint32_t islands[] = {4, 8, 16, 24, 32};
        for (size_t i = 0; i < sizeof(islands)/sizeof(islands[0]); ++i)
            add_case(PLAN_PATTERN_ISLANDS, islands[i]);
        s_initialized = true;
    }
    if (out_count) *out_count = s_count;
    return s_cases;
}
