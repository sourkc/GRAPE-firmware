#pragma once

#include "grape/grape_texture.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_path grape_path_t;

typedef struct {
    float min_x;
    float min_y;
    float max_x;
    float max_y;
} grape_path_bounds_t;

typedef struct {
    float pixels_per_unit;
    uint8_t samples_per_axis;
    uint32_t padding_pixels;
    grape_memory_t memory;
} grape_path_rasterize_config_t;

#define GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT() \
    {                                          \
        .pixels_per_unit = 1.0f,               \
        .samples_per_axis = 4,                 \
        .padding_pixels = 1,                   \
        .memory = GRAPE_MEMORY_DEFAULT,        \
    }

typedef struct {
    grape_texture_t *texture;
    float path_origin_x;
    float path_origin_y;
    float pixels_per_unit;
} grape_path_raster_t;

esp_err_t grape_path_create(grape_path_t **out_path);
esp_err_t grape_path_destroy(grape_path_t *path);
esp_err_t grape_path_clear(grape_path_t *path);
esp_err_t grape_path_move_to(grape_path_t *path, float x, float y);
esp_err_t grape_path_move_to_relative(grape_path_t *path, float dx, float dy);
esp_err_t grape_path_line_to(grape_path_t *path, float x, float y);
esp_err_t grape_path_line_to_relative(grape_path_t *path, float dx, float dy);
esp_err_t grape_path_horizontal_to(grape_path_t *path, float x);
esp_err_t grape_path_horizontal_to_relative(grape_path_t *path, float dx);
esp_err_t grape_path_vertical_to(grape_path_t *path, float y);
esp_err_t grape_path_vertical_to_relative(grape_path_t *path, float dy);
esp_err_t grape_path_quad_to(grape_path_t *path,
                             float control_x,
                             float control_y,
                             float x,
                             float y);
esp_err_t grape_path_quad_to_relative(grape_path_t *path,
                                      float control_dx,
                                      float control_dy,
                                      float dx,
                                      float dy);
esp_err_t grape_path_smooth_quad_to(grape_path_t *path, float x, float y);
esp_err_t grape_path_smooth_quad_to_relative(grape_path_t *path, float dx, float dy);
esp_err_t grape_path_cubic_to(grape_path_t *path,
                              float control1_x,
                              float control1_y,
                              float control2_x,
                              float control2_y,
                              float x,
                              float y);
esp_err_t grape_path_cubic_to_relative(grape_path_t *path,
                                       float control1_dx,
                                       float control1_dy,
                                       float control2_dx,
                                       float control2_dy,
                                       float dx,
                                       float dy);
esp_err_t grape_path_smooth_cubic_to(grape_path_t *path,
                                     float control2_x,
                                     float control2_y,
                                     float x,
                                     float y);
esp_err_t grape_path_smooth_cubic_to_relative(grape_path_t *path,
                                              float control2_dx,
                                              float control2_dy,
                                              float dx,
                                              float dy);
esp_err_t grape_path_close(grape_path_t *path);
esp_err_t grape_path_get_bounds(const grape_path_t *path, grape_path_bounds_t *out_bounds);
esp_err_t grape_path_rasterize_a8(grape_context_t *context,
                                   const grape_path_t *path,
                                   const grape_path_rasterize_config_t *config,
                                   grape_path_raster_t *out_raster);

#ifdef __cplusplus
}
#endif
