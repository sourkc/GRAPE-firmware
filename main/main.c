#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "grape/grape.h"
#include "grape/grape_benchmark.h"
#include "grape/grape_telemetry_config.h"
#include "grape_storage_sd.h"

#include "app_config.h"
#include "generated_morph_wave_orb.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal/clk_tree_ll.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_rom_sys.h"

static const char *TAG = "GRAPE";

#define DEMO_SQUARE_COUNT 10
#define DEMO_TEXTURE_SIZE 128U
#define DEMO_PI 3.14159265358979323846f

typedef struct {
    grape_surface_t *surface;
    uint32_t seed;
    float base_size;
    float move_speed;
    float spin_speed;
    float phase;
} demo_square_t;

#if GRAPE_TELEMETRY_LEVEL >= 1
typedef struct {
    uint64_t frames;
    uint64_t dirty_tiles;
    uint64_t total_tiles;
    uint64_t planner_splits;
    uint64_t split_candidates;
    uint64_t final_rects;
    uint64_t final_pixels;
    uint64_t full_screen_frames;
    uint64_t fullscreen_pixels;
    uint64_t mark_us;
    uint64_t plan_us;
    uint64_t refresh_wait_us;
} demo_damage_stats_t;
#endif

static void fill_square_a8(grape_texture_t *texture)
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

static grape_color_t hue_to_rgb(float hue)
{
    float c = 1.0f;
    float x = 1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f);

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    if (hue < 60.0f) {
        r = c;
        g = x;
    } else if (hue < 120.0f) {
        r = x;
        g = c;
    } else if (hue < 180.0f) {
        g = c;
        b = x;
    } else if (hue < 240.0f) {
        g = x;
        b = c;
    } else if (hue < 300.0f) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }

    return (grape_color_t) {
        .r = (uint8_t)(r * 255.0f),
        .g = (uint8_t)(g * 255.0f),
        .b = (uint8_t)(b * 255.0f),
        .a = 255,
    };
}

static uint32_t hash_u32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

static float hash_signed(uint32_t seed, int32_t lattice)
{
    uint32_t value = hash_u32(seed ^ hash_u32((uint32_t)lattice));
    float unit = (float)(value & 0x00ffffffU) / 16777215.0f;
    return unit * 2.0f - 1.0f;
}

static float noise_fade(float value)
{
    return value * value * value * (value * (value * 6.0f - 15.0f) + 10.0f);
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
        seed = hash_u32(seed + 0x9e3779b9U);
    }

    return value / amplitude_sum;
}

static float noise_to_unit(float value)
{
    return value * 0.5f + 0.5f;
}


#if GRAPE_APP_RUN_SHADER_DEMO
#define SHADER_DEMO_RENDER_WIDTH 240U
#define SHADER_DEMO_RENDER_HEIGHT 135U
#define SHADER_DEMO_SCALE 3.0f
#define SHADER_DEMO_LOG_INTERVAL_FRAMES 1U

static grape_texture_t *s_shader_demo_texture;
static grape_surface_t *s_shader_demo_surface;
static generated_iq_raymarch_uniforms_t s_shader_demo_uniforms;

static esp_err_t render_shader_demo_frame(uint32_t frame, float time_seconds)
{
    s_shader_demo_uniforms.iTime = time_seconds;
    s_shader_demo_uniforms.iFrame = (int32_t)frame;
    return grape_shader_render_procedural_to_texture(
        s_shader_demo_texture,
        &generated_iq_raymarch_program,
        &s_shader_demo_uniforms
    );
}

