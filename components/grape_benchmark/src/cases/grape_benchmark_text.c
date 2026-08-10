#include "grape_benchmark_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    TEXT_OP_MEASURE = 0,
    TEXT_OP_BUILD_PATH,
    TEXT_OP_RASTER_COLD,
    TEXT_OP_RASTER_WARM,
    TEXT_OP_RASTER_SCALED,
    TEXT_OP_SCENE_MOVE,
    TEXT_OP_SCENE_RESIZE_SCALED,
    TEXT_OP_SCENE_RESIZE_EXACT,
} text_operation_t;

typedef struct {
    text_operation_t operation;
    uint32_t length;
    float target_ppem;
    float source_ppem;
    float target_step_ppem;
    uint8_t samples_per_axis;
} text_case_config_t;

typedef struct {
    grape_font_t *font;
    grape_glyph_cache_t *cache;
    grape_path_t *path;
    grape_text_raster_t raster;
    grape_texture_t *display_texture;
    grape_surface_t *surface;
    uint32_t *codepoints;
    size_t codepoint_count;
    grape_text_measurement_t measurement;
    grape_glyph_cache_stats_t stats;
    float last_ppem;
} text_case_state_t;

static const char s_pattern[] = "GRAPEVECTORCACHETEXTBENCHMARK";

static float pixels_per_unit(const grape_font_t *font, float ppem)
{
    uint16_t units = grape_font_units_per_em(font);
    return units ? ppem / (float)units : 0.0f;
}

