#include <stdbool.h>
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
#include "grape/grape_profile.h"
#include "grape/grape_diagnostics_config.h"

#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
#include "esp_timer.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "grape_display";

typedef struct {
    bsp_lcd_handles_t handles;
    SemaphoreHandle_t refresh_done;
    uint8_t *front_buffer;
    uint8_t *back_buffer;
    size_t frame_buffer_size;
    size_t bytes_per_pixel;
    volatile bool swap_armed;
    uint32_t dirty_y_min;
    uint32_t dirty_y_max;
    uint64_t refresh_wait_us;
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
             "Double-buffered DPI presentation enabled: front=%p back=%p frame=%u bytes",
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
    const grape_rect_t *sync_rects,
    size_t sync_rect_count)
{
    waveshare_state_t *state = display->driver_data;
    if (!state || !state->front_buffer || !state->back_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    state->dirty_y_min = display->info.height;
    state->dirty_y_max = 0;

    for (size_t i = 0; i < sync_rect_count; ++i) {
        grape_rect_t rect = sync_rects[i];
        if (rect.width <= 0 || rect.height <= 0 ||
            rect.x < 0 || rect.y < 0 ||
            (uint32_t)(rect.x + rect.width) > display->info.width ||
            (uint32_t)(rect.y + rect.height) > display->info.height) {
            return ESP_ERR_INVALID_ARG;
        }

        size_t row_bytes = (size_t)rect.width * state->bytes_per_pixel;
        size_t row_stride = (size_t)display->info.width * state->bytes_per_pixel;
        size_t first_offset =
            ((size_t)rect.y * display->info.width + (size_t)rect.x) * state->bytes_per_pixel;

        const uint8_t *src = state->front_buffer + first_offset;
        uint8_t *dst = state->back_buffer + first_offset;

        for (int32_t y = 0; y < rect.height; ++y) {
            memcpy(dst, src, row_bytes);
            src += row_stride;
            dst += row_stride;
        }

        waveshare_track_dirty_rows(state, rect);
    }

    return ESP_OK;
}

static esp_err_t waveshare_blit(
    grape_display_t *display,
    grape_rect_t rect,
    const void *pixels)
{
    waveshare_state_t *state = display->driver_data;

    if (!state || !state->back_buffer) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t source_stride = (size_t)rect.width * state->bytes_per_pixel;
    size_t destination_stride = (size_t)display->info.width * state->bytes_per_pixel;
    size_t first_offset =
        ((size_t)rect.y * display->info.width + (size_t)rect.x) * state->bytes_per_pixel;

    const uint8_t *src = (const uint8_t *)pixels;
    uint8_t *dst = state->back_buffer + first_offset;

    for (int32_t y = 0; y < rect.height; ++y) {
        memcpy(dst, src, source_stride);
        src += source_stride;
        dst += destination_stride;
    }

    waveshare_track_dirty_rows(state, rect);
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

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_LCD_DRAW_SUBMIT
    int64_t draw_submit_start_us = grape_profile_timestamp();
#endif
    if (state->dirty_y_min >= state->dirty_y_max ||
        state->dirty_y_max > display->info.height) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = esp_lcd_panel_draw_bitmap(
        state->handles.panel,
        0,
        state->dirty_y_min,
        display->info.width,
        state->dirty_y_max,
        state->back_buffer
    );
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_LCD_DRAW_SUBMIT
    grape_profile_record(GRAPE_PROFILE_METRIC_LCD_DRAW_SUBMIT,
                         grape_profile_timestamp() - draw_submit_start_us);
#endif

    if (ret != ESP_OK) {
        state->swap_armed = false;
        return ret;
    }

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_LCD_DRAW_WAIT
    int64_t draw_wait_start_us = grape_profile_timestamp();
#endif
#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
    int64_t refresh_wait_start_us = esp_timer_get_time();
#endif
    BaseType_t refresh_complete = xSemaphoreTake(
        state->refresh_done,
        pdMS_TO_TICKS(1000)
    );
#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
    state->refresh_wait_us =
        (uint64_t)(esp_timer_get_time() - refresh_wait_start_us);
#else
    state->refresh_wait_us = 0;
#endif
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_LCD_DRAW_WAIT
    grape_profile_record(GRAPE_PROFILE_METRIC_LCD_DRAW_WAIT,
                         grape_profile_timestamp() - draw_wait_start_us);
#endif

    if (refresh_complete != pdTRUE) {
        state->swap_armed = false;
        return ESP_ERR_TIMEOUT;
    }

    uint8_t *old_front = state->front_buffer;
    state->front_buffer = state->back_buffer;
    state->back_buffer = old_front;

    return ESP_OK;
}

static esp_err_t waveshare_get_frame_stats(const grape_display_t *display,
                                             grape_display_frame_stats_t *out_stats)
{
    if (!display || !out_stats) {
        return ESP_ERR_INVALID_ARG;
    }

    const waveshare_state_t *state = display->driver_data;
    if (!state) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_stats = (grape_display_frame_stats_t){
        .refresh_wait_us = state->refresh_wait_us,
    };
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
    .blit = waveshare_blit,
    .present = waveshare_present,
    .get_frame_stats = waveshare_get_frame_stats,
    .set_brightness = waveshare_set_brightness,
};
