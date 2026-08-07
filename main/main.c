#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "grape/grape.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GRAPE";

static void fill_rgb565(grape_texture_t *texture)
{
    uint8_t *base = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);

    for (uint32_t y = 0; y < height; ++y) {
        uint16_t *row = (uint16_t *)(base + y * stride);
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = (uint8_t)((x * 255U) / (width - 1U));
            uint8_t g = (uint8_t)((y * 255U) / (height - 1U));
            uint8_t b = 48;
            row[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

static grape_color_t hue_to_rgb(float hue)
{
    float c = 1.0f;
    float x = 1.0f - fabsf(fmodf(hue / 60.0f, 2.0f) - 1.0f);

    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    if (hue < 60.0f) {
        r = c;
        g = x;
    } else if (hue < 120.0f) {
        r = x;
        g = c;
    } else if (hue < 180.0f) {
        g = c;
        b = x;
    } else if (hue < 240.0f) {
        g = x;
        b = c;
    } else if (hue < 300.0f) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }

    return (grape_color_t) {
        .r = (uint8_t)(r * 255.0f),
        .g = (uint8_t)(g * 255.0f),
        .b = (uint8_t)(b * 255.0f),
        .a = 255,
    };
}

static void fill_rgb888(grape_texture_t *texture)
{
    uint8_t *base = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = base + y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = 32;
            row[x * 3 + 1] = (uint8_t)((x * 255U) / (width - 1U));
            row[x * 3 + 2] = (uint8_t)((y * 255U) / (height - 1U));
        }
    }
}

static void fill_a8(grape_texture_t *texture)
{
    uint8_t *base = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);
    float cx = (float)width * 0.5f;
    float cy = (float)height * 0.5f;
    float radius = (float)(width < height ? width : height) * 0.45f;

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = base + y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float edge = radius - d;
            if (edge <= 0.0f) {
                row[x] = 0;
            } else if (edge >= 8.0f) {
                row[x] = 255;
            } else {
                row[x] = (uint8_t)(edge * (255.0f / 8.0f));
            }
        }
    }
}

static void test_square_a8(grape_texture_t *texture, const uint8_t alpha) {
    uint8_t *base = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = base + y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            row[x] = alpha;
        }
    }
}

static void test_circle_a8(grape_texture_t *texture, const uint8_t alpha) {
    uint8_t *base = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);
    float cx = (float)width * 0.5f;
    float cy = (float)height * 0.5f;
    float radius = (float)(width < height ? width : height) * 0.5f;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = base + y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            float dx = (float)x + 0.5f - cx;
            float dy = (float)y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float edge = radius - d;
            row[x] = 255;
            if (edge <= 0.0f) row[x] = 0;
        }
    }
}

