#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "grape_internal.h"
#include "grape/grape_benchmark_hooks.h"

/**
 * Returns screen bounds as a grape_rect_t
 *
 * @param context GRAPE context
 * @return Rect containing screen bounds
 */
static grape_rect_t screen_bounds(const grape_context_t *context)
{
    return (grape_rect_t){
        .x = 0,
        .y = 0,
        .width = (int32_t)context->display_info.width,
        .height = (int32_t)context->display_info.height,
    };
}

/**
 * Converts 2D damage tile coordinates into a linear tile index
 *
 * @param damage GRAPE damage state
 * @param x X coordinate in the damage bitmap
 * @param y Y coordinate in the damage bitmap
 * @return Returns the index
 */
static inline size_t tile_index(const grape_damage_state_t *damage,
                                uint32_t x,
                                uint32_t y)
{
    return (size_t)y * damage->tile_columns + x;
}

/**
 * Gets a bit in the bitmap byte array
 *
 * @param bitmap Bitmap byte array
 * @param index 1D index in the bitmap bits
 * @return Tile bit
 */
static inline bool tile_get(const uint8_t *bitmap, size_t index)
{
    return (bitmap[index >> 3U] & (uint8_t)(1U << (index & 7U))) != 0;
}

/**
 * Sets a bit in the bitmap byte array to 1
 *
 * @param bitmap Bitmap byte array
 * @param index 1D index in the bitmap bits
 */
static inline void tile_set(uint8_t *bitmap, size_t index)
{
    bitmap[index >> 3U] |= (uint8_t)(1U << (index & 7U));
}

/**
 * Checks whether a rect is empty
 *
 * @param rect Rect to check
 * @return Whether a rect is empty
 */
bool grape_rect_empty(grape_rect_t rect)
{
    return rect.width <= 0 || rect.height <= 0;
}

/**
 * Finds the intersection of two rects
 *
 * @param a First rect
 * @param b Second rect
 * @return Intersection rect
 */
grape_rect_t grape_rect_intersection(grape_rect_t a, grape_rect_t b)
{
    int32_t x0 = a.x > b.x ? a.x : b.x;
    int32_t y0 = a.y > b.y ? a.y : b.y;
    int32_t ax1 = a.x + a.width;
    int32_t ay1 = a.y + a.height;
    int32_t bx1 = b.x + b.width;
    int32_t by1 = b.y + b.height;
    int32_t x1 = ax1 < bx1 ? ax1 : bx1;
    int32_t y1 = ay1 < by1 ? ay1 : by1;

    if (x1 <= x0 || y1 <= y0) {
        return (grape_rect_t){0, 0, 0, 0};
    }

    return (grape_rect_t){x0, y0, x1 - x0, y1 - y0};
}

/**
 * Finds a rect that contains both rects inside of it
 *
 * @param a First rect
 * @param b Second rect
 * @return Bounds rect
 */
grape_rect_t grape_rect_union(grape_rect_t a, grape_rect_t b)
{
    if (grape_rect_empty(a)) {
        return b;
    }
    if (grape_rect_empty(b)) {
        return a;
    }

    int32_t x0 = a.x < b.x ? a.x : b.x;
    int32_t y0 = a.y < b.y ? a.y : b.y;
    int32_t ax1 = a.x + a.width;
    int32_t ay1 = a.y + a.height;
    int32_t bx1 = b.x + b.width;
    int32_t by1 = b.y + b.height;
    int32_t x1 = ax1 > bx1 ? ax1 : bx1;
    int32_t y1 = ay1 > by1 ? ay1 : by1;

    return (grape_rect_t){x0, y0, x1 - x0, y1 - y0};
}

/**
 * Checks if the rects touch or intersect
 *
 * @param a First rect
 * @param b Second rect
 * @return Whether the rects touch or intersect
 */
bool grape_rect_touches(grape_rect_t a, grape_rect_t b)
{
    if (grape_rect_empty(a) || grape_rect_empty(b)) {
        return false;
    }

    int32_t ax1 = a.x + a.width;
    int32_t ay1 = a.y + a.height;
    int32_t bx1 = b.x + b.width;
    int32_t by1 = b.y + b.height;

    return a.x <= bx1 && b.x <= ax1 && a.y <= by1 && b.y <= ay1;
}

/**
 * Finds the area of a rect
 *
 * @param rect
 * @return Area of the rect
 */
static int64_t rect_area(grape_rect_t rect)
{
    if (grape_rect_empty(rect)) {
        return 0;
    }

    return (int64_t)rect.width * (int64_t)rect.height;
}

static bool mark_rect(uint8_t *bitmap,
                      const grape_damage_state_t *damage,
                      grape_rect_t screen,
                      grape_rect_t rect)
{
    rect = grape_rect_intersection(rect, screen);
    if (grape_rect_empty(rect)) {
        return false;
    }

    const int32_t tile_size = CONFIG_GRAPE_DAMAGE_TILE_SIZE;
    uint32_t x0 = (uint32_t)(rect.x / tile_size);
    uint32_t y0 = (uint32_t)(rect.y / tile_size);
    uint32_t x1 = (uint32_t)((rect.x + rect.width - 1) / tile_size);
    uint32_t y1 = (uint32_t)((rect.y + rect.height - 1) / tile_size);

    if (x1 >= damage->tile_columns) {
        x1 = damage->tile_columns - 1U;
    }
    if (y1 >= damage->tile_rows) {
        y1 = damage->tile_rows - 1U;
    }

    for (uint32_t y = y0; y <= y1; ++y) {
        for (uint32_t x = x0; x <= x1; ++x) {
            tile_set(bitmap, tile_index(damage, x, y));
        }
    }

    return true;
}

static void bitmap_fill_all(const grape_damage_state_t *damage, uint8_t *bitmap)
{
    memset(bitmap, 0xFF, damage->bitmap_size);

    size_t tile_count = (size_t)damage->tile_columns * damage->tile_rows;
    unsigned used_bits = (unsigned)(tile_count & 7U);
    if (used_bits != 0U) {
        bitmap[damage->bitmap_size - 1U] &=
            (uint8_t)((1U << used_bits) - 1U);
    }
}

static void bitmap_or(uint8_t *destination,
                      const uint8_t *source,
                      size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        destination[i] |= source[i];
    }
}

