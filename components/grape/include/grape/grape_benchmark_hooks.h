#pragma once

/*
 * Internal benchmark hooks.
 *
 * These are intentionally not part of GRAPE's stable application API. They
 * expose deterministic entry points for the benchmark component so micro
 * benchmarks can isolate planner/compositor work without routing through the
 * normal frame pipeline.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "grape/grape.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t tile_columns;
    uint32_t tile_rows;
    uint32_t tile_size;
    size_t bitmap_size;
} grape_benchmark_damage_grid_info_t;

typedef struct {
    uint32_t dirty_tiles;
    uint32_t total_tiles;
    uint32_t planner_splits;
    uint32_t split_candidates;
    uint32_t final_rects;
    uint64_t final_pixels;
    uint64_t fullscreen_pixels;
    bool full_screen;
} grape_benchmark_damage_plan_result_t;

typedef struct {
    uint32_t columns;
    uint32_t rows;
    size_t bitmap_size;
    size_t occupied_cells;
    bool all_full;
    bool all_empty;
} grape_benchmark_occupancy_info_t;

typedef struct {
    uintptr_t function_address;
    uint64_t calls;
} grape_benchmark_function_profile_entry_t;

esp_err_t grape_benchmark_damage_grid_info(
    const grape_context_t *context,
    grape_benchmark_damage_grid_info_t *out_info
);

esp_err_t grape_benchmark_damage_plan_bitmap(
    grape_context_t *context,
    const uint8_t *bitmap,
    size_t bitmap_size,
    grape_benchmark_damage_plan_result_t *out_result
);

void grape_benchmark_damage_clear(grape_context_t *context);

esp_err_t grape_benchmark_texture_occupancy_info(
    const grape_texture_t *texture,
    grape_benchmark_occupancy_info_t *out_info
);

esp_err_t grape_benchmark_render_rects(
    grape_context_t *context,
    const grape_rect_t *rects,
    size_t rect_count,
    bool present
);

size_t grape_benchmark_shear_scratch_bytes(const grape_context_t *context);

bool grape_benchmark_function_profile_enabled(void);
size_t grape_benchmark_function_profile_capacity(void);
void grape_benchmark_function_profile_reset(void);
void grape_benchmark_function_profile_start(void);
void grape_benchmark_function_profile_stop(void);
size_t grape_benchmark_function_profile_snapshot(
    grape_benchmark_function_profile_entry_t *entries,
    size_t capacity,
    bool *out_overflow
);

#ifdef __cplusplus
}
#endif
