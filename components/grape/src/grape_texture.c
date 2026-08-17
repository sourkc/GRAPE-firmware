#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "grape_internal.h"

/**
 * Converts memory preference into ESP-IDF capability flags
 *
 * @param memory Memory type
 * @return Capability flags
 */
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

/**
 * Converts a 2D occupancy cell coordinate into a 1D index
 *
 * @param texture Texture
 * @param x Occupancy map X coordinate
 * @param y Occupancy map Y coordinate
 * @return Index of a given coordinate
 */
static inline size_t occupancy_index(const grape_texture_t *texture,
                                     uint32_t x,
                                     uint32_t y)
{
    return (size_t)y * texture->occupancy_columns + x;
}

/**
 * Sets a particular bit in an occupancy bitmap
 *
 * @param bitmap Occupancy bitmap
 * @param index Index in the bitmap
 */
static inline void occupancy_set(uint8_t *bitmap, size_t index)
{
    bitmap[index >> 3U] |= (uint8_t)(1U << (index & 7U));
}

/**
 * Returns whether a texture format has per-pixel alpha that can change
 * surface coverage.
 */
static inline bool texture_format_has_alpha(grape_pixel_format_t format)
{
    return format == GRAPE_PIXEL_FORMAT_A8 ||
           format == GRAPE_PIXEL_FORMAT_RGBA8888;
}

/**
 * Returns the alpha value of one texel in a format known to carry alpha.
 */
static inline uint8_t texture_alpha_at(const grape_texture_t *texture,
                                       const uint8_t *row,
                                       uint32_t x)
{
    if (texture->format == GRAPE_PIXEL_FORMAT_A8) {
        return row[x];
    }

    return row[(size_t)x * 4U + 3U];
}

