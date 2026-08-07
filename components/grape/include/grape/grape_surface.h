#pragma once

#include "grape/grape_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_surface grape_surface_t;

esp_err_t grape_surface_create(grape_context_t *context, grape_texture_t *texture, grape_surface_t **out_surface);
esp_err_t grape_surface_destroy(grape_surface_t *surface);
esp_err_t grape_surface_set_texture(grape_surface_t *surface, grape_texture_t *texture);
esp_err_t grape_surface_set_transform(grape_surface_t *surface, const grape_transform_t *transform);
esp_err_t grape_surface_set_position(grape_surface_t *surface, float x, float y);
esp_err_t grape_surface_set_scale(grape_surface_t *surface, float scale_x, float scale_y);
esp_err_t grape_surface_set_rotation(grape_surface_t *surface, float radians);
esp_err_t grape_surface_set_origin(grape_surface_t *surface, float origin_x, float origin_y);
esp_err_t grape_surface_set_z(grape_surface_t *surface, int32_t z);
esp_err_t grape_surface_set_opacity(grape_surface_t *surface, uint8_t opacity);
esp_err_t grape_surface_set_tint(grape_surface_t *surface, grape_color_t tint);
esp_err_t grape_surface_set_visible(grape_surface_t *surface, bool visible);
const grape_transform_t *grape_surface_transform(const grape_surface_t *surface);
int32_t grape_surface_z(const grape_surface_t *surface);
uint8_t grape_surface_opacity(const grape_surface_t *surface);
grape_color_t grape_surface_tint(const grape_surface_t *surface);
bool grape_surface_visible(const grape_surface_t *surface);
grape_texture_t *grape_surface_texture(grape_surface_t *surface);

#ifdef __cplusplus
}
#endif
