#include "grape_benchmark_internal.h"

#include <stdlib.h>

typedef enum {
    FONT_OP_LOAD = 0,
    FONT_OP_CMAP_LOOKUP,
    FONT_OP_METRICS_LOOKUP,
    FONT_OP_GLYPH_PATH,
} font_operation_t;

typedef struct {
    font_operation_t operation;
    uint32_t lookup_count;
    uint32_t codepoint;
} font_case_config_t;

typedef struct {
    grape_font_t *font;
    grape_path_t *path;
    uint16_t last_glyph_id;
    grape_font_glyph_metrics_t last_metrics;
} font_case_state_t;

static uint32_t benchmark_codepoint(uint32_t index)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    if ((index % 37U) == 36U) {
        return 0x1F347U;
    }
    return (uint8_t)alphabet[index % (sizeof(alphabet) - 1U)];
}

static void destroy_iteration_font(font_case_state_t *state)
{
    if (!state || !state->font) {
        return;
    }
    grape_font_destroy(state->font);
    state->font = NULL;
}

static void destroy_state(font_case_state_t *state)
{
    if (!state) {
        return;
    }
    destroy_iteration_font(state);
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
    const font_case_config_t *config = bench_case->user_data;
    if (!config || !out_state) {
        return ESP_ERR_INVALID_ARG;
    }

    font_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    if (config->operation != FONT_OP_LOAD) {
        ret = grape_benchmark_fixture_font_load(&state->font);
    }
    if (ret == ESP_OK && config->operation == FONT_OP_GLYPH_PATH) {
        ret = grape_path_create(&state->path);
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
    const font_case_config_t *config = bench_case->user_data;
    font_case_state_t *state = opaque_state;

    switch (config->operation) {
        case FONT_OP_LOAD:
            return grape_benchmark_fixture_font_load(&state->font);

        case FONT_OP_CMAP_LOOKUP:
            for (uint32_t i = 0; i < config->lookup_count; ++i) {
                esp_err_t ret = grape_font_get_glyph_id(
                    state->font,
                    benchmark_codepoint(sequence_iteration + i),
                    &state->last_glyph_id
                );
                if (ret != ESP_OK) {
                    return ret;
                }
            }
            return ESP_OK;

        case FONT_OP_METRICS_LOOKUP:
            for (uint32_t i = 0; i < config->lookup_count; ++i) {
                esp_err_t ret = grape_font_get_glyph_id(
                    state->font,
                    benchmark_codepoint(sequence_iteration + i),
                    &state->last_glyph_id
                );
                if (ret == ESP_OK) {
                    ret = grape_font_get_glyph_metrics(
                        state->font,
                        state->last_glyph_id,
                        &state->last_metrics
                    );
                }
                if (ret != ESP_OK) {
                    return ret;
                }
            }
            return ESP_OK;

        case FONT_OP_GLYPH_PATH: {
            esp_err_t ret = grape_path_clear(state->path);
            if (ret == ESP_OK) {
                ret = grape_font_get_glyph_id(
                    state->font,
                    config->codepoint,
                    &state->last_glyph_id
                );
            }
            if (ret == ESP_OK) {
                ret = grape_font_get_glyph_path(
                    state->font,
                    state->last_glyph_id,
                    state->path
                );
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
    const font_case_config_t *config = bench_case->user_data;
    if (config->operation == FONT_OP_LOAD) {
        destroy_iteration_font(opaque_state);
    }
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    const font_case_config_t *config = bench_case->user_data;
    font_case_state_t *state = opaque_state;
    if (capacity < 4U) {
        return 0U;
    }

    out[0] = (grape_benchmark_metric_t){
        "lookups", "", config->lookup_count,
    };
    out[1] = (grape_benchmark_metric_t){
        "glyph_id", "", state->last_glyph_id,
    };
    out[2] = (grape_benchmark_metric_t){
        "advance_width", "font_unit", state->last_metrics.advance_width,
    };
    out[3] = (grape_benchmark_metric_t){
        "codepoint", "", config->codepoint,
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

#define FONT_CASE_CAPACITY 16

static grape_benchmark_case_t s_cases[FONT_CASE_CAPACITY];
static font_case_config_t s_configs[FONT_CASE_CAPACITY];
static char s_names[FONT_CASE_CAPACITY][56];
static size_t s_case_count;
static bool s_initialized;

static const char *operation_name(font_operation_t operation)
{
    switch (operation) {
        case FONT_OP_LOAD: return "load";
        case FONT_OP_CMAP_LOOKUP: return "cmap";
        case FONT_OP_METRICS_LOOKUP: return "metrics";
        case FONT_OP_GLYPH_PATH: return "glyph_path";
        default: return "unknown";
    }
}

static void add_case(font_operation_t operation,
                     uint32_t lookup_count,
                     uint32_t codepoint)
{
    if (s_case_count >= FONT_CASE_CAPACITY) {
        return;
    }

    size_t index = s_case_count++;
    s_configs[index] = (font_case_config_t){
        .operation = operation,
        .lookup_count = lookup_count,
        .codepoint = codepoint,
    };

    if (operation == FONT_OP_GLYPH_PATH) {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s_U%04X", operation_name(operation), (unsigned)codepoint
        );
    } else if (lookup_count > 0U) {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s_n%u", operation_name(operation), (unsigned)lookup_count
        );
    } else {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s", operation_name(operation)
        );
    }

    s_cases[index] = (grape_benchmark_case_t){
        .group = "font",
        .name = s_names[index],
        .kind = operation == FONT_OP_LOAD
            ? GRAPE_BENCHMARK_KIND_LIFECYCLE
            : GRAPE_BENCHMARK_KIND_MICRO,
        .warmup_iterations = operation == FONT_OP_LOAD ? 2U : 4U,
        .measured_iterations = operation == FONT_OP_LOAD ? 16U : 32U,
        .user_data = &s_configs[index],
        .setup = setup,
        .iteration = iteration,
        .after_iteration = after_iteration,
        .collect_metrics = metrics,
        .teardown = teardown,
        .params = {
            { "operation", operation },
            { "lookups", lookup_count },
            { "codepoint", codepoint },
        },
    };
}

const grape_benchmark_case_t *grape_benchmark_font_cases(size_t *out_count)
{
    if (!s_initialized) {
        add_case(FONT_OP_LOAD, 0U, 0U);

        static const uint32_t counts[] = { 1U, 16U, 64U };
        for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
            add_case(FONT_OP_CMAP_LOOKUP, counts[i], 0U);
            add_case(FONT_OP_METRICS_LOOKUP, counts[i], 0U);
        }

        add_case(FONT_OP_GLYPH_PATH, 0U, 'A');
        add_case(FONT_OP_GLYPH_PATH, 0U, 'B');
        add_case(FONT_OP_GLYPH_PATH, 0U, 'G');
        add_case(FONT_OP_GLYPH_PATH, 0U, 'R');

        s_initialized = true;
    }

    if (out_count) {
        *out_count = s_case_count;
    }
    return s_cases;
}