static inline bool occupancy_any_bit_range(const uint8_t *bitmap,
                                           size_t first_bit,
                                           size_t last_bit)
{
    size_t first_byte = first_bit >> 3U;
    size_t last_byte = last_bit >> 3U;
    uint32_t first_offset = (uint32_t)(first_bit & 7U);
    uint32_t last_offset = (uint32_t)(last_bit & 7U);

    if (first_byte == last_byte) {
        uint8_t first_mask = (uint8_t)(0xFFU << first_offset);
        uint8_t last_mask = last_offset == 7U
            ? 0xFFU
            : (uint8_t)((1U << (last_offset + 1U)) - 1U);
        return (bitmap[first_byte] & first_mask & last_mask) != 0U;
    }

    if ((bitmap[first_byte] & (uint8_t)(0xFFU << first_offset)) != 0U) {
        return true;
    }

    for (size_t byte = first_byte + 1U; byte < last_byte; ++byte) {
        if (bitmap[byte] != 0U) {
            return true;
        }
    }

    uint8_t last_mask = last_offset == 7U
        ? 0xFFU
        : (uint8_t)((1U << (last_offset + 1U)) - 1U);
    return (bitmap[last_byte] & last_mask) != 0U;
}

static inline bool occupancy_cell_is_set(const grape_texture_t *texture,
                                         uint32_t x,
                                         uint32_t y)
{
    size_t index = (size_t)y * texture->occupancy_columns + x;
    return (texture->occupancy[index >> 3U] &
            (uint8_t)(1U << (index & 7U))) != 0U;
}

static bool mark_float_aabb(uint8_t *bitmap,
                            const grape_damage_state_t *damage,
                            const grape_context_t *context,
                            float min_x,
                            float min_y,
                            float max_x,
                            float max_y)
{
    const float screen_width = (float)context->display_info.width;
    const float screen_height = (float)context->display_info.height;

    if (max_x <= 0.0f || max_y <= 0.0f ||
        min_x >= screen_width || min_y >= screen_height ||
        max_x <= min_x || max_y <= min_y) {
        return false;
    }

    if (min_x < 0.0f) min_x = 0.0f;
    if (min_y < 0.0f) min_y = 0.0f;
    if (max_x > screen_width) max_x = screen_width;
    if (max_y > screen_height) max_y = screen_height;

    int32_t pixel_x0 = (int32_t)min_x;
    int32_t pixel_y0 = (int32_t)min_y;
    int32_t pixel_x1 = (int32_t)max_x;
    int32_t pixel_y1 = (int32_t)max_y;
    if ((float)pixel_x1 < max_x) {
        pixel_x1++;
    }
    if ((float)pixel_y1 < max_y) {
        pixel_y1++;
    }
    if (pixel_x1 <= pixel_x0 || pixel_y1 <= pixel_y0) {
        return false;
    }

    const int32_t tile_size = CONFIG_GRAPE_DAMAGE_TILE_SIZE;
    uint32_t x0 = (uint32_t)(pixel_x0 / tile_size);
    uint32_t y0 = (uint32_t)(pixel_y0 / tile_size);
    uint32_t x1 = (uint32_t)((pixel_x1 - 1) / tile_size);
    uint32_t y1 = (uint32_t)((pixel_y1 - 1) / tile_size);

    if (x0 >= damage->tile_columns) x0 = damage->tile_columns - 1U;
    if (y0 >= damage->tile_rows) y0 = damage->tile_rows - 1U;
    if (x1 >= damage->tile_columns) x1 = damage->tile_columns - 1U;
    if (y1 >= damage->tile_rows) y1 = damage->tile_rows - 1U;

    for (uint32_t y = y0; y <= y1; ++y) {
        for (uint32_t x = x0; x <= x1; ++x) {
            tile_set(bitmap, tile_index(damage, x, y));
        }
    }

    return true;
}

static bool parallelogram_intersects_rect(float quad_center_x,
                                          float quad_center_y,
                                          float edge_x_x,
                                          float edge_x_y,
                                          float edge_y_x,
                                          float edge_y_y,
                                          float rect_left,
                                          float rect_top,
                                          float rect_right,
                                          float rect_bottom)
{
    const float half_edge_x_x = 0.5f * edge_x_x;
    const float half_edge_x_y = 0.5f * edge_x_y;
    const float half_edge_y_x = 0.5f * edge_y_x;
    const float half_edge_y_y = 0.5f * edge_y_y;

    const float rect_center_x = 0.5f * (rect_left + rect_right);
    const float rect_center_y = 0.5f * (rect_top + rect_bottom);
    const float rect_half_x = 0.5f * (rect_right - rect_left);
    const float rect_half_y = 0.5f * (rect_bottom - rect_top);
    const float delta_x = rect_center_x - quad_center_x;
    const float delta_y = rect_center_y - quad_center_y;

    const float quad_radius_x = fabsf(half_edge_x_x) + fabsf(half_edge_y_x);
    if (fabsf(delta_x) > quad_radius_x + rect_half_x) {
        return false;
    }

    const float quad_radius_y = fabsf(half_edge_x_y) + fabsf(half_edge_y_y);
    if (fabsf(delta_y) > quad_radius_y + rect_half_y) {
        return false;
    }

    const float edge_x_normal_x = -edge_x_y;
    const float edge_x_normal_y = edge_x_x;
    const float delta_on_edge_x_normal =
        delta_x * edge_x_normal_x + delta_y * edge_x_normal_y;
    const float quad_radius_edge_x_normal = fabsf(
        half_edge_y_x * edge_x_normal_x +
        half_edge_y_y * edge_x_normal_y
    );
    const float rect_radius_edge_x_normal =
        rect_half_x * fabsf(edge_x_normal_x) +
        rect_half_y * fabsf(edge_x_normal_y);
    if (fabsf(delta_on_edge_x_normal) >
        quad_radius_edge_x_normal + rect_radius_edge_x_normal) {
        return false;
    }

    const float edge_y_normal_x = -edge_y_y;
    const float edge_y_normal_y = edge_y_x;
    const float delta_on_edge_y_normal =
        delta_x * edge_y_normal_x + delta_y * edge_y_normal_y;
    const float quad_radius_edge_y_normal = fabsf(
        half_edge_x_x * edge_y_normal_x +
        half_edge_x_y * edge_y_normal_y
    );
    const float rect_radius_edge_y_normal =
        rect_half_x * fabsf(edge_y_normal_x) +
        rect_half_y * fabsf(edge_y_normal_y);
    if (fabsf(delta_on_edge_y_normal) >
        quad_radius_edge_y_normal + rect_radius_edge_y_normal) {
        return false;
    }

    return true;
}

