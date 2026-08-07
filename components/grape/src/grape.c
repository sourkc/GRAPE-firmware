#include <stdlib.h>

#include "esp_heap_caps.h"
#include "grape_internal.h"

size_t grape_bytes_per_pixel(grape_pixel_format_t format)
{
    switch (format) {
        case GRAPE_PIXEL_FORMAT_RGB565:
            return 2;
        case GRAPE_PIXEL_FORMAT_RGB888:
            return 3;
        case GRAPE_PIXEL_FORMAT_A8:
            return 1;
        default:
            return 0;
    }
}

esp_err_t grape_init(const grape_config_t *config, grape_context_t **out_context)
{
    if (!out_context) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_config_t resolved = GRAPE_CONFIG_DEFAULT();
    if (config) {
        resolved = *config;
    }

    grape_context_t *context = calloc(1, sizeof(*context));
    if (!context) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;
    if (resolved.display_driver) {
        ret = grape_display_open(resolved.display_driver, &context->display);
    } else {
        ret = grape_display_open_default(&context->display);
    }

    if (ret != ESP_OK) {
        free(context);
        return ret;
    }

    const grape_display_info_t *info = grape_display_get_info(context->display);
    if (!info || (info->format != GRAPE_PIXEL_FORMAT_RGB565 && info->format != GRAPE_PIXEL_FORMAT_RGB888)) {
        grape_display_close(context->display);
        free(context);
        return ESP_ERR_NOT_SUPPORTED;
    }

    context->display_info = *info;
    context->background = resolved.background;

    size_t bpp = grape_bytes_per_pixel(info->format);
    if (info->width == 0 || info->height == 0 || bpp == 0 || info->width > SIZE_MAX / info->height) {
        grape_display_close(context->display);
        free(context);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pixels = (size_t)info->width * info->height;
    if (pixels > SIZE_MAX / bpp) {
        grape_display_close(context->display);
        free(context);
        return ESP_ERR_INVALID_SIZE;
    }

    context->scratch_size = pixels * bpp;
    context->scratch = heap_caps_malloc(context->scratch_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!context->scratch) {
        context->scratch = heap_caps_malloc(context->scratch_size, MALLOC_CAP_8BIT);
    }

    if (!context->scratch) {
        grape_display_close(context->display);
        free(context);
        return ESP_ERR_NO_MEM;
    }

    grape_damage_all(context);
    *out_context = context;
    return ESP_OK;
}

void grape_deinit(grape_context_t *context)
{
    if (!context) {
        return;
    }

    while (context->surfaces) {
        grape_surface_destroy(context->surfaces);
    }

    while (context->textures) {
        grape_texture_destroy(context->textures);
    }

    free(context->scratch);
    grape_display_close(context->display);
    free(context);
}

esp_err_t grape_present(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t count = context->damage_count;
    for (size_t i = 0; i < count; ++i) {
        esp_err_t ret = grape_compositor_render(context, context->damage[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    context->damage_count = 0;
    return ESP_OK;
}

esp_err_t grape_invalidate(grape_context_t *context, grape_rect_t rect)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    return grape_damage_add(context, rect);
}

esp_err_t grape_invalidate_all(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    grape_damage_all(context);
    return ESP_OK;
}

esp_err_t grape_set_background(grape_context_t *context, grape_color_t color)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }
    context->background = color;
    grape_damage_all(context);
    return ESP_OK;
}

const grape_display_info_t *grape_get_display_info(const grape_context_t *context)
{
    return context ? &context->display_info : NULL;
}
