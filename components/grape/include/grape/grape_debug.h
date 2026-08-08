#pragma once

#include "grape/grape_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_context grape_context_t;

typedef enum {
    GRAPE_DEBUG_LAYER_DAMAGE_RECTS = 0,
    GRAPE_DEBUG_LAYER_COUNT,
} grape_debug_layer_t;

typedef struct {
    uint32_t dirty_tiles;
    uint32_t total_tiles;
    uint32_t planner_splits;
    uint32_t split_candidates;
    uint32_t final_rects;
    uint64_t final_pixels;
    uint64_t fullscreen_pixels;
    uint64_t mark_us;
    uint64_t plan_us;
    uint64_t refresh_wait_us;
    bool full_screen;
} grape_debug_damage_stats_t;

esp_err_t grape_debug_set_layer_enabled(grape_context_t *context,
                                        grape_debug_layer_t layer,
                                        bool enabled);
bool grape_debug_is_layer_enabled(const grape_context_t *context,
                                  grape_debug_layer_t layer);
esp_err_t grape_debug_get_damage_stats(const grape_context_t *context,
                                       grape_debug_damage_stats_t *out_stats);

#ifdef __cplusplus
}
#endif
