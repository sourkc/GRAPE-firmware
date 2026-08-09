#include "grape_benchmark_internal.h"

#include <math.h>
#include <stdlib.h>

#define PI_F 3.14159265358979323846f

typedef enum {
    SCENE_MOTION = 0,
    SCENE_OVERDRAW,
    SCENE_FRAGMENT,
    SCENE_ROTATION,
    SCENE_LEGACY,
} scene_type_t;

typedef struct {
    scene_type_t type;
    uint32_t count;
    uint32_t size;
} scene_case_config_t;

typedef struct {
    grape_texture_t *texture;
    grape_surface_t **surfaces;
    uint32_t count;
    scene_type_t type;
    uint32_t size;
    grape_rotation_backend_t old_rotation;
} scene_case_state_t;

static float hash_signed(uint32_t seed, int32_t lattice)
{
    uint32_t value = grape_benchmark_hash_u32(
        seed ^ grape_benchmark_hash_u32((uint32_t)lattice)
    );
    float unit = (float)(value & 0x00ffffffU) / 16777215.0f;
    return unit * 2.0f - 1.0f;
}

static float noise_fade(float value)
{
    return value * value * value *
           (value * (value * 6.0f - 15.0f) + 10.0f);
}

static float value_noise_1d(uint32_t seed, float position)
{
    int32_t left = (int32_t)floorf(position);
    int32_t right = left + 1;
    float fraction = position - (float)left;
    float blend = noise_fade(fraction);
    float a = hash_signed(seed, left);
    float b = hash_signed(seed, right);
    return a + (b - a) * blend;
}

static float fbm_noise_1d(uint32_t seed, float position)
{
    float value = 0.0f;
    float amplitude = 1.0f;
    float amplitude_sum = 0.0f;

    for (uint32_t octave = 0; octave < 3; ++octave) {
        value += value_noise_1d(seed, position) * amplitude;
        amplitude_sum += amplitude;
        amplitude *= 0.5f;
        position *= 2.03f;
        seed = grape_benchmark_hash_u32(seed + 0x9e3779b9U);
    }
    return value / amplitude_sum;
}

static float noise_to_unit(float value)
{
    return value * 0.5f + 0.5f;
}

static void fill_texture(grape_texture_t *texture, bool circle)
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
            if (!circle) {
                row[x] = 255;
                continue;
            }

            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            row[x] = dx * dx + dy * dy < radius * radius ? 255 : 0;
        }
    }
}

static void destroy_state(scene_case_state_t *state)
{
    if (!state) {
        return;
    }

    if (state->surfaces) {
        for (uint32_t i = 0; i < state->count; ++i) {
            if (state->surfaces[i]) {
                grape_surface_destroy(state->surfaces[i]);
            }
        }
    }
    free(state->surfaces);

    if (state->texture) {
        grape_texture_destroy(state->texture);
    }
    free(state);
}

static void restore_policy(grape_benchmark_runtime_t *runtime,
                           const scene_case_state_t *state)
{
    if (!runtime || !state) {
        return;
    }
    grape_set_rotation_backend(runtime->grape, state->old_rotation);
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    const scene_case_config_t *config = bench_case->user_data;
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    if (!config || !display) {
        return ESP_ERR_INVALID_ARG;
    }

    scene_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    state->count = config->count;
    state->type = config->type;
    state->size = config->size;
    state->old_rotation = grape_get_rotation_backend(runtime->grape);
    state->surfaces = calloc(config->count, sizeof(*state->surfaces));
    if (!state->surfaces) {
        destroy_state(state);
        return ESP_ERR_NO_MEM;
    }

    grape_texture_desc_t desc = {
        .width = config->size,
        .height = config->size,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_PSRAM,
    };
    esp_err_t ret = grape_texture_create(runtime->grape, &desc, &state->texture);
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    fill_texture(state->texture, config->type == SCENE_ROTATION);
    ret = grape_texture_invalidate(state->texture);
    if (ret != ESP_OK) {
        destroy_state(state);
        return ret;
    }

    ret = grape_set_rotation_backend(
        runtime->grape,
        config->type == SCENE_ROTATION
            ? GRAPE_ROTATION_BACKEND_THREE_SHEAR
            : GRAPE_ROTATION_BACKEND_AUTO
    );
    if (ret != ESP_OK) {
        restore_policy(runtime, state);
        destroy_state(state);
        return ret;
    }

    for (uint32_t i = 0; i < config->count; ++i) {
        ret = grape_surface_create(
            runtime->grape,
            state->texture,
            &state->surfaces[i]
        );
        if (ret == ESP_OK) {
            ret = grape_surface_set_origin(
                state->surfaces[i],
                config->size * 0.5f,
                config->size * 0.5f
            );
        }
        if (ret == ESP_OK) {
            ret = grape_surface_set_z(state->surfaces[i], (int32_t)i);
        }
        if (config->type == SCENE_OVERDRAW && ret == ESP_OK) {
            ret = grape_surface_set_opacity(state->surfaces[i], 128);
        }
        if (ret != ESP_OK) {
            restore_policy(runtime, state);
            destroy_state(state);
            return ret;
        }
    }

    grape_benchmark_damage_clear(runtime->grape);
    *out_state = state;
    return ESP_OK;
}