static bool occupancy_intersects_local_tile(const grape_texture_t *texture,
                                            uint32_t cell_x0,
                                            uint32_t cell_y0,
                                            uint32_t cell_x1,
                                            uint32_t cell_y1,
                                            float local_x00,
                                            float local_y00,
                                            float edge_x_local_x,
                                            float edge_x_local_y,
                                            float edge_y_local_x,
                                            float edge_y_local_y)
{
    const uint32_t cell_size = CONFIG_GRAPE_TEXTURE_OCCUPANCY_CELL_SIZE;
    const float quad_center_x = local_x00 +
        0.5f * (edge_x_local_x + edge_y_local_x);
    const float quad_center_y = local_y00 +
        0.5f * (edge_x_local_y + edge_y_local_y);

    for (uint32_t cell_y = cell_y0; cell_y <= cell_y1; ++cell_y) {
        size_t row_start = (size_t)cell_y * texture->occupancy_columns;
        if (!occupancy_any_bit_range(
                texture->occupancy,
                row_start + cell_x0,
                row_start + cell_x1)) {
            continue;
        }

        float cell_top = (float)(cell_y * cell_size);
        float cell_bottom = cell_top + (float)cell_size;
        if (cell_bottom > (float)texture->height) {
            cell_bottom = (float)texture->height;
        }

        for (uint32_t cell_x = cell_x0; cell_x <= cell_x1; ++cell_x) {
            if (!occupancy_cell_is_set(texture, cell_x, cell_y)) {
                continue;
            }

            float cell_left = (float)(cell_x * cell_size);
            float cell_right = cell_left + (float)cell_size;
            if (cell_right > (float)texture->width) {
                cell_right = (float)texture->width;
            }

            if (parallelogram_intersects_rect(
                    quad_center_x,
                    quad_center_y,
                    edge_x_local_x,
                    edge_x_local_y,
                    edge_y_local_x,
                    edge_y_local_y,
                    cell_left,
                    cell_top,
                    cell_right,
                    cell_bottom)) {
                return true;
            }
        }
    }

    return false;
}

static bool mark_partial_surface_tiles_screen(uint8_t *bitmap,
                                              const grape_damage_state_t *damage,
                                              const grape_context_t *context,
                                              const grape_surface_t *surface)
{
    grape_rect_t candidate_bounds = grape_rect_intersection(
        surface->bounds,
        screen_bounds(context)
    );
    if (grape_rect_empty(candidate_bounds)) {
        return false;
    }

    const int32_t tile_size = CONFIG_GRAPE_DAMAGE_TILE_SIZE;
    const uint32_t occupancy_cell_size = CONFIG_GRAPE_TEXTURE_OCCUPANCY_CELL_SIZE;
    const float texture_width = (float)surface->texture->width;
    const float texture_height = (float)surface->texture->height;

    uint32_t tile_x0 = (uint32_t)(candidate_bounds.x / tile_size);
    uint32_t tile_y0 = (uint32_t)(candidate_bounds.y / tile_size);
    uint32_t tile_x1 = (uint32_t)(
        (candidate_bounds.x + candidate_bounds.width - 1) / tile_size
    );
    uint32_t tile_y1 = (uint32_t)(
        (candidate_bounds.y + candidate_bounds.height - 1) / tile_size
    );

    if (tile_x1 >= damage->tile_columns) {
        tile_x1 = damage->tile_columns - 1U;
    }
    if (tile_y1 >= damage->tile_rows) {
        tile_y1 = damage->tile_rows - 1U;
    }

    const float screen_width = (float)context->display_info.width;
    const float screen_height = (float)context->display_info.height;
    bool marked = false;

    for (uint32_t tile_y = tile_y0; tile_y <= tile_y1; ++tile_y) {
        const float top = (float)(tile_y * (uint32_t)tile_size);
        float bottom = top + (float)tile_size;
        if (bottom > screen_height) {
            bottom = screen_height;
        }

        for (uint32_t tile_x = tile_x0; tile_x <= tile_x1; ++tile_x) {
            const float left = (float)(tile_x * (uint32_t)tile_size);
            float right = left + (float)tile_size;
            if (right > screen_width) {
                right = screen_width;
            }

            float local_x00 = surface->local_x_from_screen_x * left +
                              surface->local_x_from_screen_y * top +
                              surface->local_x_offset;
            float local_y00 = surface->local_y_from_screen_x * left +
                              surface->local_y_from_screen_y * top +
                              surface->local_y_offset;
            float edge_x_local_x =
                surface->local_x_from_screen_x * (right - left);
            float edge_x_local_y =
                surface->local_y_from_screen_x * (right - left);
            float edge_y_local_x =
                surface->local_x_from_screen_y * (bottom - top);
            float edge_y_local_y =
                surface->local_y_from_screen_y * (bottom - top);

            float local_min_x = local_x00;
            float local_max_x = local_x00;
            float local_min_y = local_y00;
            float local_max_y = local_y00;

            if (edge_x_local_x < 0.0f) {
                local_min_x += edge_x_local_x;
            } else {
                local_max_x += edge_x_local_x;
            }
            if (edge_y_local_x < 0.0f) {
                local_min_x += edge_y_local_x;
            } else {
                local_max_x += edge_y_local_x;
            }
            if (edge_x_local_y < 0.0f) {
                local_min_y += edge_x_local_y;
            } else {
                local_max_y += edge_x_local_y;
            }
            if (edge_y_local_y < 0.0f) {
                local_min_y += edge_y_local_y;
            } else {
                local_max_y += edge_y_local_y;
            }

            if (local_max_x < 0.0f || local_max_y < 0.0f ||
                local_min_x > texture_width ||
                local_min_y > texture_height) {
                continue;
            }

            if (local_min_x < 0.0f) local_min_x = 0.0f;
            if (local_min_y < 0.0f) local_min_y = 0.0f;
            if (local_max_x > texture_width) local_max_x = texture_width;
            if (local_max_y > texture_height) local_max_y = texture_height;

            if (local_max_x < local_min_x || local_max_y < local_min_y) {
                continue;
            }

            uint32_t cell_x0 = (uint32_t)(
                local_min_x / (float)occupancy_cell_size
            );
            uint32_t cell_y0 = (uint32_t)(
                local_min_y / (float)occupancy_cell_size
            );
            uint32_t cell_x1 = (uint32_t)(
                local_max_x / (float)occupancy_cell_size
            );
            uint32_t cell_y1 = (uint32_t)(
                local_max_y / (float)occupancy_cell_size
            );

            if (cell_x0 >= surface->texture->occupancy_columns) {
                cell_x0 = surface->texture->occupancy_columns - 1U;
            }
            if (cell_y0 >= surface->texture->occupancy_rows) {
                cell_y0 = surface->texture->occupancy_rows - 1U;
            }
            if (cell_x1 >= surface->texture->occupancy_columns) {
                cell_x1 = surface->texture->occupancy_columns - 1U;
            }
            if (cell_y1 >= surface->texture->occupancy_rows) {
                cell_y1 = surface->texture->occupancy_rows - 1U;
            }

            if (!occupancy_intersects_local_tile(
                    surface->texture,
                    cell_x0,
                    cell_y0,
                    cell_x1,
                    cell_y1,
                    local_x00,
                    local_y00,
                    edge_x_local_x,
                    edge_x_local_y,
                    edge_y_local_x,
                    edge_y_local_y)) {
                continue;
            }

            tile_set(bitmap, tile_index(damage, tile_x, tile_y));
            marked = true;
        }
    }

    return marked;
}

