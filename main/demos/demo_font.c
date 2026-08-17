#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "demo_internal.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "grape_storage_sd.h"

static const char *TAG = "GRAPE_DEMO";
#define FONT_DEMO_PI 3.14159265358979323846f

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

    esp_err_t ret = grape_surface_create(grape, &GRAPE_SURFACE_DESC_TEXTURE(raster->texture), out_surface);
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

esp_err_t grape_demo_font_run(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = grape_storage_sd_mount();
    if (ret != ESP_OK) {
        return ret;
    }

    const char *font_path = GRAPE_STORAGE_SD_MOUNT_POINT "/fonts/Aileron-Regular.ttf";
    size_t font_size = 0U;
    ret = load_font_file(font_path, &s_font_demo_data, &font_size);
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
            float x = center_x + amplitude * sinf(move_phase * 2.0f * FONT_DEMO_PI);
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

void grape_demo_font_configure(grape_config_t *config)
{
    if (!config) {
        return;
    }
    config->background = (grape_color_t){ .r = 91, .g = 58, .b = 140, .a = 255 };
}
