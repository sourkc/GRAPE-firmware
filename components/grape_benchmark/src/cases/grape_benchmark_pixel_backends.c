#include "grape_benchmark_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    PIXEL_OP_PPA_FILL = 0,
    PIXEL_OP_CPU_FILL,
    PIXEL_OP_PPA_A8,
    PIXEL_OP_CPU_A8,
    PIXEL_OP_CPU_RGB565,
    PIXEL_OP_CPU_RGB888,
    PIXEL_OP_CPU_RGBA8888,
} pixel_operation_t;

typedef struct {
    pixel_operation_t operation;
    uint32_t size;
    uint8_t alpha;
    grape_memory_t memory;
    grape_texture_filter_t filter;
    grape_surface_aa_t aa;
    float rotation;
} pixel_case_config_t;

typedef struct {
    grape_texture_t *texture;
    grape_surface_t *surface;
    grape_rect_t rect;
    grape_feature_mode_t old_fill;
    grape_feature_mode_t old_blend;
    grape_rotation_backend_t old_rotation;
} pixel_case_state_t;

static void fill_texture(grape_texture_t *texture)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);
    grape_pixel_format_t format = grape_texture_format(texture);

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            if (format == GRAPE_PIXEL_FORMAT_A8) {
                row[x] = (uint8_t)(128U + ((x ^ y) & 127U));
            } else if (format == GRAPE_PIXEL_FORMAT_RGB565) {
                uint16_t r = (uint16_t)((x * 31U / (width ? width : 1U)) & 31U);
                uint16_t g = (uint16_t)((y * 63U / (height ? height : 1U)) & 63U);
                uint16_t b = (uint16_t)(
                    ((x + y) * 31U / ((width + height) ? width + height : 1U)) & 31U
                );
                ((uint16_t *)row)[x] = (uint16_t)((r << 11) | (g << 5) | b);
            } else if (format == GRAPE_PIXEL_FORMAT_RGBA8888) {
                uint8_t *pixel = row + (size_t)x * 4U;
                pixel[0] = (uint8_t)x;
                pixel[1] = (uint8_t)y;
                pixel[2] = (uint8_t)(x + y);
                pixel[3] = (uint8_t)(96U + ((x ^ y) & 159U));
            } else {
                row[x * 3U + 0U] = (uint8_t)x;
                row[x * 3U + 1U] = (uint8_t)y;
                row[x * 3U + 2U] = (uint8_t)(x + y);
            }
        }
    }
}

static grape_pixel_format_t operation_format(pixel_operation_t operation)
{
    switch (operation) {
        case PIXEL_OP_PPA_A8:
        case PIXEL_OP_CPU_A8:
            return GRAPE_PIXEL_FORMAT_A8;
        case PIXEL_OP_CPU_RGB565:
            return GRAPE_PIXEL_FORMAT_RGB565;
        case PIXEL_OP_CPU_RGB888:
            return GRAPE_PIXEL_FORMAT_RGB888;
        case PIXEL_OP_CPU_RGBA8888:
            return GRAPE_PIXEL_FORMAT_RGBA8888;
        default:
            return GRAPE_PIXEL_FORMAT_A8;
    }
}