static bool mark_partial_surface_cells_source(uint8_t *bitmap,
                                              const grape_damage_state_t *damage,
                                              const grape_context_t *context,
                                              const grape_surface_t *surface)
{
    const grape_texture_t *texture = surface->texture;
    const uint32_t cell_size = CONFIG_GRAPE_TEXTURE_OCCUPANCY_CELL_SIZE;
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
    const float full_step_x_x = m00 * (float)cell_size;
    const float full_step_x_y = m10 * (float)cell_size;
    bool marked = false;

    for (uint32_t cell_y = 0; cell_y < texture->occupancy_rows; ++cell_y) {
        uint32_t local_y0 = cell_y * cell_size;
        uint32_t cell_height = texture->height - local_y0;
        if (cell_height > cell_size) {
            cell_height = cell_size;
        }

        float edge_y_x = m01 * (float)cell_height;
        float edge_y_y = m11 * (float)cell_height;
        float p00_x = offset_x + m01 * (float)local_y0;
        float p00_y = offset_y + m11 * (float)local_y0;

        for (uint32_t cell_x = 0; cell_x < texture->occupancy_columns; ++cell_x) {
            uint32_t local_x0 = cell_x * cell_size;
            uint32_t cell_width = texture->width - local_x0;
            if (cell_width > cell_size) {
                cell_width = cell_size;
            }

            float edge_x_x = cell_width == cell_size
                ? full_step_x_x
                : m00 * (float)cell_width;
            float edge_x_y = cell_width == cell_size
                ? full_step_x_y
                : m10 * (float)cell_width;

            if (occupancy_cell_is_set(texture, cell_x, cell_y)) {
                float min_x = p00_x;
                float max_x = p00_x;
                float min_y = p00_y;
                float max_y = p00_y;

                if (edge_x_x < 0.0f) min_x += edge_x_x; else max_x += edge_x_x;
                if (edge_y_x < 0.0f) min_x += edge_y_x; else max_x += edge_y_x;
                if (edge_x_y < 0.0f) min_y += edge_x_y; else max_y += edge_x_y;
                if (edge_y_y < 0.0f) min_y += edge_y_y; else max_y += edge_y_y;

                if (mark_float_aabb(
                        bitmap,
                        damage,
                        context,
                        min_x,
                        min_y,
                        max_x,
                        max_y)) {
                    marked = true;
                }
            }

            p00_x += edge_x_x;
            p00_y += edge_x_y;
        }
    }

    return marked;
}

static bool partial_surface_prefers_screen_driven(
    const grape_surface_t *surface
)
{
    const grape_texture_t *texture = surface->texture;
    const size_t cell_count =
        (size_t)texture->occupancy_columns * texture->occupancy_rows;

    if (cell_count < 256U) {
        return false;
    }

    /*
     * Benchmarks show that source-driven marking is better for ordinary
     * partial textures, while the screen-driven path wins decisively only for
     * large, nearly full occupancy maps. Keep the selector intentionally
     * stable so transforms do not cause the marking strategy to oscillate.
     */
    const size_t empty_cells = cell_count - texture->occupancy_occupied_count;
    return empty_cells <= cell_count / 16U;
}

static bool mark_full_surface_quad(uint8_t *bitmap,
                                   const grape_damage_state_t *damage,
                                   const grape_context_t *context,
                                   const grape_surface_t *surface,
                                   float m00,
                                   float m01,
                                   float m10,
                                   float m11,
                                   float offset_x,
                                   float offset_y)
{
    const float width = (float)surface->texture->width;
    const float height = (float)surface->texture->height;
    const float edge_x_x = m00 * width;
    const float edge_x_y = m10 * width;
    const float edge_y_x = m01 * height;
    const float edge_y_y = m11 * height;

    float min_x = offset_x;
    float max_x = offset_x;
    float min_y = offset_y;
    float max_y = offset_y;

    if (edge_x_x < 0.0f) min_x += edge_x_x; else max_x += edge_x_x;
    if (edge_y_x < 0.0f) min_x += edge_y_x; else max_x += edge_y_x;
    if (edge_x_y < 0.0f) min_y += edge_x_y; else max_y += edge_x_y;
    if (edge_y_y < 0.0f) min_y += edge_y_y; else max_y += edge_y_y;

    const float screen_width = (float)context->display_info.width;
    const float screen_height = (float)context->display_info.height;
    if (max_x <= 0.0f || max_y <= 0.0f ||
        min_x >= screen_width || min_y >= screen_height ||
        max_x <= min_x || max_y <= min_y) {
        return false;
    }

    if (min_x < 0.0f) min_x = 0.0f;
    if (min_y < 0.0f) min_y = 0.0f;
    if (max_x > screen_width) max_x = screen_width;
    if (max_y > screen_height) max_y = screen_height;

    int32_t pixel_x0 = (int32_t)floorf(min_x);
    int32_t pixel_y0 = (int32_t)floorf(min_y);
    int32_t pixel_x1 = (int32_t)ceilf(max_x);
    int32_t pixel_y1 = (int32_t)ceilf(max_y);
    if (pixel_x1 <= pixel_x0 || pixel_y1 <= pixel_y0) {
        return false;
    }

    const int32_t tile_size = CONFIG_GRAPE_DAMAGE_TILE_SIZE;
    uint32_t tile_x0 = (uint32_t)(pixel_x0 / tile_size);
    uint32_t tile_y0 = (uint32_t)(pixel_y0 / tile_size);
    uint32_t tile_x1 = (uint32_t)((pixel_x1 - 1) / tile_size);
    uint32_t tile_y1 = (uint32_t)((pixel_y1 - 1) / tile_size);

    if (tile_x0 >= damage->tile_columns) tile_x0 = damage->tile_columns - 1U;
    if (tile_y0 >= damage->tile_rows) tile_y0 = damage->tile_rows - 1U;
    if (tile_x1 >= damage->tile_columns) tile_x1 = damage->tile_columns - 1U;
    if (tile_y1 >= damage->tile_rows) tile_y1 = damage->tile_rows - 1U;

    const float center_x = offset_x + 0.5f * (edge_x_x + edge_y_x);
    const float center_y = offset_y + 0.5f * (edge_x_y + edge_y_y);
    const float surface_half_x =
        0.5f * fabsf(surface->transform.scale_x) * width;
    const float surface_half_y =
        0.5f * fabsf(surface->transform.scale_y) * height;
    const float abs_cos = fabsf(surface->cos_rotation);
    const float abs_sin = fabsf(surface->sin_rotation);
    bool marked = false;
    for (uint32_t tile_y = tile_y0; tile_y <= tile_y1; ++tile_y) {
        const float tile_top = (float)(tile_y * (uint32_t)tile_size);
        float tile_bottom = tile_top + (float)tile_size;
        if (tile_bottom > screen_height) {
            tile_bottom = screen_height;
        }

        const float tile_center_y = 0.5f * (tile_top + tile_bottom);
        const float tile_half_y = 0.5f * (tile_bottom - tile_top);

        for (uint32_t tile_x = tile_x0; tile_x <= tile_x1; ++tile_x) {
            const float tile_left = (float)(tile_x * (uint32_t)tile_size);
            float tile_right = tile_left + (float)tile_size;
            if (tile_right > screen_width) {
                tile_right = screen_width;
            }

            const float tile_center_x = 0.5f * (tile_left + tile_right);
            const float tile_half_x = 0.5f * (tile_right - tile_left);
            const float delta_x = tile_center_x - center_x;
            const float delta_y = tile_center_y - center_y;

            const float along_surface_x =
                delta_x * surface->cos_rotation +
                delta_y * surface->sin_rotation;
            const float tile_radius_surface_x =
                tile_half_x * abs_cos + tile_half_y * abs_sin;
            if (fabsf(along_surface_x) >
                surface_half_x + tile_radius_surface_x) {
                continue;
            }

            const float along_surface_y =
                -delta_x * surface->sin_rotation +
                delta_y * surface->cos_rotation;
            const float tile_radius_surface_y =
                tile_half_x * abs_sin + tile_half_y * abs_cos;
            if (fabsf(along_surface_y) >
                surface_half_y + tile_radius_surface_y) {
                continue;
            }

            tile_set(bitmap, tile_index(damage, tile_x, tile_y));
            marked = true;
        }
    }

    return marked;
}

