#pragma once

#include "grape/grape_surface.h"
#include "grape/grape_display.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GRAPE_ROTATION_BACKEND_AUTO = 0,
    GRAPE_ROTATION_BACKEND_AFFINE,
    GRAPE_ROTATION_BACKEND_THREE_SHEAR,
} grape_rotation_backend_t;

typedef enum {
    GRAPE_SHEAR_Y_BACKEND_DIRECT = 0,
    GRAPE_SHEAR_Y_BACKEND_PPA_ROTATE,
} grape_shear_y_backend_t;

typedef struct {
    const char *display_driver;
    grape_color_t background;
} grape_config_t;

#define GRAPE_CONFIG_DEFAULT()                          \
    {                                                   \
        .display_driver = NULL,                         \
        .background = { .r = 18, .g = 18, .b = 18, .a = 255 }, \
    }

esp_err_t grape_init(const grape_config_t *config, grape_context_t **out_context);
void grape_deinit(grape_context_t *context);
esp_err_t grape_present(grape_context_t *context);
esp_err_t grape_invalidate(grape_context_t *context, grape_rect_t rect);
esp_err_t grape_invalidate_all(grape_context_t *context);
esp_err_t grape_set_background(grape_context_t *context, grape_color_t color);
esp_err_t grape_set_rotation_backend(grape_context_t *context, grape_rotation_backend_t backend);
grape_rotation_backend_t grape_get_rotation_backend(const grape_context_t *context);
esp_err_t grape_set_shear_y_backend(grape_context_t *context, grape_shear_y_backend_t backend);
grape_shear_y_backend_t grape_get_shear_y_backend(const grape_context_t *context);
const grape_display_info_t *grape_get_display_info(const grape_context_t *context);

#ifdef __cplusplus
}
#endif