static void destroy_state(pixel_case_state_t *state)
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
                           const pixel_case_state_t *state)
{
    if (!runtime || !state) {
        return;
    }

    grape_feature_set_mode(
        runtime->grape,
        GRAPE_FEATURE_PPA_FILL,
        state->old_fill
    );
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
    const pixel_case_config_t *config = bench_case->user_data;
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    if (!config || !display) {
        return ESP_ERR_INVALID_ARG;
    }

    pixel_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    state->old_rotation = grape_get_rotation_backend(runtime->grape);
    grape_feature_get_mode(
        runtime->grape,
        GRAPE_FEATURE_PPA_FILL,
        &state->old_fill
    );
    grape_feature_get_mode(
        runtime->grape,
        GRAPE_FEATURE_PPA_A8_BLEND,
        &state->old_blend
    );

    uint32_t width = config->size == 0 ? display->width : config->size;
    uint32_t height = config->size == 0 ? display->height : config->size;
    if (width > display->width) {
        width = display->width;
    }
    if (height > display->height) {
        height = display->height;
    }
    state->rect = (grape_rect_t) {
        .x = (int32_t)((display->width - width) / 2U),
        .y = (int32_t)((display->height - height) / 2U),
        .width = (int32_t)width,
        .height = (int32_t)height,
    };

    esp_err_t ret = ESP_OK;
    if (config->operation == PIXEL_OP_PPA_FILL) {
        if (!grape_feature_is_available(runtime->grape, GRAPE_FEATURE_PPA_FILL)) {
            destroy_state(state);
            return ESP_ERR_NOT_SUPPORTED;
        }
        ret = grape_feature_enable(runtime->grape, GRAPE_FEATURE_PPA_FILL);
    } else if (config->operation == PIXEL_OP_CPU_FILL) {
        ret = grape_feature_disable(runtime->grape, GRAPE_FEATURE_PPA_FILL);
    } else {
        grape_texture_desc_t desc = {
            .width = width,
            .height = height,
            .format = operation_format(config->operation),
            .memory = config->memory,
        };
        ret = grape_texture_create(runtime->grape, &desc, &state->texture);
        if (ret == ESP_ERR_NO_MEM && config->memory == GRAPE_MEMORY_INTERNAL) {
            destroy_state(state);
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (ret != ESP_OK) {
            destroy_state(state);
            return ret;
        }

        fill_texture(state->texture);
        ret = grape_texture_invalidate(state->texture);
        if (ret == ESP_OK) {
            grape_surface_desc_t surface_desc = GRAPE_SURFACE_DESC_TEXTURE(state->texture);
            surface_desc.texture_filter = config->filter;
            surface_desc.aa = config->aa;
            ret = grape_surface_create(runtime->grape, &surface_desc, &state->surface);
        }
        if (ret == ESP_OK && config->rotation == 0.0f) {
            ret = grape_surface_set_position(
                state->surface,
                (float)state->rect.x,
                (float)state->rect.y
            );
        } else if (ret == ESP_OK) {
            grape_transform_t transform = GRAPE_TRANSFORM_DEFAULT();
            transform.origin_x = (float)width * 0.5f;
            transform.origin_y = (float)height * 0.5f;
            transform.x = (float)display->width * 0.5f;
            transform.y = (float)display->height * 0.5f;
            transform.rotation = config->rotation;
            ret = grape_surface_set_transform(state->surface, &transform);

            const float abs_cos = fabsf(cosf(config->rotation));
            const float abs_sin = fabsf(sinf(config->rotation));
            uint32_t bound_width = (uint32_t)ceilf(abs_cos * (float)width + abs_sin * (float)height);
            uint32_t bound_height = (uint32_t)ceilf(abs_sin * (float)width + abs_cos * (float)height);
            if (config->aa == GRAPE_SURFACE_AA_COVERAGE_4X) {
                /* Surface bounds conservatively pad each AA edge by half a pixel. */
                bound_width += 2U;
                bound_height += 2U;
            }
            if (bound_width > display->width) bound_width = display->width;
            if (bound_height > display->height) bound_height = display->height;
            state->rect = (grape_rect_t){
                .x = (int32_t)((display->width - bound_width) / 2U),
                .y = (int32_t)((display->height - bound_height) / 2U),
                .width = (int32_t)bound_width,
                .height = (int32_t)bound_height,
            };
        }
        if (ret == ESP_OK) {
            ret = grape_surface_set_opacity(state->surface, config->alpha);
        }
        if (ret != ESP_OK) {
            destroy_state(state);
            return ret;
        }

        if (config->operation == PIXEL_OP_PPA_A8) {
            if (!grape_feature_is_available(
                    runtime->grape,
                    GRAPE_FEATURE_PPA_A8_BLEND)) {
                destroy_state(state);
                return ESP_ERR_NOT_SUPPORTED;
            }
            ret = grape_feature_enable(
                runtime->grape,
                GRAPE_FEATURE_PPA_A8_BLEND
            );
            if (ret == ESP_OK) {
                ret = grape_set_rotation_backend(
                    runtime->grape,
                    GRAPE_ROTATION_BACKEND_AUTO
                );
            }
        } else {
            ret = grape_feature_disable(
                runtime->grape,
                GRAPE_FEATURE_PPA_A8_BLEND
            );
            if (ret == ESP_OK) {
                ret = grape_set_rotation_backend(
                    runtime->grape,
                    GRAPE_ROTATION_BACKEND_AFFINE
                );
            }
        }
    }

    if (ret != ESP_OK) {
        restore_policy(runtime, state);
        destroy_state(state);
        return ret;
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
    pixel_case_state_t *state = opaque_state;
    return grape_benchmark_render_rects(runtime->grape, &state->rect, 1, false);
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    (void)bench_case;
    pixel_case_state_t *state = opaque_state;
    if (capacity < 3) {
        return 0;
    }

    double pixels = (double)state->rect.width * state->rect.height;
    out[0] = (grape_benchmark_metric_t) {
        "pixels",
        "px",
        pixels,
    };
    out[1] = (grape_benchmark_metric_t) {
        "megapixels",
        "Mpx",
        pixels / 1000000.0,
    };
    out[2] = (grape_benchmark_metric_t) {
        "bytes_source",
        "B",
        state->texture
            ? (double)grape_texture_stride(state->texture) *
              grape_texture_height(state->texture)
            : 0.0,
    };
    return 3;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)bench_case;
    pixel_case_state_t *state = opaque_state;
    if (!state) {
        return;
    }

    restore_policy(runtime, state);
    destroy_state(state);
}

#define PIXEL_CASE_CAPACITY 128

static grape_benchmark_case_t s_cases[PIXEL_CASE_CAPACITY];
static pixel_case_config_t s_configs[PIXEL_CASE_CAPACITY];
static char s_names[PIXEL_CASE_CAPACITY][64];
static size_t s_case_count;
static bool s_initialized;

static const char *operation_name(pixel_operation_t operation)
{
    switch (operation) {
        case PIXEL_OP_PPA_FILL: return "ppa_fill";
        case PIXEL_OP_CPU_FILL: return "cpu_fill";
        case PIXEL_OP_PPA_A8: return "ppa_a8";
        case PIXEL_OP_CPU_A8: return "cpu_a8";
        case PIXEL_OP_CPU_RGB565: return "cpu_rgb565";
        case PIXEL_OP_CPU_RGB888: return "cpu_rgb888";
        case PIXEL_OP_CPU_RGBA8888: return "cpu_rgba8888";
        default: return "unknown";
    }
}

static const char *memory_name(grape_memory_t memory)
{
    switch (memory) {
        case GRAPE_MEMORY_INTERNAL: return "internal";
        case GRAPE_MEMORY_PSRAM: return "psram";
        default: return "default";
    }
}

static void add_case(pixel_operation_t operation,
                     uint32_t size,
                     uint8_t alpha,
                     grape_memory_t memory)
{
    if (s_case_count >= PIXEL_CASE_CAPACITY) {
        return;
    }

    size_t index = s_case_count++;
    s_configs[index] = (pixel_case_config_t) {
        .operation = operation,
        .size = size,
        .alpha = alpha,
        .memory = memory,
        .filter = GRAPE_TEXTURE_FILTER_NEAREST,
        .aa = GRAPE_SURFACE_AA_NONE,
        .rotation = 0.0f,
    };

    snprintf(
        s_names[index],
        sizeof(s_names[index]),
        "%s_%s_a%u_%s",
        operation_name(operation),
        size ? "block" : "fullscreen",
        (unsigned)alpha,
        memory_name(memory)
    );
    if (size) {
        size_t used = strlen(s_names[index]);
        snprintf(
            s_names[index] + used,
            sizeof(s_names[index]) - used,
            "_%u",
            (unsigned)size
        );
    }

    uint32_t measured_iterations = size == 0
        ? 6U
        : (size >= 512U ? 12U : 0U);
    s_cases[index] = (grape_benchmark_case_t) {
        .group = "pixel_backend",
        .name = s_names[index],
        .kind = GRAPE_BENCHMARK_KIND_MICRO,
        .measured_iterations = measured_iterations,
        .user_data = &s_configs[index],
        .setup = setup,
        .iteration = iteration,
        .collect_metrics = metrics,
        .teardown = teardown,
        .params = {
            { "operation", operation },
            { "size", size },
            { "alpha", alpha },
            { "memory", memory },
            { "filter", GRAPE_TEXTURE_FILTER_NEAREST },
            { "aa", GRAPE_SURFACE_AA_NONE },
        },
    };
}

static void add_quality_case(pixel_operation_t operation,
                             grape_texture_filter_t filter,
                             grape_surface_aa_t aa)
{
    size_t index = s_case_count;
    add_case(operation, 256U, 255U, GRAPE_MEMORY_PSRAM);
    if (s_case_count == index) {
        return;
    }

    s_configs[index].filter = filter;
    s_configs[index].aa = aa;
    s_configs[index].rotation = 0.31f;
    snprintf(s_names[index], sizeof(s_names[index]),
             "%s_rot_quality_%s_%s",
             operation_name(operation),
             filter == GRAPE_TEXTURE_FILTER_LINEAR ? "linear" : "nearest",
             aa == GRAPE_SURFACE_AA_COVERAGE_4X ? "aa4" : "noaa");
    s_cases[index].params[4] = (grape_benchmark_param_t){"filter", filter};
    s_cases[index].params[5] = (grape_benchmark_param_t){"aa", aa};
}

const grape_benchmark_case_t *grape_benchmark_pixel_backend_cases(size_t *out_count)
{
    if (!s_initialized) {
        static const uint32_t sizes[] = {8, 16, 32, 64, 128, 256, 512, 0};
        static const pixel_operation_t surface_operations[] = {
            PIXEL_OP_PPA_A8,
            PIXEL_OP_CPU_A8,
            PIXEL_OP_CPU_RGB565,
            PIXEL_OP_CPU_RGB888,
        };
        static const uint8_t alphas[] = {255, 128};

        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            add_case(PIXEL_OP_PPA_FILL, sizes[i], 255, GRAPE_MEMORY_DEFAULT);
            add_case(PIXEL_OP_CPU_FILL, sizes[i], 255, GRAPE_MEMORY_DEFAULT);
        }

        for (size_t operation = 0;
             operation < sizeof(surface_operations) / sizeof(surface_operations[0]);
             ++operation) {
            for (size_t size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size) {
                for (size_t alpha = 0; alpha < sizeof(alphas) / sizeof(alphas[0]); ++alpha) {
                    add_case(
                        surface_operations[operation],
                        sizes[size],
                        alphas[alpha],
                        GRAPE_MEMORY_PSRAM
                    );
                }
            }
        }

        add_case(PIXEL_OP_PPA_A8, 256, 255, GRAPE_MEMORY_INTERNAL);
        add_case(PIXEL_OP_CPU_A8, 256, 255, GRAPE_MEMORY_INTERNAL);

        add_quality_case(PIXEL_OP_CPU_A8, GRAPE_TEXTURE_FILTER_NEAREST, GRAPE_SURFACE_AA_NONE);
        add_quality_case(PIXEL_OP_CPU_A8, GRAPE_TEXTURE_FILTER_NEAREST, GRAPE_SURFACE_AA_COVERAGE_4X);
        add_quality_case(PIXEL_OP_CPU_A8, GRAPE_TEXTURE_FILTER_LINEAR, GRAPE_SURFACE_AA_NONE);
        add_quality_case(PIXEL_OP_CPU_A8, GRAPE_TEXTURE_FILTER_LINEAR, GRAPE_SURFACE_AA_COVERAGE_4X);
        add_quality_case(PIXEL_OP_CPU_RGB565, GRAPE_TEXTURE_FILTER_NEAREST, GRAPE_SURFACE_AA_NONE);
        add_quality_case(PIXEL_OP_CPU_RGB565, GRAPE_TEXTURE_FILTER_NEAREST, GRAPE_SURFACE_AA_COVERAGE_4X);
        add_quality_case(PIXEL_OP_CPU_RGB565, GRAPE_TEXTURE_FILTER_LINEAR, GRAPE_SURFACE_AA_NONE);
        add_quality_case(PIXEL_OP_CPU_RGB565, GRAPE_TEXTURE_FILTER_LINEAR, GRAPE_SURFACE_AA_COVERAGE_4X);
        add_quality_case(PIXEL_OP_CPU_RGB888, GRAPE_TEXTURE_FILTER_NEAREST, GRAPE_SURFACE_AA_NONE);
        add_quality_case(PIXEL_OP_CPU_RGB888, GRAPE_TEXTURE_FILTER_NEAREST, GRAPE_SURFACE_AA_COVERAGE_4X);
        add_quality_case(PIXEL_OP_CPU_RGB888, GRAPE_TEXTURE_FILTER_LINEAR, GRAPE_SURFACE_AA_NONE);
        add_quality_case(PIXEL_OP_CPU_RGB888, GRAPE_TEXTURE_FILTER_LINEAR, GRAPE_SURFACE_AA_COVERAGE_4X);
        add_quality_case(PIXEL_OP_CPU_RGBA8888, GRAPE_TEXTURE_FILTER_NEAREST, GRAPE_SURFACE_AA_NONE);
        add_quality_case(PIXEL_OP_CPU_RGBA8888, GRAPE_TEXTURE_FILTER_NEAREST, GRAPE_SURFACE_AA_COVERAGE_4X);
        add_quality_case(PIXEL_OP_CPU_RGBA8888, GRAPE_TEXTURE_FILTER_LINEAR, GRAPE_SURFACE_AA_NONE);
        add_quality_case(PIXEL_OP_CPU_RGBA8888, GRAPE_TEXTURE_FILTER_LINEAR, GRAPE_SURFACE_AA_COVERAGE_4X);
        s_initialized = true;
    }

    if (out_count) {
        *out_count = s_case_count;
    }
    return s_cases;
}
