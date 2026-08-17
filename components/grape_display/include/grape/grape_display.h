#pragma once

#include <stdbool.h>
#include <stddef.h>
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

typedef struct {
    void *pixels;
    size_t buffer_size;
    size_t stride;
    uint32_t width;
    uint32_t height;
    grape_pixel_format_t format;
    bool ppa_compatible;
} grape_display_render_target_t;

esp_err_t grape_display_open(const char *driver_name, grape_display_t **out_display);
esp_err_t grape_display_open_default(grape_display_t **out_display);
void grape_display_close(grape_display_t *display);
const grape_display_info_t *grape_display_get_info(const grape_display_t *display);
esp_err_t grape_display_begin_frame(grape_display_t *display,
                                    const grape_rect_t *render_rects,
                                    size_t render_rect_count,
                                    grape_display_render_target_t *out_target);
esp_err_t grape_display_present(grape_display_t *display);

/**
 * Copies the framebuffer that is currently presented by the display into a
 * tightly packed caller-owned buffer. The required size is
 * width * height * bytes_per_pixel for the format reported by
 * grape_display_get_info().
 *
 * The call is synchronous. The caller must provide the same external
 * synchronization that it would use around grape_display_present().
 */
esp_err_t grape_display_copy_presented_frame(grape_display_t *display,
                                              void *dst,
                                              size_t dst_size);

esp_err_t grape_display_set_brightness(grape_display_t *display, uint8_t percent);

#ifdef __cplusplus
}
#endif
