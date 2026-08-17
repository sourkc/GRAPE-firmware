#include <inttypes.h>
#include <stdint.h>

#include "demo_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "generated_iq_raymarch.h"

static const char *TAG = "GRAPE_DEMO";

#define SHADER_DEMO_RENDER_WIDTH 240U
#define SHADER_DEMO_RENDER_HEIGHT 135U
#define SHADER_DEMO_SCALE 3.0f
#define SHADER_DEMO_LOG_INTERVAL_FRAMES 1U

static grape_texture_t *s_shader_demo_texture;
static grape_surface_t *s_shader_demo_surface;
static generated_iq_raymarch_uniforms_t s_shader_demo_uniforms;

static esp_err_t render_shader_demo_frame(uint32_t frame, float time_seconds)
{
    s_shader_demo_uniforms.iTime = time_seconds;
    s_shader_demo_uniforms.iFrame = (int32_t)frame;
    return grape_shader_render_procedural_to_texture(
        s_shader_demo_texture,
        &generated_iq_raymarch_program,
        &s_shader_demo_uniforms
    );
}

static esp_err_t prepare_shader_demo(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_texture_desc_t desc = {
        .width = SHADER_DEMO_RENDER_WIDTH,
        .height = SHADER_DEMO_RENDER_HEIGHT,
        .format = GRAPE_PIXEL_FORMAT_RGB565,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    esp_err_t ret = grape_texture_create(grape, &desc, &s_shader_demo_texture);
    if (ret != ESP_OK) {
        return ret;
    }

    s_shader_demo_uniforms.iResolution = (grape_shader_vec3_t) {
        (float)SHADER_DEMO_RENDER_WIDTH,
        (float)SHADER_DEMO_RENDER_HEIGHT,
        1.0f,
    };
    s_shader_demo_uniforms.iTime = 0.0f;
    s_shader_demo_uniforms.iFrame = 0;
    s_shader_demo_uniforms.iMouse = (grape_shader_vec4_t) {0};

    ret = render_shader_demo_frame(0U, 0.0f);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_surface_create(grape, s_shader_demo_texture, &s_shader_demo_surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_transform_t transform = GRAPE_TRANSFORM_DEFAULT();
    transform.x = ((float)display->width - (float)SHADER_DEMO_RENDER_WIDTH * SHADER_DEMO_SCALE) * 0.5f;
    transform.y = ((float)display->height - (float)SHADER_DEMO_RENDER_HEIGHT * SHADER_DEMO_SCALE) * 0.5f;
    transform.scale_x = SHADER_DEMO_SCALE;
    transform.scale_y = SHADER_DEMO_SCALE;
    ret = grape_surface_set_transform(s_shader_demo_surface, &transform);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG,
             "IQ Shadertoy demo: %ux%u render -> %.0fx scale (%ux%u display area)",
             SHADER_DEMO_RENDER_WIDTH,
             SHADER_DEMO_RENDER_HEIGHT,
             (double)SHADER_DEMO_SCALE,
             (unsigned)(SHADER_DEMO_RENDER_WIDTH * (uint32_t)SHADER_DEMO_SCALE),
             (unsigned)(SHADER_DEMO_RENDER_HEIGHT * (uint32_t)SHADER_DEMO_SCALE));
    return grape_present(grape);
}

static void animate_shader_demo(grape_context_t *grape)
{
    int64_t start_us = esp_timer_get_time();
    uint32_t frame = 1U;

    while (1) {
        int64_t frame_start_us = esp_timer_get_time();
        float time_seconds = (float)(frame_start_us - start_us) / 1000000.0f;

        int64_t shader_start_us = esp_timer_get_time();
        ESP_ERROR_CHECK(render_shader_demo_frame(frame, time_seconds));
        int64_t shader_us = esp_timer_get_time() - shader_start_us;

        ESP_ERROR_CHECK(grape_present(grape));

        if (frame % SHADER_DEMO_LOG_INTERVAL_FRAMES == 0U) {
            int64_t frame_us = esp_timer_get_time() - frame_start_us;
            ESP_LOGI(TAG,
                     "IQ frame %" PRIu32 ": %.1f ms shader, %.1f ms total (%.3f FPS)",
                     frame,
                     (double)shader_us / 1000.0,
                     (double)frame_us / 1000.0,
                     frame_us > 0 ? 1000000.0 / (double)frame_us : 0.0);
        }

        frame++;
    }
}

esp_err_t grape_demo_shader_raymarch_run(grape_context_t *grape)
{
    esp_err_t ret = prepare_shader_demo(grape);
    if (ret != ESP_OK) {
        return ret;
    }
    animate_shader_demo(grape);
    return ESP_OK;
}
