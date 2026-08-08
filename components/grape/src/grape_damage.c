#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "grape/grape_debug_config.h"

#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
#include "esp_timer.h"
#endif

#include "grape_internal.h"

static grape_rect_t screen_bounds(const grape_context_t *context)
{
    return (grape_rect_t){
        .x = 0,
        .y = 0,
        .width = (int32_t)context->display_info.width,
        .height = (int32_t)context->display_info.height,
    };
}

static inline size_t tile_index(const grape_damage_state_t *damage,
                                uint32_t x,
                                uint32_t y)
{
    return (size_t)y * damage->tile_columns + x;
}

static inline bool tile_get(const uint8_t *bitmap, size_t index)
{
    return (bitmap[index >> 3U] & (uint8_t)(1U << (index & 7U))) != 0;
}

static inline void tile_set(uint8_t *bitmap, size_t index)
{
    bitmap[index >> 3U] |= (uint8_t)(1U << (index & 7U));
}

bool grape_rect_empty(grape_rect_t rect)
{
    return rect.width <= 0 || rect.height <= 0;
}

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

static inline bool occupancy_get(const grape_texture_t *texture,
                                 uint32_t x,
                                 uint32_t y)
{
    if (texture->occupancy_all_full) {
        return true;
    }
    if (texture->occupancy_all_empty || !texture->occupancy) {
        return false;
    }

    size_t index = (size_t)y * texture->occupancy_columns + x;
    return (texture->occupancy[index >> 3U] &
            (uint8_t)(1U << (index & 7U))) != 0;
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

    if ((float)pixel_x1 < max_x) pixel_x1++;
    if ((float)pixel_y1 < max_y) pixel_y1++;

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

esp_err_t grape_damage_add_surface_coverage(grape_surface_t *surface)
{
    if (!surface || !surface->context || !surface->texture ||
        !surface->context->damage.tiles) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!surface->visible || surface->opacity == 0 || surface->tint.a == 0) {
        return ESP_OK;
    }

    grape_texture_t *texture = surface->texture;
    if (texture->occupancy_all_empty) {
        return ESP_OK;
    }

#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
    int64_t mark_start_us = esp_timer_get_time();
#endif

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_ADD
    int64_t profile_start_us = grape_profile_timestamp();
#endif

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

            if (occupancy_get(texture, cell_x, cell_y)) {
                float min_x = p00_x;
                float max_x = p00_x;
                float min_y = p00_y;
                float max_y = p00_y;

                if (edge_x_x < 0.0f) min_x += edge_x_x; else max_x += edge_x_x;
                if (edge_y_x < 0.0f) min_x += edge_y_x; else max_x += edge_y_x;
                if (edge_x_y < 0.0f) min_y += edge_x_y; else max_y += edge_x_y;
                if (edge_y_y < 0.0f) min_y += edge_y_y; else max_y += edge_y_y;

                if (mark_float_aabb(
                        surface->context->damage.tiles,
                        &surface->context->damage,
                        surface->context,
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

    if (marked) {
        surface->context->damage.has_damage = true;
    }

#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
    surface->context->damage.mark_us_current +=
        (uint64_t)(esp_timer_get_time() - mark_start_us);
#endif

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_ADD
    grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_ADD,
                         grape_profile_timestamp() - profile_start_us);
#endif
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

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_PLAN
    int64_t profile_start_us = grape_profile_timestamp();
#endif

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
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_PLAN
        grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_PLAN,
                             grape_profile_timestamp() - profile_start_us);
#endif
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
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_PLAN
        grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_PLAN,
                             grape_profile_timestamp() - profile_start_us);
#endif
        return ESP_OK;
    }

    *out_count = region_count;
    if (stats) {
        stats->planner_splits = planner_splits;
        stats->split_candidates = split_candidates;
        stats->final_rects = (uint32_t)region_count;
        stats->final_pixels = final_pixels;
    }

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_PLAN
    grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_PLAN,
                         grape_profile_timestamp() - profile_start_us);
