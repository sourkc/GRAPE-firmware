#include <stdlib.h>

#include "sdkconfig.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "bsp/display.h"
#include "grape_display_internal.h"
#include "grape/grape_profile.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    bsp_lcd_handles_t handles;
    SemaphoreHandle_t draw_done;
} waveshare_state_t;

static bool waveshare_color_trans_done(
    esp_lcd_panel_handle_t panel,
    esp_lcd_dpi_panel_event_data_t *edata,
    void *user_ctx)
{
    (void)panel;
    (void)edata;

    SemaphoreHandle_t draw_done = (SemaphoreHandle_t)user_ctx;

    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(draw_done, &task_woken);

    return task_woken == pdTRUE;
}

static esp_err_t waveshare_open(grape_display_t *display)
{
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

    // Create a semaphore to wait until the driver's async color transfer finished reading the buffer
    state->draw_done = xSemaphoreCreateBinary();

    if (!state->draw_done) {
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
        return ESP_ERR_NO_MEM;
    }

    // Make the DPI driver notify when draw_bitmap's async transfer stopped using the source pixel buffer
    esp_lcd_dpi_panel_event_callbacks_t callbacks = {
        .on_color_trans_done = waveshare_color_trans_done,
    };

    ret = esp_lcd_dpi_panel_register_event_callbacks(
        state->handles.panel,
        &callbacks,
        state->draw_done
    );

    if (ret != ESP_OK) {
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

        vSemaphoreDelete(state->draw_done);
        free(state);

        return ret;
    }

    ret = esp_lcd_panel_disp_on_off(
        state->handles.panel,
        true
    );

    if (ret != ESP_OK) {
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

        vSemaphoreDelete(state->draw_done);
        free(state);

        return ret;
    }

    ret = bsp_display_brightness_set(
        CONFIG_GRAPE_DISPLAY_BRIGHTNESS
    );

    if (ret != ESP_OK) {
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

        vSemaphoreDelete(state->draw_done);
        free(state);

        return ret;
    }

    display->driver_data = state;

    display->info.name = "waveshare-p4-bsp";
    display->info.width = BSP_LCD_H_RES;
    display->info.height = BSP_LCD_V_RES;

#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
    display->info.format = GRAPE_PIXEL_FORMAT_RGB888;
#else
    display->info.format = GRAPE_PIXEL_FORMAT_RGB565;
#endif

    return ESP_OK;
}

static void waveshare_close(grape_display_t *display)
{
    waveshare_state_t *state = display->driver_data;
    if (!state) {
        return;
    }

    if (state->draw_done) {
        vSemaphoreDelete(state->draw_done);
    }

    bsp_display_backlight_off();
    if (state->handles.panel) esp_lcd_panel_del(state->handles.panel);
    if (state->handles.control) esp_lcd_panel_del(state->handles.control);
    if (state->handles.io) esp_lcd_panel_io_del(state->handles.io);
    if (state->handles.mipi_dsi_bus) esp_lcd_del_dsi_bus(state->handles.mipi_dsi_bus);
    free(state);
    display->driver_data = NULL;
}

static esp_err_t waveshare_blit(
    grape_display_t *display,
    grape_rect_t rect,
    const void *pixels)
{
    waveshare_state_t *state = display->driver_data;

    if (!state || !state->handles.panel) {
        return ESP_ERR_INVALID_STATE;
    }

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_LCD_DRAW_SUBMIT
    int64_t draw_submit_start_us = grape_profile_timestamp();
#endif
    esp_err_t ret = esp_lcd_panel_draw_bitmap(
        state->handles.panel,
        rect.x,
        rect.y,
        rect.x + rect.width,
        rect.y + rect.height,
        pixels
    );
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_LCD_DRAW_SUBMIT
    grape_profile_record(GRAPE_PROFILE_METRIC_LCD_DRAW_SUBMIT,
                         grape_profile_timestamp() - draw_submit_start_us);
#endif

    if (ret != ESP_OK) {
        return ret;
    }

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_LCD_DRAW_WAIT
    int64_t draw_wait_start_us = grape_profile_timestamp();
#endif
    BaseType_t draw_complete = xSemaphoreTake(
        state->draw_done,
        pdMS_TO_TICKS(1000)
    );
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_LCD_DRAW_WAIT
    grape_profile_record(GRAPE_PROFILE_METRIC_LCD_DRAW_WAIT,
                         grape_profile_timestamp() - draw_wait_start_us);
#endif

    if (draw_complete != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

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
    .blit = waveshare_blit,
    .set_brightness = waveshare_set_brightness,
};
