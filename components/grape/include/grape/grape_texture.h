#pragma once

#include "grape/grape_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_context grape_context_t;
typedef struct grape_texture grape_texture_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    grape_pixel_format_t format;
    grape_memory_t memory;
} grape_texture_desc_t;

esp_err_t grape_texture_create(grape_context_t *context, const grape_texture_desc_t *desc, grape_texture_t **out_texture);
esp_err_t grape_texture_destroy(grape_texture_t *texture);
void *grape_texture_pixels(grape_texture_t *texture);
const void *grape_texture_pixels_const(const grape_texture_t *texture);
size_t grape_texture_stride(const grape_texture_t *texture);
uint32_t grape_texture_width(const grape_texture_t *texture);
uint32_t grape_texture_height(const grape_texture_t *texture);
grape_pixel_format_t grape_texture_format(const grape_texture_t *texture);
esp_err_t grape_texture_invalidate(grape_texture_t *texture);

#ifdef __cplusplus
}
#endif
