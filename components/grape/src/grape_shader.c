#include "grape_internal.h"
#include "grape_shader_runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define GRAPE_SHADER_PROCEDURAL_STRIP_ROWS 8

esp_err_t grape_shader_render_procedural_to_texture(grape_texture_t *target,
                                                     const grape_shader_program_t *shader,
                                                     const void *uniforms)
{
    if (!target || !shader || !shader->kernel ||
        target->format == GRAPE_PIXEL_FORMAT_A8 ||
        (shader->flags & GRAPE_SHADER_PROGRAM_USES_SOURCE_COLOR) != 0U ||
        (shader->uniform_size > 0U && !uniforms)) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_shader_kernel_args_t args = {
        .texture_pixels = NULL,
        .texture_stride = 0U,
        .texture_width = 0U,
        .texture_height = 0U,
        .texture_format = GRAPE_PIXEL_FORMAT_A8,
        .surface_width = target->width,
        .surface_height = target->height,
        .target_pixels = target->pixels,
        .target_stride = target->stride,
        .target_format = target->format,
        .clipped = {0, 0, (int32_t)target->width, (int32_t)target->height},
        .tint = {255, 255, 255, 255},
        .opacity = 255,
        .local_x_from_screen_x = 1.0f,
        .local_x_from_screen_y = 0.0f,
        .local_x_offset = 0.0f,
        .local_y_from_screen_x = 0.0f,
        .local_y_from_screen_y = 1.0f,
        .local_y_offset = 0.0f,
    };

    for (uint32_t y = 0U; y < target->height; y += GRAPE_SHADER_PROCEDURAL_STRIP_ROWS) {
        uint32_t rows = target->height - y;
        if (rows > GRAPE_SHADER_PROCEDURAL_STRIP_ROWS) {
            rows = GRAPE_SHADER_PROCEDURAL_STRIP_ROWS;
        }

        args.clipped.y = (int32_t)y;
        args.clipped.height = (int32_t)rows;
        shader->kernel(&args, uniforms);

        if (y + rows < target->height) {
            vTaskDelay(1);
        }
    }

    return grape_texture_invalidate(target);
}
