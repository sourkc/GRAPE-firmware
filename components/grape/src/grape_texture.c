#include <stdlib.h>
#include <string.h>

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
        return MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    }

    return MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
}

static inline size_t occupancy_index(const grape_texture_t *texture,
                                     uint32_t x,
                                     uint32_t y)
{
    return (size_t)y * texture->occupancy_columns + x;
}

static inline void occupancy_set(uint8_t *bitmap, size_t index)
{
    bitmap[index >> 3U] |= (uint8_t)(1U << (index & 7U));
}

static esp_err_t texture_init_occupancy(grape_texture_t *texture)
{
    const uint32_t cell_size = CONFIG_GRAPE_TEXTURE_OCCUPANCY_CELL_SIZE;
    uint64_t columns = ((uint64_t)texture->width + cell_size - 1U) / cell_size;
    uint64_t rows = ((uint64_t)texture->height + cell_size - 1U) / cell_size;

    if (columns == 0 || rows == 0 ||
        columns > UINT32_MAX || rows > UINT32_MAX ||
        columns > SIZE_MAX / rows) {
        return ESP_ERR_INVALID_SIZE;
    }

    texture->occupancy_columns = (uint32_t)columns;
    texture->occupancy_rows = (uint32_t)rows;

    size_t cell_count = (size_t)columns * (size_t)rows;
    if (texture->format != GRAPE_PIXEL_FORMAT_A8) {
        texture->occupancy_occupied_count = cell_count;
        texture->occupancy_all_full = true;
        texture->occupancy_all_empty = false;
        return ESP_OK;
    }

    if (cell_count > SIZE_MAX - 7U) {
        return ESP_ERR_INVALID_SIZE;
    }

    texture->occupancy_bitmap_size = (cell_count + 7U) / 8U;
    texture->occupancy = heap_caps_calloc(
        1,
        texture->occupancy_bitmap_size,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    );

    if (!texture->occupancy) {
        texture->occupancy = heap_caps_calloc(
            1,
            texture->occupancy_bitmap_size,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
    }

    if (!texture->occupancy) {
        return ESP_ERR_NO_MEM;
    }

    texture->occupancy_occupied_count = 0;
    texture->occupancy_all_full = false;
    texture->occupancy_all_empty = true;
    return ESP_OK;
}

esp_err_t grape_texture_rebuild_occupancy(grape_texture_t *texture)
{
    if (!texture) {
        return ESP_ERR_INVALID_ARG;
    }

    if (texture->format != GRAPE_PIXEL_FORMAT_A8) {
        texture->occupancy_occupied_count =
            (size_t)texture->occupancy_columns * texture->occupancy_rows;
        texture->occupancy_all_full = true;
        texture->occupancy_all_empty = false;
        return ESP_OK;
    }

    if (!texture->occupancy || texture->occupancy_bitmap_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(texture->occupancy, 0, texture->occupancy_bitmap_size);

    const uint32_t cell_size = CONFIG_GRAPE_TEXTURE_OCCUPANCY_CELL_SIZE;
    size_t occupied_count = 0;
    size_t cell_count = (size_t)texture->occupancy_columns * texture->occupancy_rows;

    for (uint32_t cell_y = 0; cell_y < texture->occupancy_rows; ++cell_y) {
        uint32_t y0 = cell_y * cell_size;
        uint32_t y1 = y0 + cell_size;
        if (y1 > texture->height) {
            y1 = texture->height;
        }

        for (uint32_t cell_x = 0; cell_x < texture->occupancy_columns; ++cell_x) {
            uint32_t x0 = cell_x * cell_size;
            uint32_t x1 = x0 + cell_size;
            if (x1 > texture->width) {
                x1 = texture->width;
            }

            bool occupied = false;
            for (uint32_t y = y0; y < y1 && !occupied; ++y) {
                const uint8_t *row = texture->pixels + (size_t)y * texture->stride;
                for (uint32_t x = x0; x < x1; ++x) {
                    if (row[x] != 0) {
                        occupied = true;
                        break;
                    }
                }
            }

            if (occupied) {
                occupancy_set(
                    texture->occupancy,
                    occupancy_index(texture, cell_x, cell_y)
                );
                occupied_count++;
            }
        }
    }

    texture->occupancy_occupied_count = occupied_count;
    texture->occupancy_all_empty = occupied_count == 0;
    texture->occupancy_all_full = occupied_count == cell_count;
    return ESP_OK;
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

    esp_err_t ret = texture_init_occupancy(texture);
    if (ret != ESP_OK) {
        heap_caps_free(texture->pixels);
        free(texture);
        return ret;
    }

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

    heap_caps_free(texture->occupancy);
    heap_caps_free(texture->pixels);
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

    grape_context_t *context = texture->context;

    if (texture->format == GRAPE_PIXEL_FORMAT_A8) {
        for (grape_surface_t *surface = context->surfaces; surface; surface = surface->next) {
            if (surface->texture == texture) {
                esp_err_t ret = grape_damage_add_surface_coverage(surface);
                if (ret != ESP_OK) {
                    return ret;
                }
            }
        }

        esp_err_t ret = grape_texture_rebuild_occupancy(texture);
        if (ret != ESP_OK) {
            return ret;
        }

        for (grape_surface_t *surface = context->surfaces; surface; surface = surface->next) {
            if (surface->texture == texture) {
                ret = grape_damage_add_surface_coverage(surface);
                if (ret != ESP_OK) {
                    return ret;
                }
            }
        }

        return ESP_OK;
    }

    for (grape_surface_t *surface = context->surfaces; surface; surface = surface->next) {
        if (surface->texture == texture) {
            esp_err_t ret = grape_damage_add_surface_coverage(surface);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    return ESP_OK;
}