esp_err_t grape_damage_add_surface_coverage(grape_surface_t *surface)
{
    if (!surface || !surface->context || !surface->context->damage.tiles) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!surface->visible || surface->opacity == 0) {
        return ESP_OK;
    }

    if (surface->shader) {
        return grape_damage_add(surface->context, surface->bounds);
    }

    if (!surface->texture) {
        return ESP_ERR_INVALID_STATE;
    }

    if (surface->tint.a == 0) {
        return ESP_OK;
    }

    grape_texture_t *texture = surface->texture;
    if (texture->occupancy_all_empty) {
        return ESP_OK;
    }

    GRAPE_TIME_SCOPE(DAMAGE_MARK);

    if (texture->occupancy_all_full) {
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

        bool marked = mark_full_surface_quad(
            surface->context->damage.tiles,
            &surface->context->damage,
            surface->context,
            surface,
            m00,
            m01,
            m10,
            m11,
            offset_x,
            offset_y
        );

        if (marked) {
            surface->context->damage.has_damage = true;
        }

        return ESP_OK;
    }

    bool marked;
    if (partial_surface_prefers_screen_driven(surface)) {
        marked = mark_partial_surface_tiles_screen(
            surface->context->damage.tiles,
            &surface->context->damage,
            surface->context,
            surface
        );
    } else {
        marked = mark_partial_surface_cells_source(
            surface->context->damage.tiles,
            &surface->context->damage,
            surface->context,
            surface
        );
    }

    if (marked) {
        surface->context->damage.has_damage = true;
    }

    return ESP_OK;
}

static bool tile_region_empty(grape_damage_tile_region_t region)
{
    return region.x0 >= region.x1 || region.y0 >= region.y1;
}

static void tile_region_add_tile(grape_damage_tile_region_t *region,
                                 uint32_t x,
                                 uint32_t y)
{
    if (tile_region_empty(*region)) {
        *region = (grape_damage_tile_region_t){
            .x0 = x,
            .y0 = y,
            .x1 = x + 1U,
            .y1 = y + 1U,
        };
        return;
    }

    if (x < region->x0) region->x0 = x;
    if (y < region->y0) region->y0 = y;
    if (x + 1U > region->x1) region->x1 = x + 1U;
    if (y + 1U > region->y1) region->y1 = y + 1U;
}

static grape_damage_tile_region_t tile_region_union(grape_damage_tile_region_t a,
                                                     grape_damage_tile_region_t b)
{
    if (tile_region_empty(a)) return b;
    if (tile_region_empty(b)) return a;

    return (grape_damage_tile_region_t){
        .x0 = a.x0 < b.x0 ? a.x0 : b.x0,
        .y0 = a.y0 < b.y0 ? a.y0 : b.y0,
        .x1 = a.x1 > b.x1 ? a.x1 : b.x1,
        .y1 = a.y1 > b.y1 ? a.y1 : b.y1,
    };
}

static grape_rect_t tile_region_pixel_rect(const grape_context_t *context,
                                           grape_damage_tile_region_t region)
{
    if (tile_region_empty(region)) {
        return (grape_rect_t){0, 0, 0, 0};
    }

    const int32_t tile_size = CONFIG_GRAPE_DAMAGE_TILE_SIZE;
    int32_t x0 = (int32_t)region.x0 * tile_size;
    int32_t y0 = (int32_t)region.y0 * tile_size;
    int32_t x1 = (int32_t)region.x1 * tile_size;
    int32_t y1 = (int32_t)region.y1 * tile_size;

    if (x1 > (int32_t)context->display_info.width) {
        x1 = (int32_t)context->display_info.width;
    }
    if (y1 > (int32_t)context->display_info.height) {
        y1 = (int32_t)context->display_info.height;
    }

    return (grape_rect_t){
        .x = x0,
        .y = y0,
        .width = x1 - x0,
        .height = y1 - y0,
    };
}

static int64_t tile_region_pixel_area(const grape_context_t *context,
                                      grape_damage_tile_region_t region)
{
    return rect_area(tile_region_pixel_rect(context, region));
}

static uint32_t scan_dirty_root(const grape_damage_state_t *damage,
                                const uint8_t *bitmap,
                                grape_damage_tile_region_t *out_root)
{
    uint32_t dirty_count = 0;
    grape_damage_tile_region_t root = {0};

    for (uint32_t y = 0; y < damage->tile_rows; ++y) {
        for (uint32_t x = 0; x < damage->tile_columns; ++x) {
            if (!tile_get(bitmap, tile_index(damage, x, y))) {
                continue;
            }

            dirty_count++;
            tile_region_add_tile(&root, x, y);
        }
    }

    *out_root = root;
    return dirty_count;
}

