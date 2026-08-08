#include <inttypes.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "grape_internal.h"

static const char *TAG = "grape";

size_t grape_bytes_per_pixel(grape_pixel_format_t format)
{
    switch (format) {
        case GRAPE_PIXEL_FORMAT_RGB565:
            return 2;
        case GRAPE_PIXEL_FORMAT_RGB888:
            return 3;
        case GRAPE_PIXEL_FORMAT_A8:
            return 1;
        default:
            return 0;
    }
}

esp_err_t grape_init(const grape_config_t *config, grape_context_t **out_context)
{
    if (!out_context) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_config_t resolved = GRAPE_CONFIG_DEFAULT();
    if (config) {
        resolved = *config;
    }

    grape_context_t *context = calloc(1, sizeof(*context));
    if (!context) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;
    if (resolved.display_driver) {
        ret = grape_display_open(resolved.display_driver, &context->display);
    } else {
        ret = grape_display_open_default(&context->display);
    }

    if (ret != ESP_OK) {
        free(context);
        return ret;
    }

    const grape_display_info_t *info = grape_display_get_info(context->display);
    if (!info || (info->format != GRAPE_PIXEL_FORMAT_RGB565 && info->format != GRAPE_PIXEL_FORMAT_RGB888)) {
        grape_display_close(context->display);
        free(context);
        return ESP_ERR_NOT_SUPPORTED;
    }

    context->display_info = *info;
    context->background = resolved.background;
    context->rotation_backend = GRAPE_ROTATION_BACKEND_AUTO;
    context->shear_y_backend = GRAPE_SHEAR_Y_BACKEND_DIRECT;

    size_t bpp = grape_bytes_per_pixel(info->format);
    if (info->width == 0 || info->height == 0 || bpp == 0 || info->width > SIZE_MAX / info->height) {
        grape_display_close(context->display);
        free(context);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pixels = (size_t)info->width * info->height;
    if (pixels > SIZE_MAX / bpp) {
        grape_display_close(context->display);
        free(context);
        return ESP_ERR_INVALID_SIZE;
    }

    ret = grape_damage_init(context);
    if (ret != ESP_OK) {
        grape_display_close(context->display);
        free(context);
        return ret;
    }

    ESP_LOGI(TAG,
             "Damage grid: %" PRIu32 "x%" PRIu32 " tiles @ %d px, max rects=%d",
             context->damage.tile_columns,
             context->damage.tile_rows,
             CONFIG_GRAPE_DAMAGE_TILE_SIZE,
             CONFIG_GRAPE_MAX_DAMAGE_RECTS);

    ret = grape_ppa_init(context);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PPA unavailable (%s); using software compositor", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "PPA acceleration enabled");
    }

    grape_damage_all(context);
    *out_context = context;
    return ESP_OK;
}

void grape_deinit(grape_context_t *context)
{
    if (!context) {
        return;
    }

    while (context->surfaces) {
        grape_surface_destroy(context->surfaces);
    }

    while (context->textures) {
        grape_texture_destroy(context->textures);
    }

    grape_ppa_deinit(context);
    grape_damage_deinit(context);
    heap_caps_free(context->shear_buffer_a);
    heap_caps_free(context->shear_buffer_b);
    grape_display_close(context->display);
    free(context);
}

