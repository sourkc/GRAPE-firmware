#include "grape_benchmark_internal.h"

#include <math.h>
#include <stdlib.h>

#include "esp_log.h"

#define BENCH_TEXTURE_SIZE 128U
#define BENCH_ORIGIN ((float)BENCH_TEXTURE_SIZE * 0.5f)
#define BENCH_PI 3.14159265358979323846f

typedef enum {
    BENCH_SUBJECT_NONE = 0,
    BENCH_SUBJECT_CIRCLE,
    BENCH_SUBJECT_SQUARE,
    BENCH_SUBJECT_BOTH,
} bench_subject_t;

typedef enum {
    BENCH_MOTION_NONE = 0,
    BENCH_MOTION_NOOP,
    BENCH_MOTION_FULL_REDRAW,
    BENCH_MOTION_CIRCLE,
    BENCH_MOTION_SQUARE,
    BENCH_MOTION_BOTH,
    BENCH_MOTION_CROSS,
    BENCH_MOTION_OVERLAP,
} bench_motion_t;

typedef struct {
    bench_subject_t subject;
    bench_motion_t motion;
    bool overlap;
    float rotation_deg;
    float scale;
    uint8_t square_opacity;
    grape_rotation_backend_t rotation_backend;
} shape_case_config_t;

typedef struct {
    grape_texture_t *circle_texture;
    grape_texture_t *square_texture;
    grape_surface_t *circle;
    grape_surface_t *square;
    float center_x;
    float center_y;
    float circle_base_x;
    float circle_base_y;
    float square_base_x;
    float square_base_y;
    grape_context_t *grape;
    grape_rotation_backend_t previous_rotation_backend;
} shape_case_state_t;

static void fill_square(grape_texture_t *texture)
{
    uint8_t *base = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = base + y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            row[x] = 255;
        }
    }
}

static void fill_circle(grape_texture_t *texture)
{
    uint8_t *base = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);

    float cx = (float)width * 0.5f;
    float cy = (float)height * 0.5f;
    float radius = (float)(width < height ? width : height) * 0.48f;

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = base + y * stride;

        for (uint32_t x = 0; x < width; ++x) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float distance = sqrtf(dx * dx + dy * dy);
            float edge = radius - distance;

            if (edge <= 0.0f) {
                row[x] = 0;
            } else if (edge >= 1.0f) {
                row[x] = 255;
            } else {
                row[x] = (uint8_t)(edge * 255.0f);
            }
        }
    }
}

static void destroy_shape_state(shape_case_state_t *state)
{
    if (!state) {
        return;
    }

    if (state->square) {
        grape_surface_destroy(state->square);
    }
    if (state->circle) {
        grape_surface_destroy(state->circle);
    }
    if (state->square_texture) {
        grape_texture_destroy(state->square_texture);
    }
    if (state->circle_texture) {
        grape_texture_destroy(state->circle_texture);
    }

    if (state->grape) {
        grape_set_rotation_backend(
            state->grape,
            state->previous_rotation_backend
        );
    }

    free(state);
}

