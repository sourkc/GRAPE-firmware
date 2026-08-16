#include <inttypes.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "grape_internal.h"

static const char *TAG = "grape";

/**
 * Determines the amount of bytes per pixel from the pixel format
 *
 * @param format Format of the pixel
 * @return Size in bytes
 */
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

/**
 * Initializes a GRAPE instance
 *
 * @param config GRAPE config structure
 * @param out_context Receives the current context of the current GRAPE instance, that is being initialized
 * @return ESP_OK on success or an error code on failure
 */
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

    // Get display info. If no info is returned ot the color format is not supported, deinitialize and throw an error.
    const grape_display_info_t *info = grape_display_get_info(context->display);
    if (!info || (info->format != GRAPE_PIXEL_FORMAT_RGB565 && info->format != GRAPE_PIXEL_FORMAT_RGB888)) {
        grape_display_close(context->display);
        free(context);
        return ESP_ERR_NOT_SUPPORTED;
    }

    context->display_info = *info;
    context->background = resolved.background; // Background color
    context->rotation_backend = GRAPE_ROTATION_BACKEND_AUTO; // Backend used for rotation, see ../../include/grape/grape.h for available backends
    grape_feature_init(context);

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

    // Initialize the damage subsystem
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

    grape_ppa_init(context);

    grape_damage_all(context);
    *out_context = context; // Set the context to the generated context
    return ESP_OK;
}

/**
 * Deinitialize the GRAPE instance
 *
 * @param context Input context
 */
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

/**
 *
 * @param context GRAPE context
 * @return ESP_OK on success or an error code on error
 */
esp_err_t grape_present(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_telemetry_report_if_due(); // Do a telemetry and timings report if enabled. May affect performance!
    GRAPE_TIME_SCOPE(PRESENT);

    esp_err_t ret = ESP_OK;

    // Shows debug damage area rects if enabled. May affect performance!
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
        ); // Begin frame write on the display driver side
        if (ret != ESP_OK) {
            // If frame write failed, we do a full frame re-draw
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

        // Register time for timing reports. May affect performance!
        uint64_t refresh_wait_before = grape_telemetry_timer_cumulative_us(
            GRAPE_TELEMETRY_TIMER_DISPLAY_REFRESH_WAIT
        );

        ret = grape_display_present(context->display); // Push the framebuffer onto the display
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

/**
 * Adds a rectangular area to dirty regions to force redraw
 * of said region next frame
 *
 * @param context GRAPE context
 * @param rect Rectangle to invalidate
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_invalidate(grape_context_t *context, grape_rect_t rect)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    return grape_damage_add(context, rect);
}

/**
 * Adds the whole screen to dirty regions to force redraw
 * on next frame
 *
 * @param context GRAPE context
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_invalidate_all(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    grape_damage_all(context);
    return ESP_OK;
}

/**
 * Sets the background color of the screen
 *
 * @param context GRAPE context
 * @param color Background color
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_set_background(grape_context_t *context, grape_color_t color)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    context->background = color;
    grape_damage_all(context);
    return ESP_OK;
}

/**
 * Sets the rotation backend. Types of backends:
 *  GRAPE_ROTATION_BACKEND_AUTO (default): Automatically decide the rotation backend
 *
 * @param context GRAPE context
 * @param backend Rotation backend
 * @return ESP_OK on success or an error code
 */
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

/**
 * Returns the current rotation backend
 *
 * @param context GRAPE context
 * @return Current rotation backend
 */
grape_rotation_backend_t grape_get_rotation_backend(const grape_context_t *context)
{
    return context ? context->rotation_backend : GRAPE_ROTATION_BACKEND_AUTO;
}

/**
 * Returns information on the current display
 *
 * @param context GRAPE context
 * @return Current display information
 */
const grape_display_info_t *grape_get_display_info(const grape_context_t *context)
{
    return context ? &context->display_info : NULL;
}
