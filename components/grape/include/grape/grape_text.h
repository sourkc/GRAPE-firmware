#pragma once

#include "grape/grape_font.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t glyph_count;
    float advance_width;
} grape_text_path_info_t;

esp_err_t grape_text_build_codepoints_path(const grape_font_t *font,
                                            const uint32_t *codepoints,
                                            size_t codepoint_count,
                                            grape_path_t *path,
                                            grape_text_path_info_t *out_info);

#ifdef __cplusplus
}
#endif
