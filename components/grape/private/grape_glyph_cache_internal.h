#pragma once

#include "grape/grape_glyph_cache.h"

grape_context_t *grape_glyph_cache_context(grape_glyph_cache_t *cache);

typedef struct {
    grape_texture_t *texture;
    float path_origin_x;
    float path_origin_y;
    float pixels_per_unit;
    bool transient;
} grape_glyph_cache_raster_t;

esp_err_t grape_glyph_cache_acquire(grape_glyph_cache_t *cache,
                                    const grape_font_t *font,
                                    uint16_t glyph_id,
                                    float pixels_per_unit,
                                    uint8_t samples_per_axis,
                                    grape_glyph_cache_raster_t *out_raster);
void grape_glyph_cache_release(grape_glyph_cache_t *cache,
                               grape_glyph_cache_raster_t *raster);
