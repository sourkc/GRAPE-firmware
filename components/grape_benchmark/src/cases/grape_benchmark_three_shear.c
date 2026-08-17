#include "grape_benchmark_internal.h"

#include <math.h>
#include <stdlib.h>

#define PI_F 3.14159265358979323846f

typedef struct {
    uint32_t size;
    float angle_deg;
    bool ppa_composite;
    bool ppa_y;
} shear_case_config_t;

typedef struct {
    grape_texture_t *texture;
    grape_surface_t *surface;
    grape_rect_t rect;
    grape_feature_mode_t old_blend;
    grape_feature_mode_t old_rotate;
    grape_rotation_backend_t old_rotation;
} shear_case_state_t;

static void fill_mask(grape_texture_t *texture)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);
    float cx = (float)width * 0.5f;
    float cy = (float)height * 0.5f;
    float radius = (float)(width < height ? width : height) * 0.47f;

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float distance = sqrtf(dx * dx + dy * dy);
            row[x] = distance < radius
                ? (uint8_t)(128U + ((x + y) & 127U))
                : 0;
        }
    }
}

static void destroy_state(shear_case_state_t *state)
{
    if (!state) {
        return;
    }
    if (state->surface) {
        grape_surface_destroy(state->surface);
    }
    if (state->texture) {
        grape_texture_destroy(state->texture);
    }
    free(state);
}

static void restore_policy(grape_benchmark_runtime_t *runtime,
                           const shear_case_state_t *state)
{
    if (!runtime || !state) {
        return;
    }

    grape_feature_set_mode(
        runtime->grape,
        GRAPE_FEATURE_PPA_A8_BLEND,
        state->old_blend
    );
    grape_feature_set_mode(
        runtime->grape,
        GRAPE_FEATURE_PPA_A8_ROTATE,
        state->old_rotate
    );
    grape_set_rotation_backend(runtime->grape, state->old_rotation);
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    const shear_case_config_t *config = bench_case->user_data;
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    if (!config || !display) {
        return ESP_ERR_INVALID_ARG;
    }

    shear_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    state->old_rotation = grape_get_rotation_backend(runtime->grape);
    grape_feature_get_mode(
        runtime->grape,
        GRAPE_FEATURE_PPA_A8_BLEND,
        &state->old_blend
    );
    grape_feature_get_mode(
        runtime->grape,
        GRAPE_FEATURE_PPA_A8_ROTATE,
        &state->old_rotate
    );

    if (config->ppa_composite &&
        !grape_feature_is_available(runtime->grape, GRAPE_FEATURE_PPA_A8_BLEND)) {
        destroy_state(state);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (config->ppa_y &&
        !grape_feature_is_available(runtime->grape, GRAPE_FEATURE_PPA_A8_ROTATE)) {
        destroy_state(state);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = config->ppa_composite
        ? grape_feature_enable(runtime->grape, GRAPE_FEATURE_PPA_A8_BLEND)
        : grape_feature_disable(runtime->grape, GRAPE_FEATURE_PPA_A8_BLEND);
    if (ret == ESP_OK) {
        ret = config->ppa_y
            ? grape_feature_enable(runtime->grape, GRAPE_FEATURE_PPA_A8_ROTATE)
            : grape_feature_disable(runtime->grape, GRAPE_FEATURE_PPA_A8_ROTATE);
    }
    if (ret == ESP_OK) {
        ret = grape_set_rotation_backend(
            runtime->grape,
            GRAPE_ROTATION_BACKEND_THREE_SHEAR
        );
    }
    if (ret != ESP_OK) {
        restore_policy(runtime, state);
        destroy_state(state);
        return ret;
    }

    grape_texture_desc_t desc = {
        .width = config->size,
        .height = config->size,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_PSRAM,
    };
    ret = grape_texture_create(runtime->grape, &desc, &state->texture);
    if (ret != ESP_OK) {
        restore_policy(runtime, state);
        destroy_state(state);
        return ret;
    }

    fill_mask(state->texture);
    ret = grape_texture_invalidate(state->texture);
    if (ret == ESP_OK) {
        ret = grape_surface_create(runtime->grape, &GRAPE_SURFACE_DESC_TEXTURE(state->texture), &state->surface);
    }
    if (ret == ESP_OK) {
        ret = grape_surface_set_origin(
            state->surface,
            config->size * 0.5f,
            config->size * 0.5f
        );
    }
    if (ret == ESP_OK) {
        ret = grape_surface_set_position(
            state->surface,
            display->width * 0.5f,
            display->height * 0.5f
        );
    }
    if (ret == ESP_OK) {
        ret = grape_surface_set_rotation(
            state->surface,
            config->angle_deg * (PI_F / 180.0f)
        );
    }
    if (ret != ESP_OK) {
        restore_policy(runtime, state);
        destroy_state(state);
        return ret;
    }

    uint32_t span_width = config->size * 3U;
    uint32_t span_height = config->size * 3U;
    if (span_width > display->width) {
        span_width = display->width;
    }
    if (span_height > display->height) {
        span_height = display->height;
    }
    state->rect = (grape_rect_t) {
        .x = (int32_t)((display->width - span_width) / 2U),
        .y = (int32_t)((display->height - span_height) / 2U),
        .width = (int32_t)span_width,
        .height = (int32_t)span_height,
    };

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
    shear_case_state_t *state = opaque_state;
    return grape_benchmark_render_rects(runtime->grape, &state->rect, 1, false);
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)bench_case;
    shear_case_state_t *state = opaque_state;
    if (capacity < 2) {
        return 0;
    }

    out[0] = (grape_benchmark_metric_t) {
        "scratch_capacity",
        "B",
        (double)grape_benchmark_shear_scratch_bytes(runtime->grape),
    };
    out[1] = (grape_benchmark_metric_t) {
        "render_pixels",
        "px",
        (double)state->rect.width * state->rect.height,
    };
    return 2;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)bench_case;
    shear_case_state_t *state = opaque_state;
    if (!state) {
        return;
    }

    restore_policy(runtime, state);
    destroy_state(state);
}

