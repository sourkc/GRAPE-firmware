#include <math.h>

#include "grape/grape_text.h"

esp_err_t grape_text_build_codepoints_path(const grape_font_t *font,
                                            const uint32_t *codepoints,
                                            size_t codepoint_count,
                                            grape_path_t *path,
                                            grape_text_path_info_t *out_info)
{
    if (!font || (!codepoints && codepoint_count != 0U) || !path) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_info) {
        *out_info = (grape_text_path_info_t){0};
    }

    esp_err_t ret = grape_path_clear(path);
    if (ret != ESP_OK) {
        return ret;
    }

    float pen_x = 0.0f;
    for (size_t i = 0; i < codepoint_count; ++i) {
        uint16_t glyph_id = 0;
        ret = grape_font_get_glyph_id(font, codepoints[i], &glyph_id);
        if (ret != ESP_OK) {
            grape_path_clear(path);
            return ret;
        }

        grape_font_glyph_info_t glyph_info = {0};
        ret = grape_font_get_glyph_info(font, glyph_id, &glyph_info);
        if (ret != ESP_OK) {
            grape_path_clear(path);
            return ret;
        }
        if (glyph_info.kind == GRAPE_FONT_GLYPH_COMPOSITE) {
            grape_path_clear(path);
            return ESP_ERR_NOT_SUPPORTED;
        }

        if (glyph_info.kind == GRAPE_FONT_GLYPH_SIMPLE && glyph_info.contour_count > 0) {
            ret = grape_font_append_glyph_path(font, glyph_id, path, pen_x, 0.0f);
            if (ret != ESP_OK) {
                grape_path_clear(path);
                return ret;
            }
        }

        grape_font_glyph_metrics_t metrics = {0};
        ret = grape_font_get_glyph_metrics(font, glyph_id, &metrics);
        if (ret != ESP_OK) {
            grape_path_clear(path);
            return ret;
        }

        pen_x += (float)metrics.advance_width;
        if (!isfinite(pen_x)) {
            grape_path_clear(path);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (out_info) {
        out_info->glyph_count = codepoint_count;
        out_info->advance_width = pen_x;
    }
    return ESP_OK;
}