static esp_err_t shape_setup(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void **out_state
)
{
    if (!runtime || !bench_case || !out_state || !bench_case->user_data) {
        return ESP_ERR_INVALID_ARG;
    }

    const shape_case_config_t *config = bench_case->user_data;
    shape_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    state->grape = runtime->grape;
    state->previous_rotation_backend =
        grape_get_rotation_backend(runtime->grape);

    esp_err_t ret = grape_set_rotation_backend(
        runtime->grape,
        config->rotation_backend
    );
    if (ret != ESP_OK) {
        destroy_shape_state(state);
        return ret;
    }

    grape_texture_desc_t texture_desc = {
        .width = BENCH_TEXTURE_SIZE,
        .height = BENCH_TEXTURE_SIZE,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_DEFAULT,
    };

    ret = grape_texture_create(
        runtime->grape,
        &texture_desc,
        &state->circle_texture
    );
    if (ret != ESP_OK) {
        destroy_shape_state(state);
        return ret;
    }

    ret = grape_texture_create(
        runtime->grape,
        &texture_desc,
        &state->square_texture
    );
    if (ret != ESP_OK) {
        destroy_shape_state(state);
        return ret;
    }

    fill_circle(state->circle_texture);
    fill_square(state->square_texture);

    ret = grape_surface_create(
        runtime->grape,
        state->circle_texture,
        &state->circle
    );
    if (ret != ESP_OK) {
        destroy_shape_state(state);
        return ret;
    }

    ret = grape_surface_create(
        runtime->grape,
        state->square_texture,
        &state->square
    );
    if (ret != ESP_OK) {
        destroy_shape_state(state);
        return ret;
    }

    const grape_display_info_t *display =
        grape_get_display_info(runtime->grape);
    if (!display) {
        destroy_shape_state(state);
        return ESP_ERR_INVALID_STATE;
    }

    state->center_x = (float)display->width * 0.5f;
    state->center_y = (float)display->height * 0.5f;

    if (config->overlap) {
        state->circle_base_x = state->center_x;
        state->circle_base_y = state->center_y;
        state->square_base_x = state->center_x;
        state->square_base_y = state->center_y;
    } else {
        state->circle_base_x = state->center_x - 96.0f;
        state->circle_base_y = state->center_y;
        state->square_base_x = state->center_x + 96.0f;
        state->square_base_y = state->center_y;
    }

    grape_color_t circle_tint = { .r = 0, .g = 220, .b = 255, .a = 255 };
    grape_color_t square_tint = { .r = 255, .g = 48, .b = 64, .a = 255 };

    ret = grape_surface_set_origin(state->circle, BENCH_ORIGIN, BENCH_ORIGIN);
    if (ret == ESP_OK) ret = grape_surface_set_origin(state->square, BENCH_ORIGIN, BENCH_ORIGIN);
    if (ret == ESP_OK) ret = grape_surface_set_position(state->circle, state->circle_base_x, state->circle_base_y);
    if (ret == ESP_OK) ret = grape_surface_set_position(state->square, state->square_base_x, state->square_base_y);
    if (ret == ESP_OK) ret = grape_surface_set_tint(state->circle, circle_tint);
    if (ret == ESP_OK) ret = grape_surface_set_tint(state->square, square_tint);
    if (ret == ESP_OK) ret = grape_surface_set_z(state->circle, 0);
    if (ret == ESP_OK) ret = grape_surface_set_z(state->square, 1);
    if (ret == ESP_OK) ret = grape_surface_set_opacity(state->square, config->square_opacity);
    if (ret == ESP_OK) ret = grape_surface_set_scale(state->square, config->scale, config->scale);
    if (ret == ESP_OK) ret = grape_surface_set_rotation(
        state->square,
        config->rotation_deg * (BENCH_PI / 180.0f)
    );

    if (ret != ESP_OK) {
        destroy_shape_state(state);
        return ret;
    }

    switch (config->subject) {
        case BENCH_SUBJECT_NONE:
            ret = grape_surface_set_visible(state->circle, false);
            if (ret == ESP_OK) ret = grape_surface_set_visible(state->square, false);
            break;

        case BENCH_SUBJECT_CIRCLE:
            ret = grape_surface_set_visible(state->circle, true);
            if (ret == ESP_OK) ret = grape_surface_set_visible(state->square, false);
            break;

        case BENCH_SUBJECT_SQUARE:
            ret = grape_surface_set_visible(state->circle, false);
            if (ret == ESP_OK) ret = grape_surface_set_visible(state->square, true);
            break;

        case BENCH_SUBJECT_BOTH:
            ret = grape_surface_set_visible(state->circle, true);
            if (ret == ESP_OK) ret = grape_surface_set_visible(state->square, true);
            break;
    }

    if (ret != ESP_OK) {
        destroy_shape_state(state);
        return ret;
    }

    ret = grape_texture_invalidate(state->circle_texture);
    if (ret == ESP_OK) {
        ret = grape_texture_invalidate(state->square_texture);
    }
    if (ret != ESP_OK) {
        destroy_shape_state(state);
        return ret;
    }

    *out_state = state;
    return ESP_OK;
}