#define SHEAR_CASE_CAPACITY 64

static grape_benchmark_case_t s_cases[SHEAR_CASE_CAPACITY];
static shear_case_config_t s_configs[SHEAR_CASE_CAPACITY];
static char s_names[SHEAR_CASE_CAPACITY][48];
static size_t s_case_count;
static bool s_initialized;

static void add_case(uint32_t size,
                     float angle_deg,
                     bool ppa_composite,
                     bool ppa_y)
{
    if (s_case_count >= SHEAR_CASE_CAPACITY) {
        return;
    }

    size_t index = s_case_count++;
    s_configs[index] = (shear_case_config_t) {
        .size = size,
        .angle_deg = angle_deg,
        .ppa_composite = ppa_composite,
        .ppa_y = ppa_y,
    };
    snprintf(
        s_names[index],
        sizeof(s_names[index]),
        "s%u_a%.0f_%s%s",
        (unsigned)size,
        (double)angle_deg,
        ppa_composite ? "ppa" : "cpu",
        ppa_y ? "_ppa_y" : ""
    );

    uint32_t measured_iterations = size >= 512U
        ? 8U
        : (size >= 256U ? 16U : 24U);
    s_cases[index] = (grape_benchmark_case_t) {
        .group = "three_shear",
        .name = s_names[index],
        .kind = GRAPE_BENCHMARK_KIND_MICRO,
        .warmup_iterations = 3,
        .measured_iterations = measured_iterations,
        .user_data = &s_configs[index],
        .setup = setup,
        .iteration = iteration,
        .collect_metrics = metrics,
        .teardown = teardown,
        .params = {
            { "size", size },
            { "angle_deg", angle_deg },
            { "ppa_composite", ppa_composite ? 1 : 0 },
            { "ppa_y", ppa_y ? 1 : 0 },
        },
    };
}

const grape_benchmark_case_t *grape_benchmark_three_shear_cases(size_t *out_count)
{
    if (!s_initialized) {
        static const uint32_t sizes[] = {32, 64, 128, 256, 512};
        static const float angles[] = {0, 5, 15, 30, 45, 60, 75, 85, 89, 90};

        for (size_t size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size) {
            for (size_t angle = 0; angle < sizeof(angles) / sizeof(angles[0]); ++angle) {
                add_case(sizes[size], angles[angle], true, false);
            }
        }

        add_case(128, 45, false, false);
        add_case(128, 85, false, false);
        add_case(128, 90, false, false);

        /* Skips on pre-v3 P4 where GRAY8 SRM rotation is unavailable. */
        add_case(128, 45, true, true);
        s_initialized = true;
    }

    if (out_count) {
        *out_count = s_case_count;
    }
    return s_cases;
}