#endif
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
        !damage->render_tiles ||
        !damage->split_regions ||
        !damage->column_bounds ||
        !damage->row_bounds ||
        !damage->suffix_bounds) {
        grape_damage_deinit(context);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void grape_damage_deinit(grape_context_t *context)
{
    if (!context) {
        return;
    }

    grape_damage_state_t *damage = &context->damage;
    free(damage->tiles);
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

#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
    int64_t mark_start_us = esp_timer_get_time();
#endif

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_ADD
    int64_t profile_start_us = grape_profile_timestamp();
#endif

    if (mark_rect(
            context->damage.tiles,
            &context->damage,
            screen_bounds(context),
            rect)) {
        context->damage.has_damage = true;
    }

#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
    context->damage.mark_us_current +=
        (uint64_t)(esp_timer_get_time() - mark_start_us);
#endif

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_ADD
    grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_ADD,
                         grape_profile_timestamp() - profile_start_us);
#endif
    return ESP_OK;
}

void grape_damage_all(grape_context_t *context)
{
    if (!context || !context->damage.tiles) {
        return;
    }

    memset(context->damage.tiles, 0xFF, context->damage.bitmap_size);
    context->damage.has_damage = true;

    size_t tile_count = (size_t)context->damage.tile_columns * context->damage.tile_rows;
    unsigned used_bits = (unsigned)(tile_count & 7U);
    if (used_bits != 0U) {
        context->damage.tiles[context->damage.bitmap_size - 1U] &=
            (uint8_t)((1U << used_bits) - 1U);
    }
}

void grape_damage_clear(grape_context_t *context)
{
    if (!context || !context->damage.tiles) {
        return;
    }

    memset(context->damage.tiles, 0, context->damage.bitmap_size);
    context->damage.has_damage = false;
    context->damage.final_rect_count = 0;
    context->damage.mark_us_current = 0;
}

esp_err_t grape_damage_build_logical_rects(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
    int64_t plan_start_us = esp_timer_get_time();
    uint64_t mark_us = context->damage.mark_us_current;
#endif

    esp_err_t ret = ESP_OK;
    if (!context->damage.has_damage) {
        context->damage.final_rect_count = 0;
        context->damage.latest_stats = (grape_debug_damage_stats_t){
            .total_tiles = context->damage.tile_columns * context->damage.tile_rows,
            .fullscreen_pixels = (uint64_t)context->display_info.width *
                                 (uint64_t)context->display_info.height,
        };
    } else {
        ret = build_rects(
            context,
            context->damage.tiles,
            context->damage.final_rects,
            CONFIG_GRAPE_MAX_DAMAGE_RECTS,
            &context->damage.final_rect_count,
            &context->damage.latest_stats
        );
    }

#if GRAPE_DAMAGE_DIAGNOSTICS_ENABLE
    context->damage.latest_stats.mark_us = mark_us;
    context->damage.latest_stats.plan_us =
        (uint64_t)(esp_timer_get_time() - plan_start_us);
#else
    context->damage.latest_stats.mark_us = 0;
    context->damage.latest_stats.plan_us = 0;
#endif
    return ret;
}

esp_err_t grape_damage_build_render_rects(grape_context_t *context,
                                          const grape_rect_t *extra_rects,
                                          size_t extra_count,
                                          grape_rect_t *out_rects,
                                          size_t out_capacity,
                                          size_t *out_count)
{
    if (!context || !out_rects || out_capacity == 0 || !out_count ||
        (extra_count > 0 && !extra_rects)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (extra_count == 0) {
        if (context->damage.final_rect_count > out_capacity) {
            return ESP_ERR_INVALID_SIZE;
        }

        memcpy(out_rects,
               context->damage.final_rects,
               context->damage.final_rect_count * sizeof(out_rects[0]));
        *out_count = context->damage.final_rect_count;
        return ESP_OK;
    }

    memcpy(context->damage.render_tiles,
           context->damage.tiles,
           context->damage.bitmap_size);

    grape_rect_t screen = screen_bounds(context);
    for (size_t i = 0; i < extra_count; ++i) {
        mark_rect(
            context->damage.render_tiles,
            &context->damage,
            screen,
            extra_rects[i]
        );
    }

    return build_rects(
        context,
        context->damage.render_tiles,
        out_rects,
        out_capacity,
        out_count,
        NULL
    );
}
