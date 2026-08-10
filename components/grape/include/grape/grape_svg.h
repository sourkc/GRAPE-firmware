#pragma once

#include "grape/grape_path.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_svg_document grape_svg_document_t;

typedef struct {
    float x;
    float y;
    float width;
    float height;
    uint8_t samples_per_axis;
    uint32_t padding_pixels;
    grape_memory_t memory;
    int32_t z_base;
} grape_svg_document_config_t;

#define GRAPE_SVG_DOCUMENT_CONFIG_DEFAULT() \
    {                                        \
        .x = 0.0f,                           \
        .y = 0.0f,                           \
        .width = 0.0f,                       \
        .height = 0.0f,                      \
        .samples_per_axis = 4,               \
        .padding_pixels = 1,                 \
        .memory = GRAPE_MEMORY_DEFAULT,      \
        .z_base = 0,                         \
    }

typedef struct {
    float min_x;
    float min_y;
    float width;
    float height;
} grape_svg_view_box_t;
esp_err_t grape_svg_parse_path_data(grape_path_t *path, const char *data);
esp_err_t grape_svg_document_create(grape_context_t *context,
                                    const char *svg_text,
                                    const grape_svg_document_config_t *config,
                                    grape_svg_document_t **out_document);
esp_err_t grape_svg_document_destroy(grape_svg_document_t *document);
size_t grape_svg_document_layer_count(const grape_svg_document_t *document);
const grape_svg_view_box_t *grape_svg_document_view_box(const grape_svg_document_t *document);

#ifdef __cplusplus
}
#endif
