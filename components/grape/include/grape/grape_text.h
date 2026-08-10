#pragma once

#include "grape/grape_glyph_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t glyph_count;
    float advance_width;
} grape_text_path_info_t;

typedef struct {
    size_t glyph_count;
    float advance_width;
    grape_path_bounds_t bounds;
    bool has_bounds;
} grape_text_measurement_t;

typedef struct {
    grape_texture_t *texture;
    float text_origin_x;
    float text_origin_y;
    float pixels_per_unit;
    float advance_width;
    size_t glyph_count;
} grape_text_raster_t;

esp_err_t grape_text_measure_codepoints(const grape_font_t *font,
                                        const uint32_t *codepoints,
                                        size_t codepoint_count,
                                        grape_text_measurement_t *out_measurement);
esp_err_t grape_text_build_codepoints_path(const grape_font_t *font,
                                            const uint32_t *codepoints,
                                            size_t codepoint_count,
                                            grape_path_t *path,
                                            grape_text_path_info_t *out_info);
esp_err_t grape_text_rasterize_codepoints_a8(
    grape_glyph_cache_t *cache,
    const grape_font_t *font,
    const uint32_t *codepoints,
    size_t codepoint_count,
    const grape_path_rasterize_config_t *config,
    grape_text_raster_t *out_raster);

#ifdef __cplusplus
}
#endif