static esp_err_t run_shader_demo(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_texture_desc_t desc = {
        .width = SHADER_DEMO_RENDER_WIDTH,
        .height = SHADER_DEMO_RENDER_HEIGHT,
        .format = GRAPE_PIXEL_FORMAT_RGB565,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    esp_err_t ret = grape_texture_create(grape, &desc, &s_shader_demo_texture);
    if (ret != ESP_OK) {
        return ret;
    }

    s_shader_demo_uniforms.iResolution = (grape_shader_vec3_t) {
        (float)SHADER_DEMO_RENDER_WIDTH,
        (float)SHADER_DEMO_RENDER_HEIGHT,
        1.0f,
    };
    s_shader_demo_uniforms.iTime = 0.0f;
    s_shader_demo_uniforms.iFrame = 0;
    s_shader_demo_uniforms.iMouse = (grape_shader_vec4_t) {0};

    ret = render_shader_demo_frame(0U, 0.0f);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_surface_create(grape, s_shader_demo_texture, &s_shader_demo_surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_transform_t transform = GRAPE_TRANSFORM_DEFAULT();
    transform.x = ((float)display->width - (float)SHADER_DEMO_RENDER_WIDTH * SHADER_DEMO_SCALE) * 0.5f;
    transform.y = ((float)display->height - (float)SHADER_DEMO_RENDER_HEIGHT * SHADER_DEMO_SCALE) * 0.5f;
    transform.scale_x = SHADER_DEMO_SCALE;
    transform.scale_y = SHADER_DEMO_SCALE;
    ret = grape_surface_set_transform(s_shader_demo_surface, &transform);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "IQ Shadertoy demo: %ux%u render -> %.0fx scale (%ux%u display area)",
             SHADER_DEMO_RENDER_WIDTH,
             SHADER_DEMO_RENDER_HEIGHT,
             (double)SHADER_DEMO_SCALE,
             (unsigned)(SHADER_DEMO_RENDER_WIDTH * (uint32_t)SHADER_DEMO_SCALE),
             (unsigned)(SHADER_DEMO_RENDER_HEIGHT * (uint32_t)SHADER_DEMO_SCALE));
    return grape_present(grape);
}

static void animate_shader_demo(grape_context_t *grape)
{
    int64_t start_us = esp_timer_get_time();
    uint32_t frame = 1U;

    while (1) {
        int64_t frame_start_us = esp_timer_get_time();
        float time_seconds = (float)(frame_start_us - start_us) / 1000000.0f;

        int64_t shader_start_us = esp_timer_get_time();
        ESP_ERROR_CHECK(render_shader_demo_frame(frame, time_seconds));
        int64_t shader_us = esp_timer_get_time() - shader_start_us;

        ESP_ERROR_CHECK(grape_present(grape));

        if (frame % SHADER_DEMO_LOG_INTERVAL_FRAMES == 0U) {
            int64_t frame_us = esp_timer_get_time() - frame_start_us;
            ESP_LOGI(TAG,
                     "IQ frame %" PRIu32 ": %.1f ms shader, %.1f ms total (%.3f FPS)",
                     frame,
                     (double)shader_us / 1000.0,
                     (double)frame_us / 1000.0,
                     frame_us > 0 ? 1000000.0 / (double)frame_us : 0.0);
        }

        frame++;
    }
}
#endif

#if GRAPE_APP_RUN_FONT_DEMO
#define FONT_DEMO_DIRECTORY GRAPE_STORAGE_SD_MOUNT_POINT "/fonts"
#define FONT_DEMO_PATH_CAPACITY 384U
#define FONT_DEMO_TEXT_COUNT 5U
#define FONT_DEMO_MOVE_PPEM 128.0f
#define FONT_DEMO_RESIZE_MIN_PPEM 96.0f
#define FONT_DEMO_RESIZE_MAX_PPEM 192.0f
#define FONT_DEMO_RESIZE_STEP_PPEM 2.0f
#define FONT_DEMO_PHASE_SECONDS 6.0f
#define FONT_DEMO_MOVE_CYCLE_SECONDS 3.5f
#define FONT_DEMO_RESIZE_CYCLE_SECONDS 4.0f
#define FONT_DEMO_AA_SAMPLES 4U

static const uint32_t s_font_demo_text[FONT_DEMO_TEXT_COUNT] = {
    'G', 'R', 'A', 'P', 'E',
};

static void *s_font_demo_data;
static grape_font_t *s_font_demo_font;
static grape_glyph_cache_t *s_font_demo_cache;
static grape_texture_t *s_font_demo_move_texture;
static grape_texture_t *s_font_demo_resize_texture;
static grape_surface_t *s_font_demo_move_surface;
static grape_surface_t *s_font_demo_resize_surface;

static bool has_ttf_extension(const char *name)
{
    const char *extension = strrchr(name, '.');
    return extension && strcasecmp(extension, ".ttf") == 0;
}

static esp_err_t choose_random_font_path(char *path, size_t capacity)
{
    if (!path || capacity == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *directory = opendir(FONT_DEMO_DIRECTORY);
    if (!directory) {
        ESP_LOGE(TAG, "Could not open %s", FONT_DEMO_DIRECTORY);
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t matches = 0;
    char chosen[FONT_DEMO_PATH_CAPACITY] = {0};
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!has_ttf_extension(entry->d_name)) {
            continue;
        }

        matches++;
        if (matches == 1U || esp_random() % matches == 0U) {
            int written = snprintf(
                chosen,
                sizeof(chosen),
                "%s/%s",
                FONT_DEMO_DIRECTORY,
                entry->d_name
            );
            if (written < 0 || (size_t)written >= sizeof(chosen)) {
                closedir(directory);
                return ESP_ERR_INVALID_SIZE;
            }
        }
    }

    closedir(directory);
    if (matches == 0U) {
        ESP_LOGE(TAG, "No .ttf files found in %s", FONT_DEMO_DIRECTORY);
        return ESP_ERR_NOT_FOUND;
    }

    if (strlen(chosen) + 1U > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(path, chosen);
    return ESP_OK;
}

static esp_err_t load_font_file(const char *path, void **out_data, size_t *out_size)
{
    if (!path || !out_data || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = NULL;
    *out_size = 0U;

    FILE *file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    long file_size = ftell(file);
    if (file_size <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t size = (size_t)file_size;
    void *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    if (!data) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    if (fread(data, 1U, size, file) != size) {
        heap_caps_free(data);
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);
    *out_data = data;
    *out_size = size;
    return ESP_OK;
}

static esp_err_t validate_demo_text(const grape_font_t *font)
{
    if (!font) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0U; i < FONT_DEMO_TEXT_COUNT; ++i) {
        uint16_t glyph_id = 0U;
        esp_err_t ret = grape_font_get_glyph_id(
            font,
            s_font_demo_text[i],
            &glyph_id
        );
        if (ret != ESP_OK) {
            return ret;
        }

        grape_font_glyph_info_t info = {0};
        ret = grape_font_get_glyph_info(font, glyph_id, &info);
        if (ret != ESP_OK) {
            return ret;
        }
        if (info.kind == GRAPE_FONT_GLYPH_COMPOSITE) {
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    return ESP_OK;
}

static grape_path_rasterize_config_t font_demo_raster_config(
    const grape_font_t *font,
    float ppem)
{
    grape_path_rasterize_config_t config =
        GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT();
    config.pixels_per_unit = ppem / (float)grape_font_units_per_em(font);
    config.samples_per_axis = FONT_DEMO_AA_SAMPLES;
    config.padding_pixels = 2U;
    config.memory = GRAPE_MEMORY_PSRAM;
    return config;
}

static esp_err_t rasterize_demo_text(float ppem, grape_text_raster_t *out_raster)
{
    if (!out_raster) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_path_rasterize_config_t config =
        font_demo_raster_config(s_font_demo_font, ppem);
    return grape_text_rasterize_codepoints_a8(
        s_font_demo_cache,
        s_font_demo_font,
        s_font_demo_text,
        FONT_DEMO_TEXT_COUNT,
        &config,
        out_raster
    );
}

static esp_err_t create_demo_text_surface(grape_context_t *grape,
                                          grape_text_raster_t *raster,
                                          float center_x,
                                          float center_y,
                                          grape_texture_t **out_texture,
                                          grape_surface_t **out_surface)
{
    if (!grape || !raster || !raster->texture || !out_texture || !out_surface) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = grape_surface_create(grape, raster->texture, out_surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_color_t glyph_color = {
        .r = 248,
        .g = 244,
        .b = 255,
        .a = 255,
    };
    ret = grape_surface_set_tint(*out_surface, glyph_color);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_transform_t transform = GRAPE_TRANSFORM_DEFAULT();
    transform.x = center_x;
    transform.y = center_y;
    transform.origin_x = (float)grape_texture_width(raster->texture) * 0.5f;
    transform.origin_y = (float)grape_texture_height(raster->texture) * 0.5f;
    ret = grape_surface_set_transform(*out_surface, &transform);
    if (ret != ESP_OK) {
        return ret;
    }

    *out_texture = raster->texture;
    return ESP_OK;
}

static esp_err_t replace_resize_raster(float ppem,
                                       float center_x,
                                       float center_y)
{
    grape_text_raster_t raster = {0};
    esp_err_t ret = rasterize_demo_text(ppem, &raster);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_texture_t *old_texture = s_font_demo_resize_texture;
    ret = grape_surface_set_texture(s_font_demo_resize_surface, raster.texture);
    if (ret != ESP_OK) {
        grape_texture_destroy(raster.texture);
        return ret;
    }
    s_font_demo_resize_texture = raster.texture;

    grape_transform_t transform = GRAPE_TRANSFORM_DEFAULT();
    transform.x = center_x;
    transform.y = center_y;
    transform.origin_x = (float)grape_texture_width(raster.texture) * 0.5f;
    transform.origin_y = (float)grape_texture_height(raster.texture) * 0.5f;
    ret = grape_surface_set_transform(s_font_demo_resize_surface, &transform);
    if (ret != ESP_OK) {
        if (old_texture) {
            grape_texture_destroy(old_texture);
        }
        return ret;
    }

    if (old_texture) {
        ret = grape_texture_destroy(old_texture);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static float quantize_resize_ppem(float ppem)
{
    float step = FONT_DEMO_RESIZE_STEP_PPEM;
    float quantized = roundf(ppem / step) * step;
    return fminf(
        fmaxf(quantized, FONT_DEMO_RESIZE_MIN_PPEM),
        FONT_DEMO_RESIZE_MAX_PPEM
    );
}

static esp_err_t run_font_demo(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = grape_storage_sd_mount();
    if (ret != ESP_OK) {
        return ret;
    }

    // char font_path[FONT_DEMO_PATH_CAPACITY] = {0};
    // ret = choose_random_font_path(font_path, sizeof(font_path));
    // if (ret != ESP_OK) {
    //     return ret;
    // }

    size_t font_size = 0U;
    ret = load_font_file("/sdcard/fonts/Aileron-Regular.ttf", &s_font_demo_data, &font_size);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_font_load_memory(s_font_demo_data, font_size, &s_font_demo_font);
    if (ret != ESP_OK) {
        return ret;
    }
    if (grape_font_units_per_em(s_font_demo_font) == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = validate_demo_text(s_font_demo_font);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Selected font cannot render simple GRAPE glyphs yet");
        return ret;
    }

    grape_glyph_cache_config_t cache_config = GRAPE_GLYPH_CACHE_CONFIG_DEFAULT();
    cache_config.capacity_bytes = 8U * 1024U * 1024U;
    cache_config.scale_backend = GRAPE_GLYPH_CACHE_SCALE_CPU;
    cache_config.max_downscale_ratio = 2.0f;
    ret = grape_glyph_cache_create(grape, &cache_config, &s_font_demo_cache);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_text_raster_t warm = {0};
    ret = rasterize_demo_text(FONT_DEMO_RESIZE_MAX_PPEM, &warm);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = grape_texture_destroy(warm.texture);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_text_raster_t move_raster = {0};
    ret = rasterize_demo_text(FONT_DEMO_MOVE_PPEM, &move_raster);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_text_raster_t resize_raster = {0};
    ret = rasterize_demo_text(FONT_DEMO_MOVE_PPEM, &resize_raster);
    if (ret != ESP_OK) {
        return ret;
    }

    float center_x = (float)display->width * 0.5f;
    float move_y = (float)display->height * 0.28f;
    float resize_y = (float)display->height * 0.72f;

    ret = create_demo_text_surface(
        grape,
        &move_raster,
        center_x,
        move_y,
        &s_font_demo_move_texture,
        &s_font_demo_move_surface
    );
    if (ret != ESP_OK) {
        return ret;
    }
    ret = create_demo_text_surface(
        grape,
        &resize_raster,
        center_x,
        resize_y,
        &s_font_demo_resize_texture,
        &s_font_demo_resize_surface
    );
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "TTF motion/resize demo: %s (%u bytes), units/em=%u, cmap=format %u",
             font_path,
             (unsigned)font_size,
             (unsigned)grape_font_units_per_em(s_font_demo_font),
             (unsigned)grape_font_cmap_format(s_font_demo_font));
    ESP_LOGI(TAG,
             "Alternating %.0fs phases: MOVE fixed %.0fpx/em, RESIZE %.0f..%.0fpx/em in %.0fpx steps",
             FONT_DEMO_PHASE_SECONDS,
             FONT_DEMO_MOVE_PPEM,
             FONT_DEMO_RESIZE_MIN_PPEM,
             FONT_DEMO_RESIZE_MAX_PPEM,
             FONT_DEMO_RESIZE_STEP_PPEM);

    ret = grape_present(grape);
    if (ret != ESP_OK) {
        return ret;
    }

    int64_t start_time = esp_timer_get_time();
    int64_t fps_start_time = start_time;
    uint32_t frame_count = 0U;
    uint32_t last_phase = UINT32_MAX;
    uint64_t update_us_total = 0U;
    uint64_t present_us_total = 0U;
    float current_resize_ppem = FONT_DEMO_MOVE_PPEM;
    grape_glyph_cache_stats_t stats_start = {0};
    grape_glyph_cache_get_stats(s_font_demo_cache, &stats_start);

    while (1) {
        int64_t now = esp_timer_get_time();
        float time = (float)(now - start_time) / 1000000.0f;
        uint32_t phase = (uint32_t)(time / FONT_DEMO_PHASE_SECONDS) & 1U;
        float phase_time = fmodf(time, FONT_DEMO_PHASE_SECONDS);

        if (phase != last_phase) {
            last_phase = phase;
            frame_count = 0U;
            update_us_total = 0U;
            present_us_total = 0U;
            fps_start_time = now;
            grape_glyph_cache_get_stats(s_font_demo_cache, &stats_start);

            if (phase == 0U) {
                ESP_LOGI(TAG, "--- MOVE phase: cached texture, position updates only ---");
            } else {
                ESP_LOGI(TAG, "--- RESIZE phase: fixed position, cached glyph resampling ---");
                ret = grape_surface_set_position(
                    s_font_demo_move_surface,
                    center_x,
                    move_y
                );
                if (ret != ESP_OK) {
                    return ret;
                }
            }
        }

        int64_t update_start = esp_timer_get_time();
        if (phase == 0U) {
            float move_phase = phase_time / FONT_DEMO_MOVE_CYCLE_SECONDS;
            float half_width =
                (float)grape_texture_width(s_font_demo_move_texture) * 0.5f;
            float amplitude = fmaxf(
                ((float)display->width - half_width * 2.0f - 32.0f) * 0.5f,
                0.0f
            );
            float x = center_x + amplitude * sinf(move_phase * 2.0f * DEMO_PI);
            ret = grape_surface_set_position(s_font_demo_move_surface, x, move_y);
            if (ret != ESP_OK) {
                return ret;
            }
        } else {
            float cycle = fmodf(phase_time, FONT_DEMO_RESIZE_CYCLE_SECONDS) /
                          FONT_DEMO_RESIZE_CYCLE_SECONDS;
            float triangle = 1.0f - fabsf(cycle * 2.0f - 1.0f);
            float requested_ppem = FONT_DEMO_RESIZE_MIN_PPEM +
                triangle * (FONT_DEMO_RESIZE_MAX_PPEM - FONT_DEMO_RESIZE_MIN_PPEM);
            requested_ppem = quantize_resize_ppem(requested_ppem);

            if (requested_ppem != current_resize_ppem) {
                ret = replace_resize_raster(requested_ppem, center_x, resize_y);
                if (ret != ESP_OK) {
                    return ret;
                }
                current_resize_ppem = requested_ppem;
            }
        }
        int64_t update_end = esp_timer_get_time();

        int64_t present_start = update_end;
        ret = grape_present(grape);
        if (ret != ESP_OK) {
            return ret;
        }
        int64_t present_end = esp_timer_get_time();

        update_us_total += (uint64_t)(update_end - update_start);
        present_us_total += (uint64_t)(present_end - present_start);
        frame_count++;

        int64_t elapsed_us = present_end - fps_start_time;
        if (elapsed_us >= 1000000 && frame_count > 0U) {
            float elapsed_seconds = (float)elapsed_us / 1000000.0f;
            float fps = (float)frame_count / elapsed_seconds;
            float update_ms = (float)update_us_total / (float)frame_count / 1000.0f;
            float present_ms = (float)present_us_total / (float)frame_count / 1000.0f;

            grape_glyph_cache_stats_t stats = {0};
            grape_glyph_cache_get_stats(s_font_demo_cache, &stats);
            printf(
                "%s FPS: %.2f | update %.3f ms | present %.3f ms | size %.0f px/em | cache +exact=%llu +scaled=%llu +miss=%llu +cpu=%llu | %.1f/%.1f MiB\n",
                phase == 0U ? "MOVE  " : "RESIZE",
                fps,
                update_ms,
                present_ms,
                current_resize_ppem,
                (unsigned long long)(stats.exact_hits - stats_start.exact_hits),
                (unsigned long long)(stats.scaled_hits - stats_start.scaled_hits),
                (unsigned long long)(stats.misses - stats_start.misses),
                (unsigned long long)(stats.cpu_scales - stats_start.cpu_scales),
                (double)stats.used_bytes / (1024.0 * 1024.0),
                (double)stats.capacity_bytes / (1024.0 * 1024.0)
            );

            frame_count = 0U;
            update_us_total = 0U;
            present_us_total = 0U;
            fps_start_time = present_end;
            stats_start = stats;
        }
    }
}
#endif

#if GRAPE_APP_RUN_VECTOR_DEMO
extern const uint8_t grape_demo_svg_start[] asm("_binary_grape_demo_svg_start");

static grape_svg_document_t *s_vector_demo_document;

static esp_err_t run_vector_demo(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    const float margin = 32.0f;
    grape_svg_document_config_t svg_config = GRAPE_SVG_DOCUMENT_CONFIG_DEFAULT();
    svg_config.x = margin;
    svg_config.y = margin;
    svg_config.width = fmaxf((float)display->width - margin * 2.0f, 1.0f);
    svg_config.height = fmaxf((float)display->height - margin * 2.0f, 1.0f);
    svg_config.samples_per_axis = 2;
    svg_config.padding_pixels = 2;

    esp_err_t ret = grape_svg_document_create(
        grape,
        (const char *)grape_demo_svg_start,
        &svg_config,
        &s_vector_demo_document
    );
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_present(grape);
    if (ret != ESP_OK) {
        grape_svg_document_destroy(s_vector_demo_document);
        s_vector_demo_document = NULL;
        return ret;
    }

    const grape_svg_view_box_t *view_box =
        grape_svg_document_view_box(s_vector_demo_document);
    ESP_LOGI(TAG,
             "SVG demo: %u layers, viewBox %.1f %.1f %.1f %.1f",
             (unsigned)grape_svg_document_layer_count(s_vector_demo_document),
             view_box ? view_box->min_x : 0.0f,
             view_box ? view_box->min_y : 0.0f,
             view_box ? view_box->width : 0.0f,
             view_box ? view_box->height : 0.0f);
    return ESP_OK;
}
#endif

void app_main(void)
{
#if EXPERIMENTAL_SET_CLOCK_400_MHZ
    REGI2C_CLOCK_ENABLE();

    clk_ll_cpll_set_config(400, 40);

    REGI2C_CLOCK_DISABLE();

    esp_rom_set_cpu_ticks_per_us(400);
#endif

    grape_context_t *grape = NULL;
    grape_config_t config = GRAPE_CONFIG_DEFAULT();
#if GRAPE_APP_RUN_FONT_DEMO
    config.background = (grape_color_t){ .r = 91, .g = 58, .b = 140, .a = 255 };
#endif

    ESP_ERROR_CHECK(grape_init(&config, &grape));

#if GRAPE_APP_RUN_BENCHMARK
    grape_benchmark_config_t benchmark_config = GRAPE_BENCHMARK_CONFIG_DEFAULT();
    benchmark_config.suite_mask = GRAPE_APP_BENCHMARK_SUITE_MASK;
    ESP_ERROR_CHECK(grape_benchmark_run(grape, &benchmark_config));

    grape_deinit(grape);
    return;
#endif

#if GRAPE_APP_RUN_SHADER_DEMO
    ESP_ERROR_CHECK(run_shader_demo(grape));
    animate_shader_demo(grape);
#endif

#if GRAPE_APP_RUN_FONT_DEMO
    ESP_ERROR_CHECK(run_font_demo(grape));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

#if GRAPE_APP_RUN_VECTOR_DEMO
    ESP_ERROR_CHECK(run_vector_demo(grape));
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif

    ESP_ERROR_CHECK(
        grape_debug_set_layer_enabled(
            grape,
            GRAPE_DEBUG_LAYER_DAMAGE_RECTS,
            false
        )
    );

    grape_texture_t *square_texture = NULL;
    grape_texture_desc_t texture_desc = {
        .width = DEMO_TEXTURE_SIZE,
        .height = DEMO_TEXTURE_SIZE,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_DEFAULT,
    };

    ESP_ERROR_CHECK(grape_texture_create(grape, &texture_desc, &square_texture));
    fill_square_a8(square_texture);
    ESP_ERROR_CHECK(grape_texture_invalidate(square_texture));

    const grape_display_info_t *display = grape_get_display_info(grape);
    ESP_LOGI(TAG, "display=%s %" PRIu32 "x%" PRIu32 " format=%d",
             display->name, display->width, display->height, display->format);

    demo_square_t squares[DEMO_SQUARE_COUNT] = {0};

    for (uint32_t i = 0; i < DEMO_SQUARE_COUNT; ++i) {
        demo_square_t *square = &squares[i];

        square->seed = hash_u32(0x47524150U + i * 0x9e3779b9U);
        square->base_size = 52.0f + (float)i * 8.0f;
        square->move_speed = 0.075f + (float)i * 0.0045f;
        square->spin_speed = (i & 1U ? -1.0f : 1.0f) * (0.14f + (float)i * 0.018f);
        square->phase = (float)i * 7.137f;

        ESP_ERROR_CHECK(grape_surface_create(grape, square_texture, &square->surface));

        float hue = 60.0f + (240.0f * (float)i / (float)(DEMO_SQUARE_COUNT - 1U));
        ESP_ERROR_CHECK(grape_surface_set_tint(square->surface, hue_to_rgb(hue)));
        ESP_ERROR_CHECK(grape_surface_set_z(square->surface, (int32_t)i));
    }

    int64_t start_time = esp_timer_get_time();
    int64_t fps_start_time = start_time;
#if GRAPE_TELEMETRY_LEVEL >= 1
    int64_t damage_stats_start_time = start_time;
    demo_damage_stats_t damage_stats = {0};
#endif
    uint32_t frame_count = 0;

    while (1) {
        int64_t now = esp_timer_get_time();
        float time = (float)(now - start_time) / 1000000.0f;

        for (uint32_t i = 0; i < DEMO_SQUARE_COUNT; ++i) {
            demo_square_t *square = &squares[i];

            float x_noise = fbm_noise_1d(
                square->seed ^ 0x243f6a88U,
                time * square->move_speed + square->phase
            );
            float y_noise = fbm_noise_1d(
                square->seed ^ 0x85a308d3U,
                time * (square->move_speed * 1.17f) + square->phase + 31.0f
            );
            float rotation_noise = fbm_noise_1d(
                square->seed ^ 0x13198a2eU,
                time * 0.095f + square->phase + 67.0f
            );
            float scale_noise = fbm_noise_1d(
                square->seed ^ 0x03707344U,
                time * 0.12f + square->phase + 103.0f
            );

            float scale_factor = 0.78f + noise_to_unit(scale_noise) * 0.48f;
            float scale = (square->base_size / (float)DEMO_TEXTURE_SIZE) * scale_factor;

            float max_half_extent = square->base_size * 0.92f;
            float x_span = fmaxf((float)display->width - max_half_extent * 2.0f, 1.0f);
            float y_span = fmaxf((float)display->height - max_half_extent * 2.0f, 1.0f);

            grape_transform_t transform = GRAPE_TRANSFORM_DEFAULT();
            transform.x = max_half_extent + noise_to_unit(x_noise) * x_span;
            transform.y = max_half_extent + noise_to_unit(y_noise) * y_span;
            transform.scale_x = scale;
            transform.scale_y = scale;
            transform.rotation = time * square->spin_speed + rotation_noise * DEMO_PI;
            transform.origin_x = (float)DEMO_TEXTURE_SIZE * 0.5f;
            transform.origin_y = (float)DEMO_TEXTURE_SIZE * 0.5f;

            ESP_ERROR_CHECK(grape_surface_set_transform(square->surface, &transform));
        }

        ESP_ERROR_CHECK(grape_present(grape));
        frame_count++;

#if GRAPE_TELEMETRY_LEVEL >= 1
        grape_debug_damage_stats_t frame_damage = {0};
        ESP_ERROR_CHECK(grape_debug_get_damage_stats(grape, &frame_damage));
        damage_stats.frames++;
        damage_stats.dirty_tiles += frame_damage.dirty_tiles;
        damage_stats.total_tiles = frame_damage.total_tiles;
        damage_stats.planner_splits += frame_damage.planner_splits;
        damage_stats.split_candidates += frame_damage.split_candidates;
        damage_stats.final_rects += frame_damage.final_rects;
        damage_stats.final_pixels += frame_damage.final_pixels;
        damage_stats.full_screen_frames += frame_damage.full_screen ? 1U : 0U;
        damage_stats.fullscreen_pixels = frame_damage.fullscreen_pixels;
        damage_stats.mark_us += frame_damage.mark_us;
        damage_stats.plan_us += frame_damage.plan_us;
        damage_stats.refresh_wait_us += frame_damage.refresh_wait_us;
#endif

        now = esp_timer_get_time();
        int64_t elapsed_us = now - fps_start_time;
        if (elapsed_us >= 1000000) {
            float elapsed_seconds = (float)elapsed_us / 1000000.0f;
            printf("FPS: %.2f\n", (float)frame_count / elapsed_seconds);
            frame_count = 0;
            fps_start_time = now;
        }

#if GRAPE_TELEMETRY_LEVEL >= 1
        int64_t damage_stats_elapsed_us = now - damage_stats_start_time;
        if (damage_stats_elapsed_us >= (int64_t)GRAPE_APP_DAMAGE_STATS_INTERVAL_MS * 1000 &&
            damage_stats.frames > 0) {
            double frames = (double)damage_stats.frames;
            double average_pixels = (double)damage_stats.final_pixels / frames;
            double coverage_percent = damage_stats.fullscreen_pixels > 0
                ? average_pixels * 100.0 / (double)damage_stats.fullscreen_pixels
                : 0.0;
            double tile_percent = damage_stats.total_tiles > 0
                ? ((double)damage_stats.dirty_tiles / frames) * 100.0 /
                  (double)damage_stats.total_tiles
                : 0.0;
            double full_screen_percent =
                (double)damage_stats.full_screen_frames * 100.0 / frames;
            double average_mark_ms =
                (double)damage_stats.mark_us / frames / 1000.0;
            double average_plan_ms =
                (double)damage_stats.plan_us / frames / 1000.0;
            double average_refresh_wait_ms =
                (double)damage_stats.refresh_wait_us / frames / 1000.0;

            printf(
                "DAMAGE: frames=%" PRIu64
                " tiles=%.1f/%" PRIu64 " (%.1f%%) splits=%.2f candidates=%.1f final_rects=%.2f "
                "pixels=%.0f/%" PRIu64 " (%.1f%%) fullscreen=%.1f%% "
                "mark=%.3f ms plan=%.3f ms refresh_wait=%.3f ms\n",
                damage_stats.frames,
                (double)damage_stats.dirty_tiles / frames,
                damage_stats.total_tiles,
                tile_percent,
                (double)damage_stats.planner_splits / frames,
                (double)damage_stats.split_candidates / frames,
                (double)damage_stats.final_rects / frames,
                average_pixels,
                damage_stats.fullscreen_pixels,
                coverage_percent,
                full_screen_percent,
                average_mark_ms,
                average_plan_ms,
                average_refresh_wait_ms
            );

            damage_stats = (demo_damage_stats_t){0};
            damage_stats_start_time = now;
        }
#endif

        // vTaskDelay(1);
    }
}