static void build_axis_bounds(grape_damage_state_t *damage,
                              const uint8_t *bitmap,
                              grape_damage_tile_region_t bounds)
{
    size_t width = (size_t)(bounds.x1 - bounds.x0);
    size_t height = (size_t)(bounds.y1 - bounds.y0);

    memset(damage->column_bounds, 0, width * sizeof(damage->column_bounds[0]));
    memset(damage->row_bounds, 0, height * sizeof(damage->row_bounds[0]));

    for (uint32_t y = bounds.y0; y < bounds.y1; ++y) {
        for (uint32_t x = bounds.x0; x < bounds.x1; ++x) {
            if (!tile_get(bitmap, tile_index(damage, x, y))) {
                continue;
            }

            tile_region_add_tile(&damage->column_bounds[x - bounds.x0], x, y);
            tile_region_add_tile(&damage->row_bounds[y - bounds.y0], x, y);
        }
    }
}

static void consider_split(const grape_context_t *context,
                           grape_damage_split_region_t *region,
                           grape_damage_tile_region_t first,
                           grape_damage_tile_region_t second,
                           int64_t parent_cost)
{
    if (tile_region_empty(first) || tile_region_empty(second)) {
        return;
    }

    int64_t split_cost = tile_region_pixel_area(context, first) +
                         tile_region_pixel_area(context, second) +
                         2LL * (int64_t)CONFIG_GRAPE_DAMAGE_RECT_OVERHEAD_PIXELS;
    int64_t saving = parent_cost - split_cost;

    if (saving > region->split_saving) {
        region->split_saving = saving;
        region->split_a = first;
        region->split_b = second;
    }
}

static void consider_clean_split(const grape_context_t *context,
                                 grape_damage_split_region_t *region,
                                 grape_damage_tile_region_t first,
                                 grape_damage_tile_region_t second,
                                 int64_t parent_cost,
                                 uint64_t *best_balance)
{
    if (tile_region_empty(first) || tile_region_empty(second)) {
        return;
    }

    int64_t first_area = tile_region_pixel_area(context, first);
    int64_t second_area = tile_region_pixel_area(context, second);
    int64_t split_cost = first_area + second_area +
                         2LL * (int64_t)CONFIG_GRAPE_DAMAGE_RECT_OVERHEAD_PIXELS;
    int64_t saving = parent_cost - split_cost;
    uint64_t balance = first_area >= second_area
        ? (uint64_t)(first_area - second_area)
        : (uint64_t)(second_area - first_area);

    if (saving > region->split_saving ||
        (saving == region->split_saving && balance < *best_balance)) {
        region->split_saving = saving;
        region->split_a = first;
        region->split_b = second;
        *best_balance = balance;
    }
}

static bool axis_bounds_span_region(const grape_damage_tile_region_t *axis_bounds,
                                    size_t length,
                                    grape_damage_tile_region_t bounds,
                                    bool row_axis)
{
    for (size_t i = 0; i < length; ++i) {
        grape_damage_tile_region_t slice = axis_bounds[i];
        if (tile_region_empty(slice)) {
            continue;
        }

        if (row_axis) {
            if (slice.x0 != bounds.x0 || slice.x1 != bounds.x1) {
                return false;
            }
        } else if (slice.y0 != bounds.y0 || slice.y1 != bounds.y1) {
            return false;
        }
    }

    return true;
}

static void consider_clean_axis_splits(const grape_context_t *context,
                                       grape_damage_state_t *damage,
                                       const grape_damage_tile_region_t *axis_bounds,
                                       size_t length,
                                       grape_damage_split_region_t *region,
                                       int64_t parent_cost,
                                       uint32_t *candidate_count,
                                       uint64_t *best_balance)
{
    if (length <= 1U) {
        return;
    }

    damage->suffix_bounds[length] = (grape_damage_tile_region_t){0};
    for (size_t i = length; i-- > 0U;) {
        damage->suffix_bounds[i] = tile_region_union(
            axis_bounds[i],
            damage->suffix_bounds[i + 1U]
        );
    }

    grape_damage_tile_region_t prefix = {0};
    size_t i = 0;
    while (i < length) {
        if (!tile_region_empty(axis_bounds[i])) {
            prefix = tile_region_union(prefix, axis_bounds[i]);
            i++;
            continue;
        }

        size_t run_end = i + 1U;
        while (run_end < length && tile_region_empty(axis_bounds[run_end])) {
            run_end++;
        }

        if (!tile_region_empty(prefix) &&
            run_end < length &&
            !tile_region_empty(damage->suffix_bounds[run_end])) {
            (*candidate_count)++;
            consider_clean_split(
                context,
                region,
                prefix,
                damage->suffix_bounds[run_end],
                parent_cost,
                best_balance
            );
        }

        i = run_end;
    }
}

static void find_best_split(grape_context_t *context,
                            const uint8_t *bitmap,
                            grape_damage_split_region_t *region,
                            uint32_t *candidate_count)
{
    region->split_a = (grape_damage_tile_region_t){0};
    region->split_b = (grape_damage_tile_region_t){0};
    region->split_saving = INT64_MIN;

    grape_damage_tile_region_t bounds = region->bounds;
    size_t width = (size_t)(bounds.x1 - bounds.x0);
    size_t height = (size_t)(bounds.y1 - bounds.y0);
    if (width <= 1U && height <= 1U) {
        return;
    }

    grape_damage_state_t *damage = &context->damage;
    build_axis_bounds(damage, bitmap, bounds);

    int64_t parent_cost = tile_region_pixel_area(context, bounds) +
                          (int64_t)CONFIG_GRAPE_DAMAGE_RECT_OVERHEAD_PIXELS;
    uint64_t clean_balance = UINT64_MAX;

    if (axis_bounds_span_region(
            damage->column_bounds, width, bounds, false)) {
        consider_clean_axis_splits(
            context,
            damage,
            damage->column_bounds,
            width,
            region,
            parent_cost,
            candidate_count,
            &clean_balance
        );
    }
    if (axis_bounds_span_region(
            damage->row_bounds, height, bounds, true)) {
        consider_clean_axis_splits(
            context,
            damage,
            damage->row_bounds,
            height,
            region,
            parent_cost,
            candidate_count,
            &clean_balance
        );
    }

    if (region->split_saving > 0) {
        return;
    }

    region->split_a = (grape_damage_tile_region_t){0};
    region->split_b = (grape_damage_tile_region_t){0};
    region->split_saving = INT64_MIN;

    if (width > 1U) {
        damage->suffix_bounds[width] = (grape_damage_tile_region_t){0};
        for (size_t i = width; i-- > 0U;) {
            damage->suffix_bounds[i] = tile_region_union(
                damage->column_bounds[i],
                damage->suffix_bounds[i + 1U]
            );
        }

        grape_damage_tile_region_t prefix = {0};
        for (size_t cut = 1U; cut < width; ++cut) {
            prefix = tile_region_union(prefix, damage->column_bounds[cut - 1U]);
            (*candidate_count)++;
            consider_split(
                context,
                region,
                prefix,
                damage->suffix_bounds[cut],
                parent_cost
            );
        }
    }

    if (height > 1U) {
        damage->suffix_bounds[height] = (grape_damage_tile_region_t){0};
        for (size_t i = height; i-- > 0U;) {
            damage->suffix_bounds[i] = tile_region_union(
                damage->row_bounds[i],
                damage->suffix_bounds[i + 1U]
            );
        }

        grape_damage_tile_region_t prefix = {0};
        for (size_t cut = 1U; cut < height; ++cut) {
            prefix = tile_region_union(prefix, damage->row_bounds[cut - 1U]);
            (*candidate_count)++;
            consider_split(
                context,
                region,
                prefix,
                damage->suffix_bounds[cut],
                parent_cost
            );
        }
    }
}

