#pragma once

#include "grape/grape_path.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_font grape_font_t;

typedef enum {
    GRAPE_FONT_GLYPH_EMPTY = 0,
    GRAPE_FONT_GLYPH_SIMPLE,
    GRAPE_FONT_GLYPH_COMPOSITE,
} grape_font_glyph_kind_t;

typedef struct {
    grape_font_glyph_kind_t kind;
    int16_t contour_count;
    int16_t x_min;
    int16_t y_min;
    int16_t x_max;
    int16_t y_max;
} grape_font_glyph_info_t;

esp_err_t grape_font_load_memory(const void *data, size_t size, grape_font_t **out_font);
esp_err_t grape_font_destroy(grape_font_t *font);
uint16_t grape_font_units_per_em(const grape_font_t *font);
uint16_t grape_font_glyph_count(const grape_font_t *font);
uint16_t grape_font_cmap_format(const grape_font_t *font);
esp_err_t grape_font_get_glyph_id(const grape_font_t *font,
                                  uint32_t codepoint,
                                  uint16_t *out_glyph_id);
esp_err_t grape_font_get_glyph_info(const grape_font_t *font,
                                    uint16_t glyph_id,
                                    grape_font_glyph_info_t *out_info);
esp_err_t grape_font_get_glyph_path(const grape_font_t *font,
                                    uint16_t glyph_id,
                                    grape_path_t *path);

#ifdef __cplusplus
}
#endif
