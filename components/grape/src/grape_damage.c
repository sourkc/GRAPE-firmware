#include <limits.h>
#include <stdlib.h>
#include <string.h>

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

static uint32_t count_dirty_tiles(const uint8_t *bitmap, size_t bitmap_size)
{
    uint32_t count = 0;

    for (size_t i = 0; i < bitmap_size; ++i) {
        count += (uint32_t)__builtin_popcount((unsigned)bitmap[i]);
    }

    return count;
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

static grape_rect_t tile_run_rect(const grape_context_t *context,
                                  uint32_t row,
                                  uint32_t start_column,
                                  uint32_t end_column)
{
    const int32_t tile_size = CONFIG_GRAPE_DAMAGE_TILE_SIZE;
    int32_t x0 = (int32_t)start_column * tile_size;
    int32_t y0 = (int32_t)row * tile_size;
    int32_t x1 = (int32_t)end_column * tile_size;
    int32_t y1 = y0 + tile_size;

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

static bool extract_rects(grape_context_t *context,
                          const uint8_t *bitmap,
                          size_t *out_count)
{
    grape_damage_state_t *damage = &context->damage;
    size_t rect_count = 0;
    size_t active_count = 0;
    size_t *active = damage->active_runs;
    size_t *next_active = damage->next_active_runs;

    for (uint32_t row = 0; row < damage->tile_rows; ++row) {
        size_t next_count = 0;
        uint32_t column = 0;

        while (column < damage->tile_columns) {
            while (column < damage->tile_columns &&
                   !tile_get(bitmap, tile_index(damage, column, row))) {
                column++;
            }

            if (column >= damage->tile_columns) {
                break;
            }

            uint32_t start = column;
            while (column < damage->tile_columns &&
                   tile_get(bitmap, tile_index(damage, column, row))) {
                column++;
            }

            grape_rect_t run = tile_run_rect(context, row, start, column);
            bool extended = false;

            for (size_t i = 0; i < active_count; ++i) {
                size_t index = active[i];
                grape_rect_t *candidate = &damage->work_rects[index];

                if (candidate->x == run.x &&
                    candidate->width == run.width &&
                    candidate->y + candidate->height == run.y) {
                    candidate->height += run.height;
                    next_active[next_count++] = index;
                    extended = true;
                    break;
                }
            }

            if (extended) {
                continue;
            }

            if (rect_count >= damage->work_rect_capacity) {
                *out_count = rect_count + 1U;
                return false;
            }

            damage->work_rects[rect_count] = run;
            next_active[next_count++] = rect_count;
            rect_count++;
        }

        size_t *swap = active;
        active = next_active;
        next_active = swap;
        active_count = next_count;
    }

    *out_count = rect_count;
    return true;
}

static int64_t merge_score(grape_rect_t a, grape_rect_t b)
{
    grape_rect_t merged = grape_rect_union(a, b);
    grape_rect_t overlap = grape_rect_intersection(a, b);
    int64_t union_area = rect_area(a) + rect_area(b) - rect_area(overlap);
    int64_t extra_area = rect_area(merged) - union_area;

    return (int64_t)CONFIG_GRAPE_DAMAGE_RECT_OVERHEAD_PIXELS - extra_area;
}

static void merge_best_pair(grape_rect_t *rects,
                            size_t *count,
                            size_t first,
                            size_t second)
{
    rects[first] = grape_rect_union(rects[first], rects[second]);
    rects[second] = rects[*count - 1U];
    (*count)--;
}

static void optimize_rects(grape_context_t *context,
                           size_t *rect_count)
{
    grape_damage_state_t *damage = &context->damage;
    size_t count = *rect_count;

    while (count > 1U) {
        size_t best_first = 0;
        size_t best_second = 1;
        int64_t best_score = INT64_MIN;

        for (size_t i = 0; i + 1U < count; ++i) {
            for (size_t j = i + 1U; j < count; ++j) {
                int64_t score = merge_score(damage->work_rects[i], damage->work_rects[j]);
                if (score > best_score) {
                    best_score = score;
                    best_first = i;
                    best_second = j;
                }
            }
        }

        if (count <= CONFIG_GRAPE_MAX_DAMAGE_RECTS && best_score <= 0) {
            break;
        }

        merge_best_pair(
            damage->work_rects,
            &count,
            best_first,
            best_second
        );
    }

    *rect_count = count;
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

    if (stats) {
        *stats = (grape_debug_damage_stats_t){
            .dirty_tiles = count_dirty_tiles(bitmap, context->damage.bitmap_size),
            .total_tiles = context->damage.tile_columns * context->damage.tile_rows,
            .fullscreen_pixels = (uint64_t)context->display_info.width *
                                 (uint64_t)context->display_info.height,
        };
    }

    size_t rect_count = 0;
    if (!extract_rects(context, bitmap, &rect_count)) {
        use_full_screen(context, out_rects, out_count);
        if (stats) {
            stats->initial_rects = (uint32_t)rect_count;
            stats->final_rects = 1;
            stats->final_pixels = stats->fullscreen_pixels;
            stats->full_screen = true;
        }
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_PLAN
        grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_PLAN,
                             grape_profile_timestamp() - profile_start_us);
#endif
        return ESP_OK;
    }

    if (stats) {
        stats->initial_rects = (uint32_t)rect_count;
    }

    if (rect_count == 0) {
        *out_count = 0;
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_PLAN
        grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_PLAN,
                             grape_profile_timestamp() - profile_start_us);
#endif
        return ESP_OK;
    }

    optimize_rects(context, &rect_count);

    int64_t partial_cost = (int64_t)rect_count *
                           (int64_t)CONFIG_GRAPE_DAMAGE_RECT_OVERHEAD_PIXELS;
    for (size_t i = 0; i < rect_count; ++i) {
        partial_cost += rect_area(context->damage.work_rects[i]);
    }

    grape_rect_t screen = screen_bounds(context);
    int64_t full_cost = rect_area(screen) +
                        (int64_t)CONFIG_GRAPE_DAMAGE_RECT_OVERHEAD_PIXELS;

    if (full_cost <= partial_cost) {
        use_full_screen(context, out_rects, out_count);
        if (stats) {
            stats->final_rects = 1;
            stats->final_pixels = stats->fullscreen_pixels;
            stats->full_screen = true;
        }
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_PLAN
        grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_PLAN,
                             grape_profile_timestamp() - profile_start_us);
#endif
        return ESP_OK;
    }

    if (rect_count > out_capacity) {
        use_full_screen(context, out_rects, out_count);
        if (stats) {
            stats->final_rects = 1;
            stats->final_pixels = stats->fullscreen_pixels;
            stats->full_screen = true;
        }
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_PLAN
        grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_PLAN,
                             grape_profile_timestamp() - profile_start_us);
