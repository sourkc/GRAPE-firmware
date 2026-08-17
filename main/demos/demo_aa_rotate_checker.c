#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "demo_internal.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "GRAPE_DEMO";

#define ROTATE_CHECKER_TEXTURE_SIZE 256U
#define ROTATE_CHECKER_RECT_X 32U
#define ROTATE_CHECKER_RECT_Y 64U
#define ROTATE_CHECKER_RECT_WIDTH 192U
#define ROTATE_CHECKER_RECT_HEIGHT 128U
#define ROTATE_CHECKER_CELL_SIZE 16U

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3U) << 11U) |
                      ((uint16_t)(g >> 2U) << 5U) |
                      (uint16_t)(b >> 3U));
}

static void fill_checker_texture(grape_texture_t *texture)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    const size_t stride = grape_texture_stride(texture);

    const uint16_t square_background = pack_rgb565(81U, 87U, 109U);   /* Catppuccin Frappe surface1 */
    const uint16_t checker_a = pack_rgb565(166U, 209U, 137U);          /* green */
    const uint16_t checker_b = pack_rgb565(239U, 159U, 118U);          /* peach */

    for (uint32_t y = 0U; y < ROTATE_CHECKER_TEXTURE_SIZE; ++y) {
        uint16_t *row = (uint16_t *)(pixels + (size_t)y * stride);
        for (uint32_t x = 0U; x < ROTATE_CHECKER_TEXTURE_SIZE; ++x) {
            const bool inside_checker =
                x >= ROTATE_CHECKER_RECT_X &&
                x < ROTATE_CHECKER_RECT_X + ROTATE_CHECKER_RECT_WIDTH &&
                y >= ROTATE_CHECKER_RECT_Y &&
                y < ROTATE_CHECKER_RECT_Y + ROTATE_CHECKER_RECT_HEIGHT;

            if (!inside_checker) {
                row[x] = square_background;
                continue;
            }

            const uint32_t checker_x = (x - ROTATE_CHECKER_RECT_X) / ROTATE_CHECKER_CELL_SIZE;
            const uint32_t checker_y = (y - ROTATE_CHECKER_RECT_Y) / ROTATE_CHECKER_CELL_SIZE;
            row[x] = ((checker_x + checker_y) & 1U) != 0U ? checker_a : checker_b;
        }
    }
}

esp_err_t grape_demo_aa_rotate_checker_run(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_texture_t *texture = NULL;
    const grape_texture_desc_t texture_desc = {
        .width = ROTATE_CHECKER_TEXTURE_SIZE,
        .height = ROTATE_CHECKER_TEXTURE_SIZE,
        .format = GRAPE_PIXEL_FORMAT_RGB565,
        .memory = GRAPE_MEMORY_DEFAULT,
    };

    esp_err_t ret = grape_texture_create(grape, &texture_desc, &texture);
    if (ret != ESP_OK) {
        return ret;
    }

    fill_checker_texture(texture);
    ret = grape_texture_invalidate(texture);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t surface_size = display->width * 3U / 4U;
    const uint32_t height_limit = display->height * 3U / 5U;
    if (surface_size > height_limit) {
        surface_size = height_limit;
    }
    if (surface_size < 192U) {
        surface_size = 192U;
    }

    grape_surface_desc_t desc = GRAPE_SURFACE_DESC_TEXTURE(texture);
    desc.width = surface_size;
    desc.height = surface_size;
    desc.texture_mode = GRAPE_SURFACE_TEXTURE_STRETCH;
    desc.texture_filter = GRAPE_TEXTURE_FILTER_LINEAR;
    desc.aa = GRAPE_SURFACE_AA_COVERAGE_4X;
    desc.transform.x = (float)display->width * 0.5f;
    desc.transform.y = (float)display->height * 0.5f;
    desc.transform.origin_x = (float)surface_size * 0.5f;
    desc.transform.origin_y = (float)surface_size * 0.5f;

    grape_surface_t *surface = NULL;
    ret = grape_surface_create(grape, &desc, &surface);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "AA rotate checker: RGB565 %ux%u texture -> %" PRIu32 "x%" PRIu32 " surface",
             ROTATE_CHECKER_TEXTURE_SIZE,
             ROTATE_CHECKER_TEXTURE_SIZE,
             surface_size,
             surface_size);
    ESP_LOGI(TAG, "outer square = geometric AA; internal checker edges = linear texture filtering");
    ESP_LOGI(TAG, "filter=linear aa=coverage4; continuously rotating");

    const int64_t start_us = esp_timer_get_time();
    int64_t fps_start_us = start_us;
    uint32_t frame_count = 0U;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        const float seconds = (float)(now_us - start_us) / 1000000.0f;

        ret = grape_surface_set_rotation(surface, seconds * 0.45f);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = grape_present(grape);
        if (ret != ESP_OK) {
            return ret;
        }

        ++frame_count;
        const int64_t elapsed_us = esp_timer_get_time() - fps_start_us;
        if (elapsed_us >= 1000000) {
            const float elapsed_seconds = (float)elapsed_us / 1000000.0f;
            printf("AA rotate checker FPS: %.2f\n", (float)frame_count / elapsed_seconds);
            frame_count = 0U;
            fps_start_us += elapsed_us;
        }
    }
}
