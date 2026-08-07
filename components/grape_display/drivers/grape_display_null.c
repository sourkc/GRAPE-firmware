#include "sdkconfig.h"
#include "grape_display_internal.h"

static esp_err_t null_open(grape_display_t *display)
{
    display->info.name = "null";
    display->info.width = CONFIG_GRAPE_NULL_DISPLAY_WIDTH;
    display->info.height = CONFIG_GRAPE_NULL_DISPLAY_HEIGHT;
#if CONFIG_GRAPE_NULL_DISPLAY_RGB888
    display->info.format = GRAPE_PIXEL_FORMAT_RGB888;
#else
    display->info.format = GRAPE_PIXEL_FORMAT_RGB565;
#endif
    return ESP_OK;
}

static void null_close(grape_display_t *display)
{
    (void)display;
}

static esp_err_t null_blit(grape_display_t *display, grape_rect_t rect, const void *pixels)
{
    (void)display;
    (void)rect;
    (void)pixels;
    return ESP_OK;
}

static esp_err_t null_set_brightness(grape_display_t *display, uint8_t percent)
{
    (void)display;
    (void)percent;
    return ESP_OK;
}

const grape_display_driver_t grape_display_driver_null = {
    .name = "null",
    .open = null_open,
    .close = null_close,
    .blit = null_blit,
    .set_brightness = null_set_brightness,
};
