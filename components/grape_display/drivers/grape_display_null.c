#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "grape_display_internal.h"

typedef struct {
    uint8_t *frame_buffer;
    size_t frame_buffer_size;
    size_t bytes_per_pixel;
} null_state_t;

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

    size_t bytes_per_pixel = display->info.format == GRAPE_PIXEL_FORMAT_RGB888 ? 3U : 2U;
    if (display->info.width > SIZE_MAX / display->info.height ||
        (size_t)display->info.width * display->info.height > SIZE_MAX / bytes_per_pixel) {
        return ESP_ERR_INVALID_SIZE;
    }

    null_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    state->bytes_per_pixel = bytes_per_pixel;
    state->frame_buffer_size =
        (size_t)display->info.width * display->info.height * bytes_per_pixel;
    state->frame_buffer = calloc(1, state->frame_buffer_size);
    if (!state->frame_buffer) {
        free(state);
        return ESP_ERR_NO_MEM;
    }

    display->driver_data = state;
    return ESP_OK;
}

static void null_close(grape_display_t *display)
{
    null_state_t *state = display->driver_data;
    if (!state) {
        return;
    }

    free(state->frame_buffer);
    free(state);
    display->driver_data = NULL;
}

static esp_err_t null_begin_frame(grape_display_t *display,
                                  const grape_rect_t *render_rects,
                                  size_t render_rect_count,
                                  grape_display_render_target_t *out_target)
{
    (void)render_rects;
    (void)render_rect_count;

    null_state_t *state = display->driver_data;
    if (!state || !state->frame_buffer || !out_target) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_target = (grape_display_render_target_t){
        .pixels = state->frame_buffer,
        .buffer_size = state->frame_buffer_size,
        .stride = (size_t)display->info.width * state->bytes_per_pixel,
        .width = display->info.width,
        .height = display->info.height,
        .format = display->info.format,
        .ppa_compatible = false,
    };
    return ESP_OK;
}

static esp_err_t null_present(grape_display_t *display)
{
    (void)display;
    return ESP_OK;
}

static esp_err_t null_copy_presented_frame(grape_display_t *display,
                                             void *dst,
                                             size_t dst_size)
{
    null_state_t *state = display ? display->driver_data : NULL;
    if (!state || !state->frame_buffer || !dst) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dst_size < state->frame_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(dst, state->frame_buffer, state->frame_buffer_size);
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
    .begin_frame = null_begin_frame,
    .present = null_present,
    .copy_presented_frame = null_copy_presented_frame,
    .set_brightness = null_set_brightness,
};