static void update_legacy_transform(const scene_case_state_t *state,
                                    const grape_display_info_t *display,
                                    uint32_t index,
                                    float time,
                                    grape_transform_t *transform)
{
    uint32_t seed = grape_benchmark_hash_u32(
        0x47524150U + index * 0x9e3779b9U
    );
    float base_size = 52.0f + (float)index * 8.0f;
    float move_speed = 0.075f + (float)index * 0.0045f;
    float spin_speed = (index & 1U ? -1.0f : 1.0f) *
                       (0.14f + (float)index * 0.018f);
    float phase = (float)index * 7.137f;

    float x_noise = fbm_noise_1d(
        seed ^ 0x243f6a88U,
        time * move_speed + phase
    );
    float y_noise = fbm_noise_1d(
        seed ^ 0x85a308d3U,
        time * (move_speed * 1.17f) + phase + 31.0f
    );
    float rotation_noise = fbm_noise_1d(
        seed ^ 0x13198a2eU,
        time * 0.095f + phase + 67.0f
    );
    float scale_noise = fbm_noise_1d(
        seed ^ 0x03707344U,
        time * 0.12f + phase + 103.0f
    );

    float scale_factor = 0.78f + noise_to_unit(scale_noise) * 0.48f;
    float scale = (base_size / (float)state->size) * scale_factor;
    float max_half_extent = base_size * 0.92f;
    float x_span = fmaxf(
        (float)display->width - max_half_extent * 2.0f,
        1.0f
    );
    float y_span = fmaxf(
        (float)display->height - max_half_extent * 2.0f,
        1.0f
    );

    transform->x = max_half_extent + noise_to_unit(x_noise) * x_span;
    transform->y = max_half_extent + noise_to_unit(y_noise) * y_span;
    transform->rotation = time * spin_speed + rotation_noise * PI_F;
    transform->scale_x = scale;
    transform->scale_y = scale;
}