void app_main(void)
{
    grape_context_t *grape = NULL;
    grape_config_t config = GRAPE_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(grape_init(&config, &grape));

    /*************
     TEXTURE INIT
    *************/

    grape_texture_t *square_a128 = NULL;
    grape_texture_t *square_a255 = NULL;
    grape_texture_t *circle_a128 = NULL;
    grape_texture_t *circle_a255 = NULL;

    grape_texture_desc_t tex_desc = {
        .width = 128,
        .height = 128,
        .format = GRAPE_PIXEL_FORMAT_A8, // temporary, set per-texture
        .memory = GRAPE_MEMORY_DEFAULT,
    };

    ESP_ERROR_CHECK(grape_texture_create(grape, &tex_desc, &square_a128));
    ESP_ERROR_CHECK(grape_texture_create(grape, &tex_desc, &square_a255));
    ESP_ERROR_CHECK(grape_texture_create(grape, &tex_desc, &circle_a128));
    ESP_ERROR_CHECK(grape_texture_create(grape, &tex_desc, &circle_a255));

    test_square_a8(square_a128, 128);
    test_square_a8(square_a255, 255);
    test_circle_a8(circle_a128, 128);
    test_circle_a8(circle_a255, 255);

    /*************
     SURFACE INIT
    *************/

    #define CIRCLES_COUNT 6
    #define SQUARES_COUNT 6

    grape_surface_t *circles[CIRCLES_COUNT] = {0};
    grape_surface_t *squares[SQUARES_COUNT] = {0};

    for (size_t i = 0; i < CIRCLES_COUNT; i++) {
        ESP_ERROR_CHECK(grape_surface_create(grape, circle_a255, &circles[i]));
    }

    for (size_t i = 0; i < SQUARES_COUNT; i++) {
        ESP_ERROR_CHECK(grape_surface_create(grape, square_a255, &squares[i]));
    }

    grape_surface_set_opacity(squares[1], 128);
    grape_surface_set_opacity(squares[5], 128);


    /*************
     SURFACE LAYOUT
    *************/

    const grape_display_info_t *display_info = grape_get_display_info(grape);

    const float area_width  = (float)display_info->width / 2.0f;
    const float area_height = (float)display_info->height / 3.0f;

    const float surface_origin_x = 64.0f;
    const float surface_origin_y = 64.0f;

    const float square_offset_x = 48.0f;
    const float square_offset_y = -48.0f;

    srand(0); // set seed

    for (uint8_t i = 0; i < CIRCLES_COUNT; i++) {
        uint8_t column = i % 2;
        uint8_t row = i / 2;

        float center_x = ((float)column + 0.5f) * area_width;
        float center_y = ((float)row + 0.5f) * area_height;
        // float circle_hue = (float)(rand() % 360);
        // float square_hue = (float)(rand() % 360);
        float circle_hue = (float)180;
        float square_hue = (float)0;
        grape_color_t circle_color = hue_to_rgb(circle_hue);
        grape_color_t square_color = hue_to_rgb(square_hue);

        ESP_ERROR_CHECK(
            grape_surface_set_tint(circles[i], circle_color)
        );

        ESP_ERROR_CHECK(
            grape_surface_set_tint(squares[i], square_color)
        );

        ESP_ERROR_CHECK(
            grape_surface_set_origin(
                circles[i],
                surface_origin_x,
                surface_origin_y
            )
        );

        ESP_ERROR_CHECK(
            grape_surface_set_position(
                circles[i],
                center_x,
                center_y
            )
        );

        ESP_ERROR_CHECK(
            grape_surface_set_z(circles[i], 0)
        );

        ESP_ERROR_CHECK(
            grape_surface_set_origin(
                squares[i],
                surface_origin_x,
                surface_origin_y
            )
        );

        ESP_ERROR_CHECK(
            grape_surface_set_position(
                squares[i],
                center_x + square_offset_x,
                center_y + square_offset_y
            )
        );

        ESP_ERROR_CHECK(
            grape_surface_set_z(squares[i], 1)
        );
    }

    /************
     RENDER LOOP
    ************/

    const grape_display_info_t *display = grape_get_display_info(grape);
    ESP_LOGI(TAG, "display=%s %ux%u format=%d", display->name, display->width, display->height, display->format);

    ESP_ERROR_CHECK(grape_texture_invalidate(square_a128));
    ESP_ERROR_CHECK(grape_texture_invalidate(square_a255));
    ESP_ERROR_CHECK(grape_texture_invalidate(circle_a128));
    ESP_ERROR_CHECK(grape_texture_invalidate(circle_a255));

    grape_transform_t t_original[SQUARES_COUNT];
    for (uint8_t i = 0; i < SQUARES_COUNT; i++) {
        t_original[i] = *grape_surface_transform(squares[i]);
    }

    #define M_PI 3.14159265358979323846

    int64_t start_time = esp_timer_get_time();
    int64_t fps_start_time = esp_timer_get_time();
    uint32_t frame_count = 0;
    while (1) {
        float time = (float)(esp_timer_get_time() - start_time) / 1000000.0f;
        float animation1 = sinf(time);
        float animation2 = cosf(time);

        grape_transform_t t = t_original[5];

        t.x = t_original[5].x + 64.0f * animation1;
        t.y = t_original[5].y + 64.0f * animation1;

        t.scale_x = t_original[5].scale_x + 0.25f * animation1;
        t.scale_y = t_original[5].scale_y + 0.25f * animation2;

        t.rotation = animation1 * (M_PI / 4.0f);

        grape_surface_set_position(squares[2], t_original[2].x + 64.0f * animation1, t_original[2].y + 64.0f * animation1);
        grape_surface_set_scale(squares[3], t.scale_x, t.scale_y);
        grape_surface_set_rotation(squares[4], t.rotation);
        grape_surface_set_position(squares[5], t_original[5].x + 64.0f * animation1, t_original[5].y + 64.0f * animation1);
        grape_surface_set_scale(squares[5], t.scale_x, t.scale_y);
        grape_surface_set_rotation(squares[5], t.rotation);

        ESP_ERROR_CHECK(grape_present(grape));

        ESP_ERROR_CHECK(grape_present(grape));

        frame_count++;

        int64_t now = esp_timer_get_time();
        int64_t elapsed_us = now - fps_start_time;

        if (elapsed_us >= 1000000) {
            float elapsed_seconds = (float)elapsed_us / 1000000.0f;
            float fps = (float)frame_count / elapsed_seconds;

            printf("FPS: %.2f\n", fps);

            frame_count = 0;
            fps_start_time = now;
        }

        vTaskDelay(1);
    }
}
