#include <stdint.h>

#include "demo_internal.h"
#include "esp_log.h"
#include "generated_brightness.h"
#include "generated_procedural_gradient.h"

static const char *TAG = "GRAPE_DEMO";

#define SURFACE_DEMO_TEXTURE_WIDTH 96U
#define SURFACE_DEMO_TEXTURE_HEIGHT 48U
#define SURFACE_DEMO_MARGIN 24U
#define SURFACE_DEMO_GAP 12U

static grape_texture_t *s_texture;
static grape_surface_t *s_layout_surfaces[5];
static grape_surface_t *s_shader_surface;
static generated_brightness_uniforms_t s_brightness = {
    .brightness = 0.65f,
};

static void fill_demo_texture(grape_texture_t *texture)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    const size_t stride = grape_texture_stride(texture);

    for (uint32_t y = 0U; y < SURFACE_DEMO_TEXTURE_HEIGHT; ++y) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0U; x < SURFACE_DEMO_TEXTURE_WIDTH; ++x) {
            uint8_t *pixel = row + (size_t)x * 4U;
            const bool checker = (((x / 12U) + (y / 12U)) & 1U) != 0U;
            const bool left = x < SURFACE_DEMO_TEXTURE_WIDTH / 2U;
            const bool top = y < SURFACE_DEMO_TEXTURE_HEIGHT / 2U;

            if (left && top) {
                pixel[0] = checker ? 166U : 129U;
                pixel[1] = checker ? 209U : 200U;
                pixel[2] = checker ? 137U : 120U;
            } else if (!left && top) {
                pixel[0] = checker ? 231U : 220U;
                pixel[1] = checker ? 130U : 115U;
                pixel[2] = checker ? 132U : 120U;
            } else if (left) {
                pixel[0] = checker ? 140U : 120U;
                pixel[1] = checker ? 170U : 150U;
                pixel[2] = checker ? 238U : 220U;
            } else {
                pixel[0] = checker ? 245U : 220U;
                pixel[1] = checker ? 225U : 205U;
                pixel[2] = checker ? 170U : 150U;
            }
            pixel[3] = 255U;
        }
    }
}

esp_err_t grape_demo_surface_features_run(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display || display->width <= SURFACE_DEMO_MARGIN * 2U ||
        display->height <= SURFACE_DEMO_MARGIN * 2U) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_texture_desc_t texture_desc = {
        .width = SURFACE_DEMO_TEXTURE_WIDTH,
        .height = SURFACE_DEMO_TEXTURE_HEIGHT,
        .format = GRAPE_PIXEL_FORMAT_RGBA8888,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    esp_err_t ret = grape_texture_create(grape, &texture_desc, &s_texture);
    if (ret != ESP_OK) {
        return ret;
    }
    fill_demo_texture(s_texture);
    ret = grape_texture_invalidate(s_texture);
    if (ret != ESP_OK) {
        return ret;
    }

    const grape_surface_texture_mode_t modes[] = {
        GRAPE_SURFACE_TEXTURE_STRETCH,
        GRAPE_SURFACE_TEXTURE_TILE,
        GRAPE_SURFACE_TEXTURE_FIT,
        GRAPE_SURFACE_TEXTURE_COVER,
        GRAPE_SURFACE_TEXTURE_CENTER,
    };
    const char *mode_names[] = {"stretch", "tile", "fit", "cover", "center"};

    const uint32_t usable_width = display->width - SURFACE_DEMO_MARGIN * 2U;
    const uint32_t shader_height = display->height / 7U;
    const uint32_t layout_space = display->height - SURFACE_DEMO_MARGIN * 2U -
                                  shader_height - SURFACE_DEMO_GAP * 5U;
    const uint32_t panel_height = layout_space / 5U;
    if (panel_height == 0U || shader_height == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t y = SURFACE_DEMO_MARGIN;
    for (size_t i = 0U; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        grape_surface_desc_t desc = GRAPE_SURFACE_DESC_DEFAULT();
        desc.texture = s_texture;
        desc.width = usable_width;
        desc.height = panel_height;
        desc.texture_mode = modes[i];
        desc.transform.x = (float)SURFACE_DEMO_MARGIN;
        desc.transform.y = (float)y;
        desc.z = (int32_t)i;

        ret = grape_surface_create(grape, &desc, &s_layout_surfaces[i]);
        if (ret != ESP_OK) {
            return ret;
        }
        ESP_LOGI(TAG, "surface layout %u: %s", (unsigned)i, mode_names[i]);
        y += panel_height + SURFACE_DEMO_GAP;
    }

    const grape_surface_shader_desc_t shaders[] = {
        {
            .program = &generated_procedural_gradient_program,
            .uniforms = NULL,
        },
        {
            .program = &generated_brightness_program,
            .uniforms = &s_brightness,
        },
    };
    grape_surface_desc_t shader_desc = GRAPE_SURFACE_DESC_DEFAULT();
    shader_desc.width = usable_width;
    shader_desc.height = shader_height;
    shader_desc.shaders = shaders;
    shader_desc.shader_count = sizeof(shaders) / sizeof(shaders[0]);
    shader_desc.transform.x = (float)SURFACE_DEMO_MARGIN;
    shader_desc.transform.y = (float)y;
    shader_desc.z = 10;

    ret = grape_surface_create(grape, &shader_desc, &s_shader_surface);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "surface feature demo: 5 texture layouts + procedural_gradient -> brightness shader chain");
    return grape_present(grape);
}