esp_err_t grape_present(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_telemetry_report_if_due();
    GRAPE_TIME_SCOPE(PRESENT);

    esp_err_t ret = ESP_OK;

    if (grape_debug_is_layer_enabled(context, GRAPE_DEBUG_LAYER_DAMAGE_RECTS)) {
        ret = grape_damage_build_logical_rects(context);
        if (ret != ESP_OK) {
            return ret;
        }
    } else {
        context->damage.final_rect_count = 0;
    }

    ret = grape_debug_prepare_frame(context);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_damage_prepare_visible(
        context,
        context->debug.render_damage,
        context->debug.render_damage_count
    );
    if (ret != ESP_OK) {
        grape_debug_reset_frame(context);
        return ret;
    }

    grape_rect_t render_damage[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t render_damage_count = 0;

    ret = grape_damage_build_render_rects(
        context,
        context->display_backbuffer_needs_full_redraw,
        render_damage,
        CONFIG_GRAPE_MAX_DAMAGE_RECTS,
        &render_damage_count
    );
    if (ret != ESP_OK) {
        grape_debug_reset_frame(context);
        return ret;
    }

    if (render_damage_count > 0) {
        grape_display_render_target_t target = {0};
        ret = grape_display_begin_frame(
            context->display,
            render_damage,
            render_damage_count,
            &target
        );
        if (ret != ESP_OK) {
            context->display_backbuffer_needs_full_redraw = true;
            grape_debug_reset_frame(context);
            return ret;
        }

        context->render_target = target;

        for (size_t i = 0; i < render_damage_count; ++i) {
            ret = grape_compositor_render(context, render_damage[i]);
            if (ret != ESP_OK) {
                context->render_target = (grape_display_render_target_t){0};
                context->display_backbuffer_needs_full_redraw = true;
                grape_debug_reset_frame(context);
                return ret;
            }
        }

        uint64_t refresh_wait_before = grape_telemetry_timer_cumulative_us(
            GRAPE_TELEMETRY_TIMER_DISPLAY_REFRESH_WAIT
        );

        ret = grape_display_present(context->display);
        context->render_target = (grape_display_render_target_t){0};

        uint64_t refresh_wait_after = grape_telemetry_timer_cumulative_us(
            GRAPE_TELEMETRY_TIMER_DISPLAY_REFRESH_WAIT
        );
        context->damage.latest_stats.refresh_wait_us =
            refresh_wait_after - refresh_wait_before;

        if (ret != ESP_OK) {
            context->display_backbuffer_needs_full_redraw = true;
            grape_debug_reset_frame(context);
            return ret;
        }

        grape_damage_commit_visible(context);
        context->display_backbuffer_needs_full_redraw = false;
    } else {
        context->damage.latest_stats.refresh_wait_us = 0;
    }

    grape_damage_clear(context);
    grape_debug_finish_frame(context);

    return ESP_OK;
}

esp_err_t grape_invalidate(grape_context_t *context, grape_rect_t rect)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    return grape_damage_add(context, rect);
}

esp_err_t grape_invalidate_all(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    grape_damage_all(context);
    return ESP_OK;
}

esp_err_t grape_set_background(grape_context_t *context, grape_color_t color)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    context->background = color;
    grape_damage_all(context);
    return ESP_OK;
}

esp_err_t grape_set_rotation_backend(grape_context_t *context, grape_rotation_backend_t backend)
{
    if (!context ||
        backend < GRAPE_ROTATION_BACKEND_AUTO ||
        backend > GRAPE_ROTATION_BACKEND_THREE_SHEAR) {
        return ESP_ERR_INVALID_ARG;
    }

    if (context->rotation_backend == backend) {
        return ESP_OK;
    }

    context->rotation_backend = backend;

    for (grape_surface_t *surface = context->surfaces;
         surface;
         surface = surface->next) {
        grape_surface_recache(surface);
    }

    grape_damage_all(context);
    return ESP_OK;
}

grape_rotation_backend_t grape_get_rotation_backend(const grape_context_t *context)
{
    return context ? context->rotation_backend : GRAPE_ROTATION_BACKEND_AUTO;
}

esp_err_t grape_set_shear_y_backend(grape_context_t *context, grape_shear_y_backend_t backend)
{
    if (!context ||
        backend < GRAPE_SHEAR_Y_BACKEND_DIRECT ||
        backend > GRAPE_SHEAR_Y_BACKEND_PPA_ROTATE) {
        return ESP_ERR_INVALID_ARG;
    }

    if (context->shear_y_backend == backend) {
        return ESP_OK;
    }

    context->shear_y_backend = backend;
    grape_damage_all(context);
    return ESP_OK;
}

grape_shear_y_backend_t grape_get_shear_y_backend(const grape_context_t *context)
{
    return context ? context->shear_y_backend : GRAPE_SHEAR_Y_BACKEND_DIRECT;
}

const grape_display_info_t *grape_get_display_info(const grape_context_t *context)
{
    return context ? &context->display_info : NULL;
}