static esp_err_t invalidate_subject(
    const shape_case_config_t *config,
    shape_case_state_t *state
)
{
    switch (config->subject) {
        case BENCH_SUBJECT_NONE:
            return ESP_OK;

        case BENCH_SUBJECT_CIRCLE:
            return grape_texture_invalidate(state->circle_texture);

        case BENCH_SUBJECT_SQUARE:
            return grape_texture_invalidate(state->square_texture);

        case BENCH_SUBJECT_BOTH: {
            esp_err_t ret = grape_texture_invalidate(state->circle_texture);
            if (ret != ESP_OK) {
                return ret;
            }
            return grape_texture_invalidate(state->square_texture);
        }
    }

    return ESP_ERR_INVALID_ARG;
}

static esp_err_t shape_step(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *opaque_state,
    uint32_t sequence_frame
)
{
    (void)runtime;

    if (!bench_case || !bench_case->user_data || !opaque_state) {
        return ESP_ERR_INVALID_ARG;
    }

    const shape_case_config_t *config = bench_case->user_data;
    shape_case_state_t *state = opaque_state;

    float phase =
        ((float)(sequence_frame % GRAPE_BENCHMARK_MOVEMENT_CYCLE_FRAMES) /
         (float)GRAPE_BENCHMARK_MOVEMENT_CYCLE_FRAMES) *
        (2.0f * BENCH_PI);

    float sin_phase = sinf(phase);
    float cos_phase = cosf(phase);

    const float move_x = 120.0f;
    const float move_y = 72.0f;

    switch (config->motion) {
        case BENCH_MOTION_NONE:
            return invalidate_subject(config, state);

        case BENCH_MOTION_NOOP:
            return ESP_OK;

        case BENCH_MOTION_FULL_REDRAW:
            return grape_invalidate_all(runtime->grape);

        case BENCH_MOTION_CIRCLE:
            return grape_surface_set_position(
                state->circle,
                state->circle_base_x + move_x * sin_phase,
                state->circle_base_y + move_y * cos_phase
            );

        case BENCH_MOTION_SQUARE:
            return grape_surface_set_position(
                state->square,
                state->square_base_x + move_x * sin_phase,
                state->square_base_y + move_y * cos_phase
            );

        case BENCH_MOTION_BOTH: {
            esp_err_t ret = grape_surface_set_position(
                state->circle,
                state->circle_base_x + move_x * sin_phase,
                state->circle_base_y + move_y * cos_phase
            );
            if (ret != ESP_OK) {
                return ret;
            }

            return grape_surface_set_position(
                state->square,
                state->square_base_x + move_x * cos_phase,
                state->square_base_y - move_y * sin_phase
            );
        }

        case BENCH_MOTION_CROSS: {
            float distance = 110.0f * cos_phase;

            esp_err_t ret = grape_surface_set_position(
                state->circle,
                state->center_x - distance,
                state->center_y
            );
            if (ret != ESP_OK) {
                return ret;
            }

            return grape_surface_set_position(
                state->square,
                state->center_x + distance,
                state->center_y
            );
        }

        case BENCH_MOTION_OVERLAP: {
            float x = state->center_x + move_x * sin_phase;
            float y = state->center_y + move_y * cos_phase;

            esp_err_t ret = grape_surface_set_position(state->circle, x, y);
            if (ret != ESP_OK) {
                return ret;
            }

            return grape_surface_set_position(state->square, x + 24.0f, y - 24.0f);
        }
    }

    return ESP_ERR_INVALID_ARG;
}

static void shape_teardown(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state
)
{
    (void)runtime;
    (void)bench_case;
    destroy_shape_state(state);
}