#endif
        return ESP_OK;
    }

    memcpy(out_rects,
           context->damage.work_rects,
           rect_count * sizeof(out_rects[0]));
    *out_count = rect_count;

    if (stats) {
        stats->final_rects = (uint32_t)rect_count;
        for (size_t i = 0; i < rect_count; ++i) {
            stats->final_pixels += (uint64_t)rect_area(context->damage.work_rects[i]);
        }
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
    damage->work_rect_capacity = CONFIG_GRAPE_DAMAGE_MAX_WORK_RECTS;
    if (damage->work_rect_capacity < CONFIG_GRAPE_MAX_DAMAGE_RECTS) {
        damage->work_rect_capacity = CONFIG_GRAPE_MAX_DAMAGE_RECTS;
    }
    damage->active_run_capacity = columns;

    damage->tiles = calloc(1, damage->bitmap_size);
    damage->render_tiles = calloc(1, damage->bitmap_size);
    damage->work_rects = calloc(damage->work_rect_capacity, sizeof(damage->work_rects[0]));
    damage->active_runs = calloc(damage->active_run_capacity, sizeof(damage->active_runs[0]));
    damage->next_active_runs = calloc(damage->active_run_capacity, sizeof(damage->next_active_runs[0]));

    if (!damage->tiles ||
        !damage->render_tiles ||
        !damage->work_rects ||
        !damage->active_runs ||
        !damage->next_active_runs) {
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
    free(damage->work_rects);
    free(damage->active_runs);
    free(damage->next_active_runs);
    *damage = (grape_damage_state_t){0};
}

esp_err_t grape_damage_add(grape_context_t *context, grape_rect_t rect)
{
    if (!context || !context->damage.tiles) {
        return ESP_ERR_INVALID_ARG;
    }

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
}

esp_err_t grape_damage_build_logical_rects(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!context->damage.has_damage) {
        context->damage.final_rect_count = 0;
        context->damage.latest_stats = (grape_debug_damage_stats_t){
            .total_tiles = context->damage.tile_columns * context->damage.tile_rows,
            .fullscreen_pixels = (uint64_t)context->display_info.width *
                                 (uint64_t)context->display_info.height,
        };
        return ESP_OK;
    }

    return build_rects(
        context,
        context->damage.tiles,
        context->damage.final_rects,
        CONFIG_GRAPE_MAX_DAMAGE_RECTS,
        &context->damage.final_rect_count,
        &context->damage.latest_stats
    );
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
