#include <stdlib.h>

#include "esp_heap_caps.h"
#include "grape_internal.h"

static uint32_t texture_caps(grape_memory_t memory)
{
    grape_memory_t resolved = memory;
    if (resolved == GRAPE_MEMORY_DEFAULT) {
#if CONFIG_GRAPE_TEXTURE_DEFAULT_PSRAM
        resolved = GRAPE_MEMORY_PSRAM;
#else
        resolved = GRAPE_MEMORY_INTERNAL;
#endif
    }

    if (resolved == GRAPE_MEMORY_PSRAM) {
        return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    }

    return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
}

esp_err_t grape_texture_create(grape_context_t *context, const grape_texture_desc_t *desc, grape_texture_t **out_texture)
{
    if (!context || !desc || !out_texture || desc->width == 0 || desc->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t bpp = grape_bytes_per_pixel(desc->format);
    if (bpp == 0 || desc->width > SIZE_MAX / bpp) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t stride = (size_t)desc->width * bpp;
    if (desc->height > SIZE_MAX / stride) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_texture_t *texture = calloc(1, sizeof(*texture));
    if (!texture) {
        return ESP_ERR_NO_MEM;
    }

    texture->size = stride * desc->height;
    texture->pixels = heap_caps_calloc(1, texture->size, texture_caps(desc->memory));
    if (!texture->pixels) {
        free(texture);
        return ESP_ERR_NO_MEM;
    }

    texture->context = context;
    texture->width = desc->width;
    texture->height = desc->height;
    texture->format = desc->format;
    texture->memory = desc->memory;
    texture->stride = stride;
    texture->next = context->textures;
    context->textures = texture;

    *out_texture = texture;
    return ESP_OK;
}

esp_err_t grape_texture_destroy(grape_texture_t *texture)
{
    if (!texture) {
        return ESP_ERR_INVALID_ARG;
    }
    if (texture->ref_count != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_context_t *context = texture->context;
    grape_texture_t **cursor = &context->textures;
    while (*cursor && *cursor != texture) {
        cursor = &(*cursor)->next;
    }
    if (*cursor == texture) {
        *cursor = texture->next;
    }

    free(texture->pixels);
    free(texture);
    return ESP_OK;
}

void *grape_texture_pixels(grape_texture_t *texture)
{
    return texture ? texture->pixels : NULL;
}

const void *grape_texture_pixels_const(const grape_texture_t *texture)
{
    return texture ? texture->pixels : NULL;
}

size_t grape_texture_stride(const grape_texture_t *texture)
{
    return texture ? texture->stride : 0;
}

uint32_t grape_texture_width(const grape_texture_t *texture)
{
    return texture ? texture->width : 0;
}

uint32_t grape_texture_height(const grape_texture_t *texture)
{
    return texture ? texture->height : 0;
}

grape_pixel_format_t grape_texture_format(const grape_texture_t *texture)
{
    return texture ? texture->format : GRAPE_PIXEL_FORMAT_RGB565;
}

esp_err_t grape_texture_invalidate(grape_texture_t *texture)
{
    if (!texture) {
        return ESP_ERR_INVALID_ARG;
    }

    for (grape_surface_t *surface = texture->context->surfaces; surface; surface = surface->next) {
        if (surface->texture == texture && surface->visible) {
            grape_damage_add(texture->context, surface->bounds);
        }
    }

    return ESP_OK;
}