#define SHAPE_CASE(group_value, name_value, subject_value, motion_value, overlap_value, rotation_value, scale_value, opacity_value, param_name, param_value) \
    { \
        .group = group_value, \
        .name = name_value, \
        .user_data = &(const shape_case_config_t){ \
            .subject = subject_value, \
            .motion = motion_value, \
            .overlap = overlap_value, \
            .rotation_deg = rotation_value, \
            .scale = scale_value, \
            .square_opacity = opacity_value, \
        }, \
        .setup = shape_setup, \
        .step = shape_step, \
        .teardown = shape_teardown, \
        .params = { { .name = param_name, .value = param_value } }, \
    }

#define SHAPE_CASE2(group_value, name_value, subject_value, motion_value, overlap_value, rotation_value, scale_value, opacity_value, p0_name, p0_value, p1_name, p1_value) \
    { \
        .group = group_value, \
        .name = name_value, \
        .user_data = &(const shape_case_config_t){ \
            .subject = subject_value, \
            .motion = motion_value, \
            .overlap = overlap_value, \
            .rotation_deg = rotation_value, \
            .scale = scale_value, \
            .square_opacity = opacity_value, \
        }, \
        .setup = shape_setup, \
        .step = shape_step, \
        .teardown = shape_teardown, \
        .params = { \
            { .name = p0_name, .value = p0_value }, \
            { .name = p1_name, .value = p1_value }, \
        }, \
    }

#define SHAPE_ROTATION_CASE(group_value, name_value, rotation_value, backend_value) \
    { \
        .group = group_value, \
        .name = name_value, \
        .user_data = &(const shape_case_config_t){ \
            .subject = BENCH_SUBJECT_SQUARE, \
            .motion = BENCH_MOTION_NONE, \
            .overlap = false, \
            .rotation_deg = rotation_value, \
            .scale = 1.0f, \
            .square_opacity = 255, \
            .rotation_backend = backend_value, \
        }, \
        .setup = shape_setup, \
        .step = shape_step, \
        .teardown = shape_teardown, \
        .params = { { .name = "angle_deg", .value = rotation_value } }, \
    }

