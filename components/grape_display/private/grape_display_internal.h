#pragma once

#include "grape/grape_display.h"

typedef struct grape_display_driver {
    const char *name;
    esp_err_t (*open)(grape_display_t *display);
    void (*close)(grape_display_t *display);
    esp_err_t (*blit)(grape_display_t *display, grape_rect_t rect, const void *pixels);
    esp_err_t (*set_brightness)(grape_display_t *display, uint8_t percent);
} grape_display_driver_t;

struct grape_display {
    const grape_display_driver_t *driver;
    grape_display_info_t info;
    void *driver_data;
};

#if CONFIG_GRAPE_DISPLAY_DRIVER_WAVESHARE_P4_BSP
extern const grape_display_driver_t grape_display_driver_waveshare_p4_bsp;
#endif

#if CONFIG_GRAPE_DISPLAY_DRIVER_NULL
extern const grape_display_driver_t grape_display_driver_null;
#endif
