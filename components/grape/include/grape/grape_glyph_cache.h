#pragma once

#include "grape/grape_font.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_glyph_cache grape_glyph_cache_t;

typedef enum {
    GRAPE_GLYPH_CACHE_SCALE_CPU = 0,
    GRAPE_GLYPH_CACHE_SCALE_PPA,
} grape_glyph_cache_scale_backend_t;

typedef struct {
    size_t capacity_bytes;
    grape_memory_t memory;
    uint32_t raster_padding_pixels;
    float max_downscale_ratio;
    grape_glyph_cache_scale_backend_t scale_backend;
} grape_glyph_cache_config_t;

#define GRAPE_GLYPH_CACHE_CONFIG_DEFAULT()       \
    {                                             \
        .capacity_bytes = 4U * 1024U * 1024U,    \
        .memory = GRAPE_MEMORY_PSRAM,             \
        .raster_padding_pixels = 2U,              \
        .max_downscale_ratio = 2.0f,              \
        .scale_backend = GRAPE_GLYPH_CACHE_SCALE_CPU, \
    }

typedef struct {
    uint64_t requests;
    uint64_t exact_hits;
    uint64_t scaled_hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t uncached_rasters;
    uint64_t cpu_scales;
    size_t entry_count;
    size_t used_bytes;
    size_t capacity_bytes;
} grape_glyph_cache_stats_t;

esp_err_t grape_glyph_cache_create(grape_context_t *context,
                                   const grape_glyph_cache_config_t *config,
                                   grape_glyph_cache_t **out_cache);
esp_err_t grape_glyph_cache_destroy(grape_glyph_cache_t *cache);
esp_err_t grape_glyph_cache_clear(grape_glyph_cache_t *cache);
void grape_glyph_cache_get_stats(const grape_glyph_cache_t *cache,
                                 grape_glyph_cache_stats_t *out_stats);

#ifdef __cplusplus
}
#endif
