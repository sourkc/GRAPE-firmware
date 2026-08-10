#include "grape_benchmark_internal.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    GLYPH_CACHE_COLD = 0,
    GLYPH_CACHE_EXACT,
    GLYPH_CACHE_SCALED,
    GLYPH_CACHE_EVICTION,
} glyph_cache_operation_t;

typedef struct {
    glyph_cache_operation_t operation;
    float target_ppem;
    float source_ppem;
    float target_step_ppem;
    size_t capacity_bytes;
    uint8_t samples_per_axis;
    uint32_t working_set;
} glyph_cache_case_config_t;

typedef struct {
    grape_font_t *font;
    grape_glyph_cache_t *cache;
    grape_text_raster_t raster;
    grape_glyph_cache_stats_t stats;
    float last_target_ppem;
} glyph_cache_case_state_t;

static float pixels_per_unit(const grape_font_t *font, float ppem)
{
    uint16_t units = grape_font_units_per_em(font);
    return units ? ppem / (float)units : 0.0f;
}

static void destroy_raster(glyph_cache_case_state_t *state)
{
    if (!state || !state->raster.texture) {
        return;
    }
    grape_texture_destroy(state->raster.texture);
    state->raster = (grape_text_raster_t){0};
}

static esp_err_t rasterize_codepoint(glyph_cache_case_state_t *state,
                                     uint32_t codepoint,
                                     float ppem,
                                     uint8_t samples_per_axis)
{
    grape_path_rasterize_config_t config = GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT();
    config.pixels_per_unit = pixels_per_unit(state->font, ppem);
    config.samples_per_axis = samples_per_axis;
    config.padding_pixels = 2U;
    config.memory = GRAPE_MEMORY_PSRAM;

    state->last_target_ppem = ppem;
    return grape_text_rasterize_codepoints_a8(
        state->cache,
        state->font,
        &codepoint,
        1U,
        &config,
        &state->raster
    );
}

static esp_err_t warm_codepoint(glyph_cache_case_state_t *state,
                                uint32_t codepoint,
                                float ppem,
                                uint8_t samples_per_axis)
{
    esp_err_t ret = rasterize_codepoint(
        state,
        codepoint,
        ppem,
        samples_per_axis
    );
    destroy_raster(state);
    return ret;
}

static void destroy_state(glyph_cache_case_state_t *state)
{
    if (!state) {
        return;
    }
    destroy_raster(state);
    if (state->cache) {
        grape_glyph_cache_destroy(state->cache);
    }
    if (state->font) {
        grape_font_destroy(state->font);
    }
    free(state);
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    const glyph_cache_case_config_t *config = bench_case->user_data;
    if (!config || !out_state) {
        return ESP_ERR_INVALID_ARG;
    }

    glyph_cache_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = grape_benchmark_fixture_font_load(&state->font);
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    grape_glyph_cache_config_t cache_config = GRAPE_GLYPH_CACHE_CONFIG_DEFAULT();
    cache_config.capacity_bytes = config->capacity_bytes;
    cache_config.memory = GRAPE_MEMORY_PSRAM;
    cache_config.raster_padding_pixels = 2U;
    cache_config.max_downscale_ratio = 2.0f;
    cache_config.scale_backend = GRAPE_GLYPH_CACHE_SCALE_CPU;
    ret = grape_glyph_cache_create(runtime->grape, &cache_config, &state->cache);
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    if (config->operation == GLYPH_CACHE_EXACT) {
        ret = warm_codepoint(
            state,
            'G',
            config->target_ppem,
            config->samples_per_axis
        );
    } else if (config->operation == GLYPH_CACHE_SCALED) {
        ret = warm_codepoint(
            state,
            'G',
            config->source_ppem,
            config->samples_per_axis
        );
    }

    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    *out_state = state;
    return ESP_OK;
}

static esp_err_t iteration(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void *opaque_state,
                           uint32_t sequence_iteration)
{
    (void)runtime;
    const glyph_cache_case_config_t *config = bench_case->user_data;
    glyph_cache_case_state_t *state = opaque_state;

    uint32_t codepoint = 'G';
    float ppem = config->target_ppem;
    if (config->operation == GLYPH_CACHE_SCALED) {
        ppem += config->target_step_ppem * (float)sequence_iteration;
        if (ppem >= config->source_ppem) {
            return ESP_ERR_INVALID_STATE;
        }
    } else if (config->operation == GLYPH_CACHE_EVICTION) {
        uint32_t working_set = config->working_set ? config->working_set : 1U;
        codepoint = 'A' + (sequence_iteration % working_set);
    }

    return rasterize_codepoint(
        state,
        codepoint,
        ppem,
        config->samples_per_axis
    );
}

