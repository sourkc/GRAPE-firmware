#pragma once

#include "demo.h"

esp_err_t grape_demo_moving_squares_run(grape_context_t *grape);
esp_err_t grape_demo_shader_raymarch_run(grape_context_t *grape);
void grape_demo_font_configure(grape_config_t *config);
esp_err_t grape_demo_font_run(grape_context_t *grape);
esp_err_t grape_demo_vector_svg_run(grape_context_t *grape);
esp_err_t grape_demo_gpu_3d_triangle_run(grape_context_t *grape);
esp_err_t grape_demo_surface_features_run(grape_context_t *grape);
