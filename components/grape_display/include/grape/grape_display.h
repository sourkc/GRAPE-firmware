#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "grape/grape_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_display grape_display_t;

typedef struct {
    const char *name;
    uint32_t width;
    uint32_t height;
    grape_pixel_format_t format;
} grape_display_info_t;

esp_err_t grape_display_open(const char *driver_name, grape_display_t **out_display);
esp_err_t grape_display_open_default(grape_display_t **out_display);
void grape_display_close(grape_display_t *display);
const grape_display_info_t *grape_display_get_info(const grape_display_t *display);
esp_err_t grape_display_blit(grape_display_t *display, grape_rect_t rect, const void *pixels);
esp_err_t grape_display_set_brightness(grape_display_t *display, uint8_t percent);

#ifdef __cplusplus
}
#endif