static const grape_benchmark_case_t s_cases[] = {
#if GRAPE_BENCHMARK_SUITE_BASIC
    SHAPE_CASE("baseline", "empty_present", BENCH_SUBJECT_NONE, BENCH_MOTION_NOOP, false, 0.0f, 1.0f, 255, NULL, 0.0),
    SHAPE_CASE("baseline", "fullscreen_background", BENCH_SUBJECT_NONE, BENCH_MOTION_FULL_REDRAW, false, 0.0f, 1.0f, 255, NULL, 0.0),
    SHAPE_CASE("basic", "circle_static_redraw", BENCH_SUBJECT_CIRCLE, BENCH_MOTION_NONE, false, 0.0f, 1.0f, 255, NULL, 0.0),
    SHAPE_CASE("basic", "square_static_redraw", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 0.0f, 1.0f, 255, NULL, 0.0),
#endif

#if GRAPE_BENCHMARK_SUITE_MOVEMENT
    SHAPE_CASE("movement", "circle_move", BENCH_SUBJECT_CIRCLE, BENCH_MOTION_CIRCLE, false, 0.0f, 1.0f, 255, NULL, 0.0),
    SHAPE_CASE("movement", "square_move", BENCH_SUBJECT_SQUARE, BENCH_MOTION_SQUARE, false, 0.0f, 1.0f, 255, NULL, 0.0),
    SHAPE_CASE("movement", "both_move", BENCH_SUBJECT_BOTH, BENCH_MOTION_BOTH, false, 0.0f, 1.0f, 255, NULL, 0.0),
    SHAPE_CASE("movement", "both_cross", BENCH_SUBJECT_BOTH, BENCH_MOTION_CROSS, false, 0.0f, 1.0f, 255, NULL, 0.0),
#endif

#if GRAPE_BENCHMARK_SUITE_OVERLAP
    SHAPE_CASE("overlap", "both_overlap_static", BENCH_SUBJECT_BOTH, BENCH_MOTION_NONE, true, 0.0f, 1.0f, 255, NULL, 0.0),
    SHAPE_CASE("overlap", "both_overlap_move", BENCH_SUBJECT_BOTH, BENCH_MOTION_OVERLAP, true, 0.0f, 1.0f, 255, NULL, 0.0),
#endif

#if GRAPE_BENCHMARK_SUITE_OPACITY
    SHAPE_CASE("opacity", "square_opacity_255", BENCH_SUBJECT_BOTH, BENCH_MOTION_NONE, true, 0.0f, 1.0f, 255, "opacity", 255.0),
    SHAPE_CASE("opacity", "square_opacity_192", BENCH_SUBJECT_BOTH, BENCH_MOTION_NONE, true, 0.0f, 1.0f, 192, "opacity", 192.0),
    SHAPE_CASE("opacity", "square_opacity_128", BENCH_SUBJECT_BOTH, BENCH_MOTION_NONE, true, 0.0f, 1.0f, 128, "opacity", 128.0),
    SHAPE_CASE("opacity", "square_opacity_064", BENCH_SUBJECT_BOTH, BENCH_MOTION_NONE, true, 0.0f, 1.0f, 64, "opacity", 64.0),
#endif

#if GRAPE_BENCHMARK_SUITE_SCALE
    SHAPE_CASE("scale", "square_scale_050", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 0.0f, 0.50f, 255, "scale", 0.50),
    SHAPE_CASE("scale", "square_scale_075", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 0.0f, 0.75f, 255, "scale", 0.75),
    SHAPE_CASE("scale", "square_scale_100", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 0.0f, 1.00f, 255, "scale", 1.00),
    SHAPE_CASE("scale", "square_scale_125", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 0.0f, 1.25f, 255, "scale", 1.25),
    SHAPE_CASE("scale", "square_scale_150", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 0.0f, 1.50f, 255, "scale", 1.50),
    SHAPE_CASE("scale", "square_scale_200", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 0.0f, 2.00f, 255, "scale", 2.00),
#endif

#if GRAPE_BENCHMARK_SUITE_ROTATION
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_00", 0.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_05", 5.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_10", 10.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_15", 15.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_20", 20.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_25", 25.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_30", 30.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_35", 35.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_40", 40.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_45", 45.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_50", 50.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_55", 55.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_60", 60.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_65", 65.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_70", 70.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_75", 75.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_80", 80.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_85", 85.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_affine", "square_rotation_90", 90.0f, GRAPE_ROTATION_BACKEND_AFFINE),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_00", 0.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_05", 5.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_10", 10.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_15", 15.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_20", 20.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_25", 25.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_30", 30.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_35", 35.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_40", 40.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_45", 45.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_50", 50.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_55", 55.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_60", 60.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_65", 65.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_70", 70.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_75", 75.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_80", 80.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_85", 85.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
    SHAPE_ROTATION_CASE("rotation_shear", "square_rotation_90", 90.0f, GRAPE_ROTATION_BACKEND_THREE_SHEAR),
#endif

#if GRAPE_BENCHMARK_SUITE_ROTATION_SCALE
    SHAPE_CASE2("rotation_scale", "square_rot45_scale050", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 45.0f, 0.50f, 255, "angle_deg", 45.0, "scale", 0.50),
    SHAPE_CASE2("rotation_scale", "square_rot45_scale100", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 45.0f, 1.00f, 255, "angle_deg", 45.0, "scale", 1.00),
    SHAPE_CASE2("rotation_scale", "square_rot45_scale150", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 45.0f, 1.50f, 255, "angle_deg", 45.0, "scale", 1.50),
    SHAPE_CASE2("rotation_scale", "square_rot45_scale200", BENCH_SUBJECT_SQUARE, BENCH_MOTION_NONE, false, 45.0f, 2.00f, 255, "angle_deg", 45.0, "scale", 2.00),
#endif
};

const grape_benchmark_case_t *grape_benchmark_shape_cases(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_cases) / sizeof(s_cases[0]);
    }

    return s_cases;
}