static void use_full_screen(grape_context_t *context,
                            grape_rect_t *out_rects,
                            size_t *out_count)
{
    out_rects[0] = screen_bounds(context);
    *out_count = 1;
}

static esp_err_t build_rects(grape_context_t *context,
                             const uint8_t *bitmap,
                             grape_rect_t *out_rects,
                             size_t out_capacity,
                             size_t *out_count,
                             grape_debug_damage_stats_t *stats)
{
    if (!context || !bitmap || !out_rects || out_capacity == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    GRAPE_TIME_SCOPE(DAMAGE_PLAN);

    grape_damage_state_t *damage = &context->damage;
    grape_damage_tile_region_t root = {0};
    uint32_t dirty_tiles = scan_dirty_root(damage, bitmap, &root);
    uint64_t fullscreen_pixels = (uint64_t)context->display_info.width *
                                 (uint64_t)context->display_info.height;

    if (stats) {
        *stats = (grape_debug_damage_stats_t){
            .dirty_tiles = dirty_tiles,
            .total_tiles = damage->tile_columns * damage->tile_rows,
            .fullscreen_pixels = fullscreen_pixels,
        };
    }

    if (dirty_tiles == 0 || tile_region_empty(root)) {
        *out_count = 0;
        return ESP_OK;
    }

    size_t region_limit = out_capacity;
    if (region_limit > damage->split_region_capacity) {
        region_limit = damage->split_region_capacity;
    }
    if (region_limit == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t split_candidates = 0;
    uint32_t planner_splits = 0;
    size_t region_count = 1;
    damage->split_regions[0] = (grape_damage_split_region_t){
        .bounds = root,
        .split_saving = INT64_MIN,
    };

    if (region_limit > 1U) {
        find_best_split(
            context,
            bitmap,
            &damage->split_regions[0],
            &split_candidates
        );
    }

    while (region_count < region_limit) {
        size_t best_index = SIZE_MAX;
        int64_t best_saving = 0;

        for (size_t i = 0; i < region_count; ++i) {
            if (damage->split_regions[i].split_saving > best_saving) {
                best_saving = damage->split_regions[i].split_saving;
                best_index = i;
            }
        }

        if (best_index == SIZE_MAX) {
            break;
        }

        grape_damage_tile_region_t first = damage->split_regions[best_index].split_a;
        grape_damage_tile_region_t second = damage->split_regions[best_index].split_b;

        damage->split_regions[best_index] = (grape_damage_split_region_t){
            .bounds = first,
            .split_saving = INT64_MIN,
        };
        damage->split_regions[region_count] = (grape_damage_split_region_t){
            .bounds = second,
            .split_saving = INT64_MIN,
        };

        region_count++;
        planner_splits++;

        if (region_count < region_limit) {
            find_best_split(
                context,
                bitmap,
                &damage->split_regions[best_index],
                &split_candidates
            );
            find_best_split(
                context,
                bitmap,
                &damage->split_regions[region_count - 1U],
                &split_candidates
            );
        }
    }

    int64_t partial_cost = (int64_t)region_count *
                           (int64_t)CONFIG_GRAPE_DAMAGE_RECT_OVERHEAD_PIXELS;
    uint64_t final_pixels = 0;

    for (size_t i = 0; i < region_count; ++i) {
        grape_rect_t rect = tile_region_pixel_rect(
            context,
            damage->split_regions[i].bounds
        );
        out_rects[i] = rect;
        int64_t area = rect_area(rect);
        partial_cost += area;
        final_pixels += (uint64_t)area;
    }

    grape_rect_t screen = screen_bounds(context);
    int64_t full_cost = rect_area(screen) +
                        (int64_t)CONFIG_GRAPE_DAMAGE_RECT_OVERHEAD_PIXELS;

    if (full_cost <= partial_cost) {
        use_full_screen(context, out_rects, out_count);
        if (stats) {
            stats->planner_splits = planner_splits;
            stats->split_candidates = split_candidates;
            stats->final_rects = 1;
            stats->final_pixels = fullscreen_pixels;
            stats->full_screen = true;
        }
        return ESP_OK;
    }

    *out_count = region_count;
    if (stats) {
        stats->planner_splits = planner_splits;
        stats->split_candidates = split_candidates;
        stats->final_rects = (uint32_t)region_count;
        stats->final_pixels = final_pixels;
    }

    return ESP_OK;
}

esp_err_t grape_damage_init(grape_context_t *context)
{
    if (!context || context->display_info.width == 0 || context->display_info.height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_damage_state_t *damage = &context->damage;
    const size_t tile_size = CONFIG_GRAPE_DAMAGE_TILE_SIZE;

    size_t columns = ((size_t)context->display_info.width + tile_size - 1U) / tile_size;
    size_t rows = ((size_t)context->display_info.height + tile_size - 1U) / tile_size;

    if (columns == 0 || rows == 0 ||
        columns > UINT32_MAX || rows > UINT32_MAX ||
        columns > SIZE_MAX / rows) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t tile_count = columns * rows;
    if (tile_count > SIZE_MAX - 7U) {
        return ESP_ERR_INVALID_SIZE;
    }

    damage->tile_columns = (uint32_t)columns;
    damage->tile_rows = (uint32_t)rows;
    damage->bitmap_size = (tile_count + 7U) / 8U;
    damage->split_region_capacity = CONFIG_GRAPE_MAX_DAMAGE_RECTS;
    damage->split_axis_capacity = columns > rows ? columns : rows;

    damage->tiles = calloc(1, damage->bitmap_size);
    damage->current_visible_tiles = calloc(1, damage->bitmap_size);
    damage->previous_visible_tiles = calloc(1, damage->bitmap_size);
    damage->render_tiles = calloc(1, damage->bitmap_size);
    damage->split_regions = calloc(
        damage->split_region_capacity,
        sizeof(damage->split_regions[0])
    );
    damage->column_bounds = calloc(columns, sizeof(damage->column_bounds[0]));
    damage->row_bounds = calloc(rows, sizeof(damage->row_bounds[0]));
    damage->suffix_bounds = calloc(
        damage->split_axis_capacity + 1U,
        sizeof(damage->suffix_bounds[0])
    );

    if (!damage->tiles ||
        !damage->current_visible_tiles ||
        !damage->previous_visible_tiles ||
        !damage->render_tiles ||
        !damage->split_regions ||
        !damage->column_bounds ||
        !damage->row_bounds ||
        !damage->suffix_bounds) {
        grape_damage_deinit(context);
        return ESP_ERR_NO_MEM;
    }

    damage->mark_timer_start_us = grape_telemetry_timer_cumulative_us(
        GRAPE_TELEMETRY_TIMER_DAMAGE_MARK
    );
    return ESP_OK;
}

void grape_damage_deinit(grape_context_t *context)
{
    if (!context) {
        return;
    }

    grape_damage_state_t *damage = &context->damage;
    free(damage->tiles);
    free(damage->current_visible_tiles);
    free(damage->previous_visible_tiles);
    free(damage->render_tiles);
    free(damage->split_regions);
    free(damage->column_bounds);
    free(damage->row_bounds);
    free(damage->suffix_bounds);
    *damage = (grape_damage_state_t){0};
}

esp_err_t grape_damage_add(grape_context_t *context, grape_rect_t rect)
{
    if (!context || !context->damage.tiles) {
        return ESP_ERR_INVALID_ARG;
    }

    GRAPE_TIME_SCOPE(DAMAGE_MARK);

    if (mark_rect(
            context->damage.tiles,
            &context->damage,
            screen_bounds(context),
            rect)) {
        context->damage.has_damage = true;
    }

    return ESP_OK;
}

void grape_damage_all(grape_context_t *context)
{
    if (!context || !context->damage.tiles) {
        return;
    }

    bitmap_fill_all(&context->damage, context->damage.tiles);
    context->damage.has_damage = true;
}

void grape_damage_clear(grape_context_t *context)
{
    if (!context || !context->damage.tiles) {
        return;
    }

    memset(context->damage.tiles, 0, context->damage.bitmap_size);
    memset(context->damage.current_visible_tiles, 0, context->damage.bitmap_size);
    memset(context->damage.render_tiles, 0, context->damage.bitmap_size);
    context->damage.has_damage = false;
    context->damage.final_rect_count = 0;
    context->damage.mark_timer_start_us = grape_telemetry_timer_cumulative_us(
        GRAPE_TELEMETRY_TIMER_DAMAGE_MARK
    );
}

esp_err_t grape_damage_build_logical_rects(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!context->damage.has_damage) {
        context->damage.final_rect_count = 0;
        return ESP_OK;
    }

    return build_rects(
        context,
        context->damage.tiles,
        context->damage.final_rects,
        CONFIG_GRAPE_MAX_DAMAGE_RECTS,
        &context->damage.final_rect_count,
        NULL
    );
}

esp_err_t grape_damage_prepare_visible(grape_context_t *context,
                                       const grape_rect_t *extra_rects,
                                       size_t extra_count)
{
    if (!context || !context->damage.tiles || !context->damage.current_visible_tiles ||
        (extra_count > 0 && !extra_rects)) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_damage_state_t *damage = &context->damage;
    memcpy(damage->current_visible_tiles, damage->tiles, damage->bitmap_size);

    grape_rect_t screen = screen_bounds(context);
    for (size_t i = 0; i < extra_count; ++i) {
        mark_rect(
            damage->current_visible_tiles,
            damage,
            screen,
            extra_rects[i]
        );
    }

    return ESP_OK;
}

esp_err_t grape_damage_build_render_rects(grape_context_t *context,
                                          bool force_full_redraw,
                                          grape_rect_t *out_rects,
                                          size_t out_capacity,
                                          size_t *out_count)
{
    if (!context || !out_rects || out_capacity == 0 || !out_count ||
        !context->damage.current_visible_tiles ||
        !context->damage.previous_visible_tiles ||
        !context->damage.render_tiles) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_damage_state_t *damage = &context->damage;
    uint64_t mark_total_us = grape_telemetry_timer_cumulative_us(
        GRAPE_TELEMETRY_TIMER_DAMAGE_MARK
    );
    uint64_t plan_total_before = grape_telemetry_timer_cumulative_us(
        GRAPE_TELEMETRY_TIMER_DAMAGE_PLAN
    );
    memcpy(damage->render_tiles, damage->current_visible_tiles, damage->bitmap_size);
    bitmap_or(
        damage->render_tiles,
        damage->previous_visible_tiles,
        damage->bitmap_size
    );

    if (force_full_redraw) {
        bitmap_fill_all(damage, damage->render_tiles);
    }

    esp_err_t ret = build_rects(
        context,
        damage->render_tiles,
        out_rects,
        out_capacity,
        out_count,
        &damage->latest_stats
    );

    uint64_t plan_total_after = grape_telemetry_timer_cumulative_us(
        GRAPE_TELEMETRY_TIMER_DAMAGE_PLAN
    );
    damage->latest_stats.mark_us = mark_total_us - damage->mark_timer_start_us;
    damage->latest_stats.plan_us = plan_total_after - plan_total_before;

    return ret;
}

void grape_damage_commit_visible(grape_context_t *context)
{
    if (!context || !context->damage.current_visible_tiles ||
        !context->damage.previous_visible_tiles) {
        return;
    }

    memcpy(
        context->damage.previous_visible_tiles,
        context->damage.current_visible_tiles,
        context->damage.bitmap_size
    );
}

esp_err_t grape_benchmark_damage_plan_bitmap(
    grape_context_t *context,
    const uint8_t *bitmap,
    size_t bitmap_size,
    grape_benchmark_damage_plan_result_t *out_result
)
{
    if (!context || !bitmap || !out_result ||
        bitmap_size != context->damage.bitmap_size) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_rect_t rects[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t rect_count = 0;
    grape_debug_damage_stats_t stats = {0};

    esp_err_t ret = build_rects(
        context,
        bitmap,
        rects,
        CONFIG_GRAPE_MAX_DAMAGE_RECTS,
        &rect_count,
        &stats
    );
    if (ret != ESP_OK) {
        return ret;
    }

    *out_result = (grape_benchmark_damage_plan_result_t) {
        .dirty_tiles = stats.dirty_tiles,
        .total_tiles = stats.total_tiles,
        .planner_splits = stats.planner_splits,
        .split_candidates = stats.split_candidates,
        .final_rects = stats.final_rects,
        .final_pixels = stats.final_pixels,
        .fullscreen_pixels = stats.fullscreen_pixels,
        .full_screen = stats.full_screen,
    };
    return ESP_OK;
}