static void after_iteration(grape_benchmark_runtime_t *runtime,
                            const grape_benchmark_case_t *bench_case,
                            void *opaque_state,
                            uint32_t sequence_iteration)
{
    (void)runtime;
    (void)sequence_iteration;
    const glyph_cache_case_config_t *config = bench_case->user_data;
    glyph_cache_case_state_t *state = opaque_state;

    destroy_raster(state);
    if (config->operation == GLYPH_CACHE_COLD) {
        grape_glyph_cache_clear(state->cache);
    }
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    (void)bench_case;
    glyph_cache_case_state_t *state = opaque_state;
    if (capacity < 9U) {
        return 0U;
    }

    grape_glyph_cache_get_stats(state->cache, &state->stats);
    out[0] = (grape_benchmark_metric_t){
        "requests", "", (double)state->stats.requests,
    };
    out[1] = (grape_benchmark_metric_t){
        "exact_hits", "", (double)state->stats.exact_hits,
    };
    out[2] = (grape_benchmark_metric_t){
        "scaled_hits", "", (double)state->stats.scaled_hits,
    };
    out[3] = (grape_benchmark_metric_t){
        "misses", "", (double)state->stats.misses,
    };
    out[4] = (grape_benchmark_metric_t){
        "cpu_scales", "", (double)state->stats.cpu_scales,
    };
    out[5] = (grape_benchmark_metric_t){
        "evictions", "", (double)state->stats.evictions,
    };
    out[6] = (grape_benchmark_metric_t){
        "entries", "", (double)state->stats.entry_count,
    };
    out[7] = (grape_benchmark_metric_t){
        "used_bytes", "B", (double)state->stats.used_bytes,
    };
    out[8] = (grape_benchmark_metric_t){
        "last_target_ppem", "px/em", state->last_target_ppem,
    };
    return 9U;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)runtime;
    (void)bench_case;
    destroy_state(opaque_state);
}

#define GLYPH_CACHE_CASE_CAPACITY 20

static grape_benchmark_case_t s_cases[GLYPH_CACHE_CASE_CAPACITY];
static glyph_cache_case_config_t s_configs[GLYPH_CACHE_CASE_CAPACITY];
static char s_names[GLYPH_CACHE_CASE_CAPACITY][64];
static size_t s_case_count;
static bool s_initialized;

static const char *operation_name(glyph_cache_operation_t operation)
{
    switch (operation) {
        case GLYPH_CACHE_COLD: return "cold";
        case GLYPH_CACHE_EXACT: return "exact";
        case GLYPH_CACHE_SCALED: return "scaled";
        case GLYPH_CACHE_EVICTION: return "eviction";
        default: return "unknown";
    }
}

static void add_case(glyph_cache_operation_t operation,
                     float target_ppem,
                     float source_ppem,
                     float step_ppem,
                     size_t capacity_bytes,
                     uint32_t working_set)
{
    if (s_case_count >= GLYPH_CACHE_CASE_CAPACITY) {
        return;
    }

    size_t index = s_case_count++;
    s_configs[index] = (glyph_cache_case_config_t){
        .operation = operation,
        .target_ppem = target_ppem,
        .source_ppem = source_ppem,
        .target_step_ppem = step_ppem,
        .capacity_bytes = capacity_bytes,
        .samples_per_axis = 4U,
        .working_set = working_set,
    };

    if (operation == GLYPH_CACHE_SCALED) {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s_%.0f_from_%.0f",
            operation_name(operation), target_ppem, source_ppem
        );
    } else if (operation == GLYPH_CACHE_EVICTION) {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s_%uKiB_ws%u",
            operation_name(operation),
            (unsigned)(capacity_bytes / 1024U),
            (unsigned)working_set
        );
    } else {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s_%.0f", operation_name(operation), target_ppem
        );
    }

    uint32_t warmup = operation == GLYPH_CACHE_SCALED ? 2U : 4U;
    uint32_t measured = operation == GLYPH_CACHE_SCALED ? 16U : 24U;
    s_cases[index] = (grape_benchmark_case_t){
        .group = "glyph_cache",
        .name = s_names[index],
        .kind = GRAPE_BENCHMARK_KIND_PIPELINE,
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
            { "target_ppem", target_ppem },
            { "source_ppem", source_ppem },
            { "capacity_bytes", (double)capacity_bytes },
            { "working_set", working_set },
        },
    };
}

const grape_benchmark_case_t *grape_benchmark_glyph_cache_cases(size_t *out_count)
{
    if (!s_initialized) {
        static const float sizes[] = { 32.0f, 64.0f, 128.0f, 192.0f };
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            add_case(
                GLYPH_CACHE_COLD,
                sizes[i], 0.0f, 0.0f,
                4U * 1024U * 1024U,
                0U
            );
            add_case(
                GLYPH_CACHE_EXACT,
                sizes[i], 0.0f, 0.0f,
                4U * 1024U * 1024U,
                0U
            );
        }

        add_case(GLYPH_CACHE_SCALED, 32.0f, 64.0f, 1.0f,
                 4U * 1024U * 1024U, 0U);
        add_case(GLYPH_CACHE_SCALED, 48.0f, 96.0f, 2.0f,
                 4U * 1024U * 1024U, 0U);
        add_case(GLYPH_CACHE_SCALED, 72.0f, 144.0f, 3.0f,
                 4U * 1024U * 1024U, 0U);
        add_case(GLYPH_CACHE_SCALED, 96.0f, 192.0f, 4.0f,
                 8U * 1024U * 1024U, 0U);

        add_case(GLYPH_CACHE_EVICTION, 128.0f, 0.0f, 0.0f,
                 16U * 1024U, 16U);
        add_case(GLYPH_CACHE_EVICTION, 128.0f, 0.0f, 0.0f,
                 32U * 1024U, 16U);
        add_case(GLYPH_CACHE_EVICTION, 128.0f, 0.0f, 0.0f,
                 64U * 1024U, 16U);

        s_initialized = true;
    }

    if (out_count) {
        *out_count = s_case_count;
    }
    return s_cases;
}
