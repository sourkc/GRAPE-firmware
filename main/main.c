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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

#if GRAPE_APP_RUN_FONT_DEMO
#define FONT_DEMO_DIRECTORY GRAPE_STORAGE_SD_MOUNT_POINT "/fonts"
#define FONT_DEMO_PATH_CAPACITY 384U

static void *s_font_demo_data;
static grape_font_t *s_font_demo_font;
static grape_path_t *s_font_demo_path;
static grape_texture_t *s_font_demo_texture;
static grape_surface_t *s_font_demo_surface;

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

typedef struct {
    uint32_t codepoint;
    uint16_t glyph_id;
    grape_font_glyph_info_t info;
} font_demo_glyph_t;

static esp_err_t choose_demo_glyph(const grape_font_t *font,
                                   font_demo_glyph_t *out_glyph)
{
    if (!font || !out_glyph) {
        return ESP_ERR_INVALID_ARG;
    }

    static const uint32_t codepoints[] = {
        'A', 'B', 'C', 'a', 'b', '0', '1', '?', '@',
    };

    for (size_t i = 0; i < sizeof(codepoints) / sizeof(codepoints[0]); ++i) {
        uint16_t glyph_id = 0;
        esp_err_t ret = grape_font_get_glyph_id(font, codepoints[i], &glyph_id);
        if (ret == ESP_ERR_NOT_FOUND) {
            continue;
        }
        if (ret != ESP_OK) {
            return ret;
        }

        grape_font_glyph_info_t info = {0};
        ret = grape_font_get_glyph_info(font, glyph_id, &info);
        if (ret != ESP_OK) {
            return ret;
        }
        if (info.kind != GRAPE_FONT_GLYPH_SIMPLE || info.contour_count <= 0 ||
            info.x_max <= info.x_min || info.y_max <= info.y_min) {
            continue;
        }

        out_glyph->codepoint = codepoints[i];
        out_glyph->glyph_id = glyph_id;
        out_glyph->info = info;
        return ESP_OK;
    }

    return ESP_ERR_NOT_FOUND;
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

    char font_path[FONT_DEMO_PATH_CAPACITY] = {0};
    ret = choose_random_font_path(font_path, sizeof(font_path));
    if (ret != ESP_OK) {
        return ret;
    }

    size_t font_size = 0;
    ret = load_font_file(font_path, &s_font_demo_data, &font_size);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_font_load_memory(s_font_demo_data, font_size, &s_font_demo_font);
    if (ret != ESP_OK) {
        return ret;
    }

    font_demo_glyph_t demo_glyph = {0};
    ret = choose_demo_glyph(s_font_demo_font, &demo_glyph);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "No supported simple demo character in selected font");
        return ret;
    }

    ret = grape_path_create(&s_font_demo_path);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = grape_font_get_glyph_path(
        s_font_demo_font,
        demo_glyph.glyph_id,
        s_font_demo_path
    );
    if (ret != ESP_OK) {
        return ret;
    }

    grape_path_bounds_t bounds = {0};
    ret = grape_path_get_bounds(s_font_demo_path, &bounds);
    if (ret != ESP_OK) {
        return ret;
    }

    float path_width = bounds.max_x - bounds.min_x;
    float path_height = bounds.max_y - bounds.min_y;
    if (path_width <= 0.0f || path_height <= 0.0f) {
        return ESP_ERR_INVALID_SIZE;
    }

    float scale_x = (float)display->width * 0.70f / path_width;
    float scale_y = (float)display->height * 0.60f / path_height;
    float pixels_per_unit = fminf(scale_x, scale_y);

    grape_path_rasterize_config_t raster_config = GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT();
    raster_config.pixels_per_unit = pixels_per_unit;
    raster_config.samples_per_axis = 4;
    raster_config.padding_pixels = 2;
    raster_config.memory = GRAPE_MEMORY_PSRAM;

    grape_path_raster_t raster = {0};
    ret = grape_path_rasterize_a8(
        grape,
        s_font_demo_path,
        &raster_config,
        &raster
    );
    if (ret != ESP_OK) {
        return ret;
    }
    s_font_demo_texture = raster.texture;

    ret = grape_surface_create(grape, s_font_demo_texture, &s_font_demo_surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_color_t glyph_color = {
        .r = 248,
        .g = 244,
        .b = 255,
        .a = 255,
    };
    ret = grape_surface_set_tint(s_font_demo_surface, glyph_color);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_transform_t transform = GRAPE_TRANSFORM_DEFAULT();
    transform.x = (float)display->width * 0.5f;
    transform.y = (float)display->height * 0.5f;
    transform.origin_x = (float)grape_texture_width(s_font_demo_texture) * 0.5f;
    transform.origin_y = (float)grape_texture_height(s_font_demo_texture) * 0.5f;
    ret = grape_surface_set_transform(s_font_demo_surface, &transform);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_present(grape);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "TTF demo: %s (%u bytes), units/em=%u, glyphs=%u, cmap=format %u",
             font_path,
             (unsigned)font_size,
             (unsigned)grape_font_units_per_em(s_font_demo_font),
             (unsigned)grape_font_glyph_count(s_font_demo_font),
             (unsigned)grape_font_cmap_format(s_font_demo_font));
    ESP_LOGI(TAG,
             "TTF character: U+%04" PRIX32 " '%c' -> glyph=%u contours=%d bbox=[%d,%d]-[%d,%d] raster=%ux%u scale=%.5f",
             demo_glyph.codepoint,
             (char)demo_glyph.codepoint,
             (unsigned)demo_glyph.glyph_id,
             (int)demo_glyph.info.contour_count,
             (int)demo_glyph.info.x_min,
             (int)demo_glyph.info.y_min,
             (int)demo_glyph.info.x_max,
             (int)demo_glyph.info.y_max,
             (unsigned)grape_texture_width(s_font_demo_texture),
             (unsigned)grape_texture_height(s_font_demo_texture),
             pixels_per_unit);

    return ESP_OK;
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
