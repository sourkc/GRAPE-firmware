#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "grape_display_internal.h"

static const grape_display_driver_t *const drivers[] = {
#if CONFIG_GRAPE_DISPLAY_DRIVER_WAVESHARE_P4_BSP
    &grape_display_driver_waveshare_p4_bsp,
#endif
#if CONFIG_GRAPE_DISPLAY_DRIVER_NULL
    &grape_display_driver_null,
#endif
    NULL,
};

static const grape_display_driver_t *find_driver(const char *name)
{
    if (!name) {
        return NULL;
    }

    for (size_t i = 0; drivers[i]; ++i) {
        if (strcmp(drivers[i]->name, name) == 0) {
            return drivers[i];
        }
    }

    return NULL;
}

static const char *default_driver_name(void)
{
#if CONFIG_GRAPE_DISPLAY_DEFAULT_WAVESHARE_P4_BSP
    return "waveshare-p4-bsp";
#elif CONFIG_GRAPE_DISPLAY_DEFAULT_NULL
    return "null";
#else
    return NULL;
#endif
}

esp_err_t grape_display_open(const char *driver_name, grape_display_t **out_display)
{
    if (!driver_name || !out_display) {
        return ESP_ERR_INVALID_ARG;
    }

    const grape_display_driver_t *driver = find_driver(driver_name);
    if (!driver) {
        return ESP_ERR_NOT_FOUND;
    }

    grape_display_t *display = calloc(1, sizeof(*display));
    if (!display) {
        return ESP_ERR_NO_MEM;
    }

    display->driver = driver;
    esp_err_t ret = driver->open(display);
    if (ret != ESP_OK) {
        free(display);
        return ret;
    }

    if (!display->info.name) {
        display->info.name = driver->name;
    }

    *out_display = display;
    return ESP_OK;
}

esp_err_t grape_display_open_default(grape_display_t **out_display)
{
    const char *name = default_driver_name();
    if (!name) {
        return ESP_ERR_NOT_FOUND;
    }
    return grape_display_open(name, out_display);
}

void grape_display_close(grape_display_t *display)
{
    if (!display) {
        return;
    }
    if (display->driver && display->driver->close) {
        display->driver->close(display);
    }
    free(display);
}

const grape_display_info_t *grape_display_get_info(const grape_display_t *display)
{
    return display ? &display->info : NULL;
}


esp_err_t grape_display_begin_frame(grape_display_t *display,
                                    const grape_rect_t *render_rects,
                                    size_t render_rect_count,
                                    grape_display_render_target_t *out_target)
{
    if (!display || !out_target || (render_rect_count > 0 && !render_rects)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_target = (grape_display_render_target_t){0};

    if (!display->driver || !display->driver->begin_frame) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (size_t i = 0; i < render_rect_count; ++i) {
        grape_rect_t rect = render_rects[i];
        if (rect.width <= 0 || rect.height <= 0 ||
            rect.x < 0 || rect.y < 0 ||
            (uint32_t)(rect.x + rect.width) > display->info.width ||
            (uint32_t)(rect.y + rect.height) > display->info.height) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t ret = display->driver->begin_frame(
        display,
        render_rects,
        render_rect_count,
        out_target
    );
    if (ret != ESP_OK) {
        return ret;
    }

    size_t bpp;
    switch (out_target->format) {
        case GRAPE_PIXEL_FORMAT_RGB565:
            bpp = 2U;
            break;
        case GRAPE_PIXEL_FORMAT_RGB888:
            bpp = 3U;
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (!out_target->pixels ||
        out_target->width != display->info.width ||
        out_target->height != display->info.height ||
        out_target->format != display->info.format ||
        out_target->width > SIZE_MAX / bpp ||
        out_target->stride < (size_t)out_target->width * bpp ||
        out_target->height > SIZE_MAX / out_target->stride ||
        out_target->buffer_size < out_target->stride * out_target->height) {
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}


esp_err_t grape_display_present(grape_display_t *display)
{
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!display->driver || !display->driver->present) {
        return ESP_OK;
    }

    return display->driver->present(display);
}

esp_err_t grape_display_copy_presented_frame(grape_display_t *display,
                                              void *dst,
                                              size_t dst_size)
{
    if (!display || !dst) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bpp;
    switch (display->info.format) {
        case GRAPE_PIXEL_FORMAT_RGB565:
            bpp = 2U;
            break;
        case GRAPE_PIXEL_FORMAT_RGB888:
            bpp = 3U;
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }

    if (display->info.width > SIZE_MAX / bpp) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t row_bytes = (size_t)display->info.width * bpp;
    if (display->info.height > SIZE_MAX / row_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t required = row_bytes * display->info.height;
    if (dst_size < required) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!display->driver || !display->driver->copy_presented_frame) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    return display->driver->copy_presented_frame(display, dst, dst_size);
}

esp_err_t grape_display_set_brightness(grape_display_t *display, uint8_t percent)
{
    if (!display || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!display->driver || !display->driver->set_brightness) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return display->driver->set_brightness(display, percent);
}
