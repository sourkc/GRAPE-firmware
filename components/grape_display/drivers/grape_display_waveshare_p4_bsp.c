#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "bsp/display.h"
#include "grape_display_internal.h"
#include "grape/grape_telemetry.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "grape_display";

typedef struct {
    bsp_lcd_handles_t handles;
    SemaphoreHandle_t refresh_done;
    uint8_t *front_buffer;
    uint8_t *back_buffer;
    bool framebuffer_ppa_compatible;
    size_t frame_buffer_size;
    size_t bytes_per_pixel;
    volatile bool swap_armed;
    uint32_t dirty_y_min;
    uint32_t dirty_y_max;
} waveshare_state_t;

static bool IRAM_ATTR waveshare_color_trans_done(
    esp_lcd_panel_handle_t panel,
    esp_lcd_dpi_panel_event_data_t *edata,
    void *user_ctx)
{
    (void)panel;
    (void)edata;

    waveshare_state_t *state = (waveshare_state_t *)user_ctx;
    state->swap_armed = true;
    return false;
}

static bool IRAM_ATTR waveshare_refresh_done(
    esp_lcd_panel_handle_t panel,
    esp_lcd_dpi_panel_event_data_t *edata,
    void *user_ctx)
{
    (void)panel;
    (void)edata;

    waveshare_state_t *state = (waveshare_state_t *)user_ctx;
    if (!state->swap_armed) {
        return false;
    }

    state->swap_armed = false;

    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(state->refresh_done, &task_woken);
    return task_woken == pdTRUE;
}

static void waveshare_cleanup(waveshare_state_t *state)
{
    if (!state) {
        return;
    }

    if (state->refresh_done) {
        vSemaphoreDelete(state->refresh_done);
    }

    if (state->handles.panel) {
        esp_lcd_panel_del(state->handles.panel);
    }
    if (state->handles.control) {
        esp_lcd_panel_del(state->handles.control);
    }
    if (state->handles.io) {
        esp_lcd_panel_io_del(state->handles.io);
    }
    if (state->handles.mipi_dsi_bus) {
        esp_lcd_del_dsi_bus(state->handles.mipi_dsi_bus);
    }

    free(state);
}

static esp_err_t waveshare_open(grape_display_t *display)
{
#if CONFIG_BSP_LCD_DPI_BUFFER_NUMS < 2
    ESP_LOGE(TAG, "Tear-free GRAPE presentation requires at least two DPI frame buffers");
    return ESP_ERR_NOT_SUPPORTED;
#else
    waveshare_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    bsp_display_config_t config = {0};

    esp_err_t ret = bsp_display_new_with_handles(
        &config,
        &state->handles
    );

    if (ret != ESP_OK) {
        free(state);
        return ret;
    }

    display->info.name = "waveshare-p4-bsp";
    display->info.width = BSP_LCD_H_RES;
    display->info.height = BSP_LCD_V_RES;

#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
    display->info.format = GRAPE_PIXEL_FORMAT_RGB888;
    state->bytes_per_pixel = 3U;
#else
    display->info.format = GRAPE_PIXEL_FORMAT_RGB565;
    state->bytes_per_pixel = 2U;
#endif

    if (display->info.width > SIZE_MAX / display->info.height ||
        (size_t)display->info.width * display->info.height > SIZE_MAX / state->bytes_per_pixel) {
        waveshare_cleanup(state);
        return ESP_ERR_INVALID_SIZE;
    }

    state->frame_buffer_size =
        (size_t)display->info.width * display->info.height * state->bytes_per_pixel;

    void *fb0 = NULL;
    void *fb1 = NULL;
    ret = esp_lcd_dpi_panel_get_frame_buffer(
        state->handles.panel,
        2,
        &fb0,
        &fb1
    );
    if (ret != ESP_OK || !fb0 || !fb1) {
        ESP_LOGE(TAG, "Could not obtain two DPI frame buffers");
        waveshare_cleanup(state);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    state->front_buffer = (uint8_t *)fb0;
    state->back_buffer = (uint8_t *)fb1;

    size_t cache_line_size = CONFIG_CACHE_L2_CACHE_LINE_SIZE;
    state->framebuffer_ppa_compatible =
        cache_line_size != 0 &&
        ((uintptr_t)state->front_buffer % cache_line_size) == 0 &&
        ((uintptr_t)state->back_buffer % cache_line_size) == 0 &&
        (state->frame_buffer_size % cache_line_size) == 0;

    if (!state->framebuffer_ppa_compatible) {
        ESP_LOGW(TAG, "DPI framebuffer does not satisfy PPA output alignment");
    }

    memset(state->front_buffer, 0, state->frame_buffer_size);
    memset(state->back_buffer, 0, state->frame_buffer_size);

    ret = esp_lcd_panel_draw_bitmap(
        state->handles.panel,
        0,
        0,
        display->info.width,
        display->info.height,
        state->front_buffer
    );
    if (ret != ESP_OK) {
        waveshare_cleanup(state);
        return ret;
    }

    state->refresh_done = xSemaphoreCreateBinary();
    if (!state->refresh_done) {
        waveshare_cleanup(state);
        return ESP_ERR_NO_MEM;
    }

    esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_color_trans_done = waveshare_color_trans_done,
        .on_refresh_done = waveshare_refresh_done,
    };

    ret = esp_lcd_dpi_panel_register_event_callbacks(
        state->handles.panel,
        &callbacks,
        state
    );
    if (ret != ESP_OK) {
        waveshare_cleanup(state);
        return ret;
    }

    ret = esp_lcd_panel_disp_on_off(
        state->handles.panel,
        true
    );
    if (ret != ESP_OK) {
        waveshare_cleanup(state);
        return ret;
    }

    ret = bsp_display_brightness_set(
        CONFIG_GRAPE_DISPLAY_BRIGHTNESS
    );
    if (ret != ESP_OK) {
        waveshare_cleanup(state);
        return ret;
    }

    display->driver_data = state;

    ESP_LOGI(TAG,
             "Direct backbuffer rendering enabled: front=%p back=%p frame=%u bytes",
             state->front_buffer,
             state->back_buffer,
             (unsigned)state->frame_buffer_size);

    return ESP_OK;
#endif
}