static esp_err_t iteration(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void *opaque_state,
                           uint32_t sequence_iteration)
{
    (void)bench_case;
    scene_case_state_t *state = opaque_state;
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    float time = grape_benchmark_fixed_time_s(runtime, sequence_iteration);

    for (uint32_t i = 0; i < state->count; ++i) {
        grape_transform_t transform = *grape_surface_transform(state->surfaces[i]);
        float phase = (float)i * 0.6180339f;

        switch (state->type) {
            case SCENE_LEGACY:
                update_legacy_transform(state, display, i, time, &transform);
                break;

            case SCENE_OVERDRAW:
                transform.x = display->width * 0.5f +
                              sinf(time * 1.3f + phase) * 24.0f;
                transform.y = display->height * 0.5f +
                              cosf(time * 1.1f + phase) * 24.0f;
                transform.rotation = time * 0.7f + phase * 0.2f;
                transform.scale_x = 1.0f;
                transform.scale_y = 1.0f;
                break;

            case SCENE_FRAGMENT:
                transform.x = 24.0f + fmodf(
                    (float)(i * 83U) + sinf(time * 2.0f + phase) * 18.0f,
                    (float)(display->width - 48U)
                );
                transform.y = 24.0f + fmodf(
                    (float)(i * 137U) + cosf(time * 1.7f + phase) * 18.0f,
                    (float)(display->height - 48U)
                );
                transform.rotation = time * 0.4f + phase;
                break;

            case SCENE_ROTATION: {
                uint32_t columns = 5;
                uint32_t row = i / columns;
                uint32_t column = i % columns;
                transform.x = (column + 1.0f) * display->width /
                              (columns + 1.0f);
                transform.y = (row + 1.0f) * 160.0f;
                while (transform.y > display->height - 80.0f) {
                    transform.y -= display->height * 0.5f;
                }
                transform.rotation =
                    (45.0f + 20.0f * sinf(time * 0.8f + phase)) *
                    (PI_F / 180.0f);
                transform.scale_x = 1.0f;
                transform.scale_y = 1.0f;
                break;
            }

            case SCENE_MOTION:
            default: {
                transform.x = display->width * 0.5f +
                              sinf(time * (0.7f + 0.01f * i) + phase) *
                              display->width * 0.42f;
                transform.y = display->height * 0.5f +
                              cosf(time * (0.61f + 0.013f * i) + phase) *
                              display->height * 0.42f;
                transform.rotation = time * (0.45f + 0.02f * i) + phase;
                float scale = 0.65f + 0.35f *
                              (0.5f + 0.5f * sinf(time * 0.5f + phase));
                transform.scale_x = scale;
                transform.scale_y = scale;
                break;
            }
        }

        esp_err_t ret = grape_surface_set_transform(
            state->surfaces[i],
            &transform
        );
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    (void)bench_case;
    scene_case_state_t *state = opaque_state;
    if (capacity < 2) {
        return 0;
    }

    out[0] = (grape_benchmark_metric_t) {
        "surfaces",
        "",
        state->count,
    };
    out[1] = (grape_benchmark_metric_t) {
        "texture_size",
        "px",
        state->size,
    };
    return 2;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)bench_case;
    scene_case_state_t *state = opaque_state;
    if (!state) {
        return;
    }

    restore_policy(runtime, state);
    destroy_state(state);
}

static scene_case_config_t s_configs[] = {
    {SCENE_MOTION, 10, 128},
    {SCENE_MOTION, 50, 128},
    {SCENE_OVERDRAW, 10, 128},
    {SCENE_OVERDRAW, 25, 128},
    {SCENE_OVERDRAW, 50, 128},
    {SCENE_FRAGMENT, 25, 32},
    {SCENE_FRAGMENT, 50, 32},
    {SCENE_FRAGMENT, 100, 32},
    {SCENE_ROTATION, 5, 128},
    {SCENE_ROTATION, 10, 128},
    {SCENE_ROTATION, 20, 128},
    {SCENE_LEGACY, 10, 128},
};

static grape_benchmark_case_t s_cases[12];
static char s_names[12][40];
static bool s_initialized;

static const char *scene_name(scene_type_t type)
{
    switch (type) {
        case SCENE_MOTION: return "motion";
        case SCENE_OVERDRAW: return "overdraw";
        case SCENE_FRAGMENT: return "fragment";
        case SCENE_ROTATION: return "rotation";
        case SCENE_LEGACY: return "legacy";
        default: return "unknown";
    }
}

const grape_benchmark_case_t *grape_benchmark_scene_cases(size_t *out_count)
{
    if (!s_initialized) {
        for (size_t i = 0; i < 12; ++i) {
            snprintf(
                s_names[i],
                sizeof(s_names[i]),
                "%s_n%u_s%u",
                scene_name(s_configs[i].type),
                (unsigned)s_configs[i].count,
                (unsigned)s_configs[i].size
            );
            s_cases[i] = (grape_benchmark_case_t) {
                .group = "scenes",
                .name = s_names[i],
                .kind = GRAPE_BENCHMARK_KIND_SCENE,
                .flags = GRAPE_BENCHMARK_CASE_PRESENT,
                .warmup_iterations = 6,
                .measured_iterations =
                    s_configs[i].type == SCENE_ROTATION ? 16 : 32,
                .user_data = &s_configs[i],
                .setup = setup,
                .iteration = iteration,
                .collect_metrics = metrics,
                .teardown = teardown,
                .params = {
                    { "type", s_configs[i].type },
                    { "surfaces", s_configs[i].count },
                    { "size", s_configs[i].size },
                },
            };
        }
        s_initialized = true;
    }

    if (out_count) {
        *out_count = 12;
    }
    return s_cases;
}
