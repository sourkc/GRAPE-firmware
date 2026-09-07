#pragma once
#include "grape_gpu_internal.h"

void grape_gpu_ppa_feature_init(grape_context_t *grape);
bool grape_gpu_ppa_try_triangle(grape_gpu_context_t *context,
                                const grape_gpu_triangle_setup_t *setup,
                                grape_color_t color);
void grape_gpu_ppa_release(grape_gpu_context_t *context);

/* Internal bridge to the unchanged opaque, no-depth CPU rasterizer. */
esp_err_t grape_gpu_raster_color_block(grape_gpu_context_t *context,
    const grape_gpu_triangle_setup_t *setup, grape_color_t color);