static void waveshare_close(grape_display_t *display)
{
    waveshare_state_t *state = display->driver_data;
    if (!state) {
        return;
    }

    bsp_display_backlight_off();
    waveshare_cleanup(state);
    display->driver_data = NULL;
}

static void waveshare_track_dirty_rows(waveshare_state_t *state, grape_rect_t rect)
{
    uint32_t y0 = (uint32_t)rect.y;
    uint32_t y1 = (uint32_t)(rect.y + rect.height);
    if (y0 < state->dirty_y_min) {
        state->dirty_y_min = y0;
    }
    if (y1 > state->dirty_y_max) {
        state->dirty_y_max = y1;
    }
}

static esp_err_t waveshare_begin_frame(
    grape_display_t *display,
    const grape_rect_t *render_rects,
    size_t render_rect_count,
    grape_display_render_target_t *out_target)
{
    waveshare_state_t *state = display->driver_data;
    if (!state || !state->front_buffer || !state->back_buffer || !out_target) {
        return ESP_ERR_INVALID_STATE;
    }

    state->dirty_y_min = display->info.height;
    state->dirty_y_max = 0;

    for (size_t i = 0; i < render_rect_count; ++i) {
        waveshare_track_dirty_rows(state, render_rects[i]);
    }

    *out_target = (grape_display_render_target_t){
        .pixels = state->back_buffer,
        .buffer_size = state->frame_buffer_size,
        .stride = (size_t)display->info.width * state->bytes_per_pixel,
        .width = display->info.width,
        .height = display->info.height,
        .format = display->info.format,
        .ppa_compatible = state->framebuffer_ppa_compatible,
    };
    return ESP_OK;
}

static esp_err_t waveshare_present(grape_display_t *display)
{
    waveshare_state_t *state = display->driver_data;
    if (!state || !state->handles.panel || !state->back_buffer || !state->refresh_done) {
        return ESP_ERR_INVALID_STATE;
    }

    while (xSemaphoreTake(state->refresh_done, 0) == pdTRUE) {
    }
    state->swap_armed = false;

    if (state->dirty_y_min >= state->dirty_y_max ||
        state->dirty_y_max > display->info.height) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_FAIL;
    GRAPE_TIME_BLOCK(DISPLAY_SUBMIT) {
        ret = esp_lcd_panel_draw_bitmap(
            state->handles.panel,
            0,
            state->dirty_y_min,
            display->info.width,
            state->dirty_y_max,
            state->back_buffer
        );
    }

    if (ret != ESP_OK) {
        state->swap_armed = false;
        return ret;
    }

    BaseType_t refresh_complete = pdFALSE;
    GRAPE_TIME_BLOCK(DISPLAY_REFRESH_WAIT) {
        refresh_complete = xSemaphoreTake(
            state->refresh_done,
            pdMS_TO_TICKS(1000)
        );
    }

    if (refresh_complete != pdTRUE) {
        state->swap_armed = false;
        return ESP_ERR_TIMEOUT;
    }

    uint8_t *old_front = state->front_buffer;
    state->front_buffer = state->back_buffer;
    state->back_buffer = old_front;

    return ESP_OK;
}

static esp_err_t waveshare_copy_presented_frame(grape_display_t *display,
                                                 void *dst,
                                                 size_t dst_size)
{
    waveshare_state_t *state = display ? display->driver_data : NULL;
    if (!state || !state->front_buffer || !dst) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dst_size < state->frame_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(dst, state->front_buffer, state->frame_buffer_size);
    return ESP_OK;
}

static esp_err_t waveshare_set_brightness(grape_display_t *display, uint8_t percent)
{
    (void)display;
    return bsp_display_brightness_set(percent);
}

const grape_display_driver_t grape_display_driver_waveshare_p4_bsp = {
    .name = "waveshare-p4-bsp",
    .open = waveshare_open,
    .close = waveshare_close,
    .begin_frame = waveshare_begin_frame,
    .present = waveshare_present,
    .copy_presented_frame = waveshare_copy_presented_frame,
    .set_brightness = waveshare_set_brightness,
};