static esp_err_t make_codepoints(uint32_t length,
                                 uint32_t **out_codepoints,
                                 size_t *out_count)
{
    if (!out_codepoints || !out_count || length == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t *codepoints = calloc(length, sizeof(*codepoints));
    if (!codepoints) {
        return ESP_ERR_NO_MEM;
    }

    size_t pattern_length = strlen(s_pattern);
    for (uint32_t i = 0; i < length; ++i) {
        codepoints[i] = (uint8_t)s_pattern[i % pattern_length];
    }

    *out_codepoints = codepoints;
    *out_count = length;
    return ESP_OK;
}

static void destroy_raster(text_case_state_t *state)
{
    if (!state || !state->raster.texture) {
        return;
    }
    grape_texture_destroy(state->raster.texture);
    state->raster = (grape_text_raster_t){0};
}

static esp_err_t rasterize(text_case_state_t *state,
                           float ppem,
                           uint8_t samples_per_axis)
{
    grape_path_rasterize_config_t config = GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT();
    config.pixels_per_unit = pixels_per_unit(state->font, ppem);
    config.samples_per_axis = samples_per_axis;
    config.padding_pixels = 2U;
    config.memory = GRAPE_MEMORY_PSRAM;
    state->last_ppem = ppem;

    return grape_text_rasterize_codepoints_a8(
        state->cache,
        state->font,
        state->codepoints,
        state->codepoint_count,
        &config,
        &state->raster
    );
}

static esp_err_t warm_raster(text_case_state_t *state,
                             float ppem,
                             uint8_t samples_per_axis)
{
    esp_err_t ret = rasterize(state, ppem, samples_per_axis);
    destroy_raster(state);
    return ret;
}

static void destroy_state(text_case_state_t *state)
{
    if (!state) {
        return;
    }
    if (state->surface) {
        grape_surface_destroy(state->surface);
    }
    destroy_raster(state);
    if (state->display_texture) {
        grape_texture_destroy(state->display_texture);
    }
    if (state->path) {
        grape_path_destroy(state->path);
    }
    if (state->cache) {
        grape_glyph_cache_destroy(state->cache);
    }
    if (state->font) {
        grape_font_destroy(state->font);
    }
    free(state->codepoints);
    free(state);
}

static esp_err_t create_surface_from_raster(grape_benchmark_runtime_t *runtime,
                                            text_case_state_t *state,
                                            float ppem,
                                            uint8_t samples_per_axis)
{
    esp_err_t ret = rasterize(state, ppem, samples_per_axis);
    if (ret != ESP_OK) {
        return ret;
    }

    state->display_texture = state->raster.texture;
    state->raster.texture = NULL;
    ret = grape_surface_create(runtime->grape, state->display_texture, &state->surface);
    if (ret == ESP_OK) {
        ret = grape_surface_set_tint(
            state->surface,
            (grape_color_t){ .r = 235, .g = 230, .b = 255, .a = 255 }
        );
    }
    if (ret == ESP_OK) {
        const grape_display_info_t *display = grape_get_display_info(runtime->grape);
        float width = (float)grape_texture_width(state->display_texture);
        float height = (float)grape_texture_height(state->display_texture);
        ret = grape_surface_set_position(
            state->surface,
            ((float)display->width - width) * 0.5f,
            ((float)display->height - height) * 0.5f
        );
    }
    return ret;
}

static esp_err_t prewarm_resize_sizes(text_case_state_t *state,
                                      uint8_t samples_per_axis)
{
    for (float ppem = 96.0f; ppem <= 160.0f; ppem += 4.0f) {
        esp_err_t ret = warm_raster(state, ppem, samples_per_axis);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    const text_case_config_t *config = bench_case->user_data;
    if (!config || !out_state) {
        return ESP_ERR_INVALID_ARG;
    }

    text_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = grape_benchmark_fixture_font_load(&state->font);
    if (ret == ESP_OK) {
        ret = make_codepoints(config->length, &state->codepoints, &state->codepoint_count);
    }
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    ret = grape_text_measure_codepoints(
        state->font,
        state->codepoints,
        state->codepoint_count,
        &state->measurement
    );
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    if (config->operation == TEXT_OP_BUILD_PATH) {
        ret = grape_path_create(&state->path);
        if (ret != ESP_OK) {
            destroy_state(state);
            return ret;
        }
    }

    if (config->operation >= TEXT_OP_RASTER_COLD) {
        grape_glyph_cache_config_t cache_config = GRAPE_GLYPH_CACHE_CONFIG_DEFAULT();
        cache_config.capacity_bytes = 16U * 1024U * 1024U;
        cache_config.memory = GRAPE_MEMORY_PSRAM;
        cache_config.raster_padding_pixels = 2U;
        cache_config.max_downscale_ratio = 2.0f;
        cache_config.scale_backend = GRAPE_GLYPH_CACHE_SCALE_CPU;
        ret = grape_glyph_cache_create(runtime->grape, &cache_config, &state->cache);
        if (ret != ESP_OK) {
            destroy_state(state);
            return ret;
        }
    }

    switch (config->operation) {
        case TEXT_OP_RASTER_WARM:
            ret = warm_raster(state, config->target_ppem, config->samples_per_axis);
            break;

        case TEXT_OP_RASTER_SCALED:
        case TEXT_OP_SCENE_RESIZE_SCALED:
            ret = warm_raster(state, config->source_ppem, config->samples_per_axis);
            break;

        case TEXT_OP_SCENE_RESIZE_EXACT:
            ret = prewarm_resize_sizes(state, config->samples_per_axis);
            break;

        default:
            break;
    }
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    if (config->operation == TEXT_OP_SCENE_MOVE) {
        ret = create_surface_from_raster(
            runtime, state, config->target_ppem, config->samples_per_axis
        );
    } else if (config->operation == TEXT_OP_SCENE_RESIZE_SCALED ||
               config->operation == TEXT_OP_SCENE_RESIZE_EXACT) {
        ret = create_surface_from_raster(
            runtime, state, 96.0f, config->samples_per_axis
        );
    }

    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    grape_benchmark_damage_clear(runtime->grape);
    *out_state = state;
    return ESP_OK;
}

static float resize_ppem(const text_case_config_t *config,
                         uint32_t sequence_iteration)
{
    if (config->operation == TEXT_OP_SCENE_RESIZE_EXACT) {
        return 96.0f + (float)(sequence_iteration % 17U) * 4.0f;
    }
    return config->target_ppem +
           config->target_step_ppem * (float)sequence_iteration;
}

static esp_err_t replace_surface_texture(grape_benchmark_runtime_t *runtime,
                                         text_case_state_t *state,
                                         float ppem,
                                         uint8_t samples_per_axis)
{
    esp_err_t ret = rasterize(state, ppem, samples_per_axis);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_texture_t *new_texture = state->raster.texture;
    state->raster.texture = NULL;
    ret = grape_surface_set_texture(state->surface, new_texture);
    if (ret != ESP_OK) {
        grape_texture_destroy(new_texture);
        return ret;
    }

    grape_texture_t *old_texture = state->display_texture;
    state->display_texture = new_texture;
    if (old_texture) {
        ret = grape_texture_destroy(old_texture);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    float width = (float)grape_texture_width(new_texture);
    float height = (float)grape_texture_height(new_texture);
    return grape_surface_set_position(
        state->surface,
        ((float)display->width - width) * 0.5f,
        ((float)display->height - height) * 0.5f
    );
}

static esp_err_t iteration(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void *opaque_state,
                           uint32_t sequence_iteration)
{
    const text_case_config_t *config = bench_case->user_data;
    text_case_state_t *state = opaque_state;

    switch (config->operation) {
        case TEXT_OP_MEASURE:
            return grape_text_measure_codepoints(
                state->font,
                state->codepoints,
                state->codepoint_count,
                &state->measurement
            );

        case TEXT_OP_BUILD_PATH: {
            esp_err_t ret = grape_path_clear(state->path);
            grape_text_path_info_t info = {0};
            return ret == ESP_OK
                ? grape_text_build_codepoints_path(
                    state->font,
                    state->codepoints,
                    state->codepoint_count,
                    state->path,
                    &info
                )
                : ret;
        }

        case TEXT_OP_RASTER_COLD:
        case TEXT_OP_RASTER_WARM:
            return rasterize(
                state,
                config->target_ppem,
                config->samples_per_axis
            );

        case TEXT_OP_RASTER_SCALED: {
            float ppem = resize_ppem(config, sequence_iteration);
            if (ppem >= config->source_ppem) {
                return ESP_ERR_INVALID_STATE;
            }
            return rasterize(state, ppem, config->samples_per_axis);
        }

        case TEXT_OP_SCENE_MOVE: {
            const grape_display_info_t *display = grape_get_display_info(runtime->grape);
            float time = grape_benchmark_fixed_time_s(runtime, sequence_iteration);
            float width = (float)grape_texture_width(state->display_texture);
            float height = (float)grape_texture_height(state->display_texture);
            float span_x = fmaxf((float)display->width - width, 1.0f);
            float span_y = fmaxf((float)display->height - height, 1.0f);
            float x = (sinf(time * 1.7f) * 0.5f + 0.5f) * span_x;
            float y = (cosf(time * 1.1f) * 0.5f + 0.5f) * span_y;
            return grape_surface_set_position(state->surface, x, y);
        }

        case TEXT_OP_SCENE_RESIZE_SCALED:
        case TEXT_OP_SCENE_RESIZE_EXACT: {
            float ppem = resize_ppem(config, sequence_iteration);
            if (config->operation == TEXT_OP_SCENE_RESIZE_SCALED &&
                ppem >= config->source_ppem) {
                return ESP_ERR_INVALID_STATE;
            }
            return replace_surface_texture(
                runtime,
                state,
                ppem,
                config->samples_per_axis
            );
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
    (void)sequence_iteration;
    const text_case_config_t *config = bench_case->user_data;
    text_case_state_t *state = opaque_state;

    if (config->operation == TEXT_OP_RASTER_COLD ||
        config->operation == TEXT_OP_RASTER_WARM ||
        config->operation == TEXT_OP_RASTER_SCALED) {
        destroy_raster(state);
    }
    if (config->operation == TEXT_OP_RASTER_COLD) {
        grape_glyph_cache_clear(state->cache);
    }
    grape_benchmark_damage_clear(runtime->grape);
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    const text_case_config_t *config = bench_case->user_data;
    text_case_state_t *state = opaque_state;
    if (capacity < 8U) {
        return 0U;
    }

    if (state->cache) {
        grape_glyph_cache_get_stats(state->cache, &state->stats);
    }
    out[0] = (grape_benchmark_metric_t){
        "glyphs", "", state->codepoint_count,
    };
    out[1] = (grape_benchmark_metric_t){
        "advance_units", "font_unit", state->measurement.advance_width,
    };
    out[2] = (grape_benchmark_metric_t){
        "cache_exact", "", (double)state->stats.exact_hits,
    };
    out[3] = (grape_benchmark_metric_t){
        "cache_scaled", "", (double)state->stats.scaled_hits,
    };
    out[4] = (grape_benchmark_metric_t){
        "cache_misses", "", (double)state->stats.misses,
    };
    out[5] = (grape_benchmark_metric_t){
        "cpu_scales", "", (double)state->stats.cpu_scales,
    };
    out[6] = (grape_benchmark_metric_t){
        "cache_bytes", "B", (double)state->stats.used_bytes,
    };
    out[7] = (grape_benchmark_metric_t){
        "last_ppem", "px/em", state->last_ppem,
    };
    (void)config;
    return 8U;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)runtime;
    (void)bench_case;
    destroy_state(opaque_state);
}

#define TEXT_CASE_CAPACITY 32

static grape_benchmark_case_t s_cases[TEXT_CASE_CAPACITY];
static text_case_config_t s_configs[TEXT_CASE_CAPACITY];
static char s_names[TEXT_CASE_CAPACITY][72];
static size_t s_case_count;
static bool s_initialized;

static const char *operation_name(text_operation_t operation)
{
    switch (operation) {
        case TEXT_OP_MEASURE: return "measure";
        case TEXT_OP_BUILD_PATH: return "build_path";
        case TEXT_OP_RASTER_COLD: return "raster_cold";
        case TEXT_OP_RASTER_WARM: return "raster_warm";
        case TEXT_OP_RASTER_SCALED: return "raster_scaled";
        case TEXT_OP_SCENE_MOVE: return "scene_move";
        case TEXT_OP_SCENE_RESIZE_SCALED: return "scene_resize_scaled";
        case TEXT_OP_SCENE_RESIZE_EXACT: return "scene_resize_exact";
        default: return "unknown";
    }
}

static void add_case(text_operation_t operation,
                     uint32_t length,
                     float target_ppem,
                     float source_ppem,
                     float step_ppem,
                     uint8_t samples_per_axis)
{
    if (s_case_count >= TEXT_CASE_CAPACITY) {
        return;
    }

    size_t index = s_case_count++;
    s_configs[index] = (text_case_config_t){
        .operation = operation,
        .length = length,
        .target_ppem = target_ppem,
        .source_ppem = source_ppem,
        .target_step_ppem = step_ppem,
        .samples_per_axis = samples_per_axis,
    };

    if (target_ppem > 0.0f) {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s_n%u_s%.0f", operation_name(operation),
            (unsigned)length, target_ppem
        );
    } else {
        snprintf(
            s_names[index], sizeof(s_names[index]),
            "%s_n%u", operation_name(operation), (unsigned)length
        );
    }

    bool scene = operation >= TEXT_OP_SCENE_MOVE;
    uint32_t warmup = scene ? 3U : 4U;
    uint32_t measured = 24U;
    if (operation == TEXT_OP_RASTER_SCALED ||
        operation == TEXT_OP_SCENE_RESIZE_SCALED) {
        warmup = 2U;
        measured = 12U;
    } else if (scene) {
        measured = 24U;
    } else if ((operation == TEXT_OP_RASTER_COLD ||
                operation == TEXT_OP_RASTER_WARM) &&
               (length >= 20U || target_ppem >= 128.0f)) {
        measured = 12U;
    }

    s_cases[index] = (grape_benchmark_case_t){
        .group = scene ? "text.scene" : "text",
        .name = s_names[index],
        .kind = scene ? GRAPE_BENCHMARK_KIND_SCENE
                      : GRAPE_BENCHMARK_KIND_PIPELINE,
        .flags = scene
            ? GRAPE_BENCHMARK_CASE_PRESENT |
              GRAPE_BENCHMARK_CASE_CAPTURE_REFRESH_WAIT
            : 0U,
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
            { "length", length },
            { "target_ppem", target_ppem },
            { "source_ppem", source_ppem },
            { "aa", samples_per_axis },
        },
    };
}

const grape_benchmark_case_t *grape_benchmark_text_cases(size_t *out_count)
{
    if (!s_initialized) {
        static const uint32_t lengths[] = { 5U, 20U, 100U };
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
            add_case(TEXT_OP_MEASURE, lengths[i], 0.0f, 0.0f, 0.0f, 4U);
            add_case(TEXT_OP_BUILD_PATH, lengths[i], 0.0f, 0.0f, 0.0f, 4U);
        }

        static const uint32_t raster_lengths[] = { 5U, 20U };
        static const float sizes[] = { 32.0f, 64.0f, 128.0f };
        for (size_t li = 0;
             li < sizeof(raster_lengths) / sizeof(raster_lengths[0]);
             ++li) {
            for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
                add_case(
                    TEXT_OP_RASTER_COLD,
                    raster_lengths[li], sizes[si], 0.0f, 0.0f, 4U
                );
                add_case(
                    TEXT_OP_RASTER_WARM,
                    raster_lengths[li], sizes[si], 0.0f, 0.0f, 4U
                );
            }
        }

        add_case(TEXT_OP_RASTER_SCALED, 5U, 64.0f, 128.0f, 4.0f, 4U);
        add_case(TEXT_OP_RASTER_SCALED, 20U, 64.0f, 128.0f, 4.0f, 4U);
        add_case(TEXT_OP_RASTER_SCALED, 5U, 96.0f, 192.0f, 6.0f, 4U);
        add_case(TEXT_OP_RASTER_SCALED, 20U, 96.0f, 192.0f, 6.0f, 4U);

        add_case(TEXT_OP_SCENE_MOVE, 5U, 128.0f, 0.0f, 0.0f, 4U);
        add_case(TEXT_OP_SCENE_RESIZE_SCALED, 5U, 96.0f, 192.0f, 4.0f, 4U);
        add_case(TEXT_OP_SCENE_RESIZE_EXACT, 5U, 96.0f, 0.0f, 0.0f, 4U);

        s_initialized = true;
    }

    if (out_count) {
        *out_count = s_case_count;
    }
    return s_cases;
}