/**
 * Initializes occupancy information for a texture
 *
 * @param texture Texture to initialize occupancy for
 * @return ESP_OK on success or an error code
 */
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
    if (!texture_format_has_alpha(texture->format)) {
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

/**
 * Rebuilds an occupancy map for a texture
 *
 * @param texture Texture to calculate the occupancy map for
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_texture_rebuild_occupancy(grape_texture_t *texture)
{
    if (!texture) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!texture_format_has_alpha(texture->format)) {
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
                    if (texture_alpha_at(texture, row, x) != 0U) {
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

/**
 *
 * @param context GRAPE context
 * @param desc Texture parameters
 * @param out_texture Returns the initialized texture
 * @return ESP_OK on success or an error code
 */
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

/**
 * Destroys a GRAPE texture
 *
 * @param texture Texture to destroy
 * @return ESP_OK on success or an error code
 */
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

/**
 * Returns the writable pixel data of a texture
 *
 * @param texture Texture to access
 * @return Pointer to the texture's pixel data, or NULL if texture is NULL
 */
void *grape_texture_pixels(grape_texture_t *texture)
{
    return texture ? texture->pixels : NULL;
}

/**
 * Returns the read-only pixel data of a texture
 *
 * @param texture Texture to access
 * @return Const pointer to the texture's pixel data, or NULL if texture is NULL
 */
const void *grape_texture_pixels_const(const grape_texture_t *texture)
{
    return texture ? texture->pixels : NULL;
}

/**
 * Returns the texture's stride in bytes
 *
 * @param texture Texture to access
 * @return Texture stride in bytes, or 0 if texture is NULL
 */
size_t grape_texture_stride(const grape_texture_t *texture)
{
    return texture ? texture->stride : 0;
}

/**
 * Returns the texture's width in pixels
 *
 * @param texture Texture to access
 * @return Texture width in pixels, or 0 if texture is NULL
 */
uint32_t grape_texture_width(const grape_texture_t *texture)
{
    return texture ? texture->width : 0;
}

/**
 * Returns the texture's height in pixels
 *
 * @param texture Texture to access
 * @return Texture height in pixels, or 0 if texture is NULL
 */
uint32_t grape_texture_height(const grape_texture_t *texture)
{
    return texture ? texture->height : 0;
}

/**
 * Returns the texture's pixel format
 *
 * @param texture Texture to access
 * @return Texture pixel format, or GRAPE_PIXEL_FORMAT_RGB565 if texture is NULL
 */
grape_pixel_format_t grape_texture_format(const grape_texture_t *texture)
{
    return texture ? texture->format : GRAPE_PIXEL_FORMAT_RGB565;
}

static grape_rect_t texture_rect_surface_bounds(const grape_surface_t *surface,
                                                uint32_t x,
                                                uint32_t y,
                                                uint32_t width,
                                                uint32_t height)
{
    const float m00 = surface->cos_rotation * surface->transform.scale_x;
    const float m01 = -surface->sin_rotation * surface->transform.scale_y;
    const float m10 = surface->sin_rotation * surface->transform.scale_x;
    const float m11 = surface->cos_rotation * surface->transform.scale_y;
    const float offset_x = surface->transform.x
                         - m00 * surface->transform.origin_x
                         - m01 * surface->transform.origin_y;
    const float offset_y = surface->transform.y
                         - m10 * surface->transform.origin_x
                         - m11 * surface->transform.origin_y;
    const float x0 = (float)x;
    const float y0 = (float)y;
    const float x1 = (float)(x + width);
    const float y1 = (float)(y + height);
    const float px[4] = {
        offset_x + m00 * x0 + m01 * y0,
        offset_x + m00 * x1 + m01 * y0,
        offset_x + m00 * x0 + m01 * y1,
        offset_x + m00 * x1 + m01 * y1,
    };
    const float py[4] = {
        offset_y + m10 * x0 + m11 * y0,
        offset_y + m10 * x1 + m11 * y0,
        offset_y + m10 * x0 + m11 * y1,
        offset_y + m10 * x1 + m11 * y1,
    };
    float min_x = px[0];
    float max_x = px[0];
    float min_y = py[0];
    float max_y = py[0];

    for (size_t i = 1U; i < 4U; ++i) {
        if (px[i] < min_x) min_x = px[i];
        if (px[i] > max_x) max_x = px[i];
        if (py[i] < min_y) min_y = py[i];
        if (py[i] > max_y) max_y = py[i];
    }

    int32_t ix0 = grape_floor_to_i32(min_x);
    int32_t iy0 = grape_floor_to_i32(min_y);
    int32_t ix1 = grape_floor_to_i32(max_x);
    int32_t iy1 = grape_floor_to_i32(max_y);
    if ((float)ix1 < max_x) ix1++;
    if ((float)iy1 < max_y) iy1++;

    return (grape_rect_t){
        .x = ix0,
        .y = iy0,
        .width = ix1 - ix0,
        .height = iy1 - iy0,
    };
}

esp_err_t grape_texture_invalidate_rect(grape_texture_t *texture,
                                        uint32_t x,
                                        uint32_t y,
                                        uint32_t width,
                                        uint32_t height)
{
    if (!texture || width == 0U || height == 0U ||
        x > texture->width || y > texture->height ||
        width > texture->width - x || height > texture->height - y) {
        return ESP_ERR_INVALID_ARG;
    }

    if (texture_format_has_alpha(texture->format)) {
        return grape_texture_invalidate(texture);
    }

    for (grape_surface_t *surface = texture->context->surfaces;
         surface;
         surface = surface->next) {
        if (surface->texture != texture || !surface->visible ||
            surface->opacity == 0U || surface->tint.a == 0U) {
            continue;
        }

        esp_err_t ret;
        if (surface->shader_count > 0U || !surface->texture_mapping_identity) {
            ret = grape_damage_add_surface_coverage(surface);
        } else {
            ret = grape_damage_add(
                texture->context,
                texture_rect_surface_bounds(surface, x, y, width, height)
            );
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

/**
 * Use this after changing the texture.
 *  1. Marks all surfaces that use this texture as damaged regions
 *  2. Rebuilds the occupancy map
 *
 * @param texture Texture to invalidate
 * @return
 */
esp_err_t grape_texture_invalidate(grape_texture_t *texture)
{
    if (!texture) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_context_t *context = texture->context;

    if (texture_format_has_alpha(texture->format)) {
        // WARNING: Both damage passes are required for alpha-bearing textures.
        // The old coverage must be damaged before rebuilding occupancy, then
        // the new coverage must be damaged after the rebuild.

        // Invalidate once for the OLD occupancy map
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

        // Invalidate a second time for the NEW occupancy map
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

    // Opaque textures only need one damage pass
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
