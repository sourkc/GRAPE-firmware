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
                                    const grape_rect_t *sync_rects,
                                    size_t sync_rect_count)
{
    if (!display || (sync_rect_count > 0 && !sync_rects)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!display->driver || !display->driver->begin_frame) {
        return ESP_OK;
    }

    return display->driver->begin_frame(display, sync_rects, sync_rect_count);
}

esp_err_t grape_display_blit(grape_display_t *display, grape_rect_t rect, const void *pixels)
{
    if (!display || !display->driver || !display->driver->blit || !pixels || rect.width <= 0 || rect.height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (rect.x < 0 || rect.y < 0 ||
        (uint32_t)(rect.x + rect.width) > display->info.width ||
        (uint32_t)(rect.y + rect.height) > display->info.height) {
        return ESP_ERR_INVALID_ARG;
    }

    return display->driver->blit(display, rect, pixels);
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

esp_err_t grape_display_get_frame_stats(const grape_display_t *display,
                                        grape_display_frame_stats_t *out_stats)
{
    if (!display || !out_stats) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_stats = (grape_display_frame_stats_t){0};

    if (!display->driver || !display->driver->get_frame_stats) {
        return ESP_OK;
    }

    return display->driver->get_frame_stats(display, out_stats);
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
