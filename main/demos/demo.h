#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "grape/grape.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GRAPE_DEMO_MOVING_SQUARES = 0,
    GRAPE_DEMO_SHADER_RAYMARCH = 1,
    GRAPE_DEMO_FONT = 2,
    GRAPE_DEMO_VECTOR_SVG = 3,
    GRAPE_DEMO_GPU_3D_TRIANGLE = 4,
    GRAPE_DEMO_SURFACE_FEATURES = 5,
    GRAPE_DEMO_AA = 6,
    GRAPE_DEMO_SCREENSHOT = 7,
    GRAPE_DEMO_AA_ROTATE_CHECKER = 8,
    GRAPE_DEMO_GPU_3D_CUBE = 9,
    GRAPE_DEMO_GPU_3D_TEXTURED_CUBE = 10,
    GRAPE_DEMO_GPU_PPA_TRIANGLE = 11,
} grape_demo_id_t;

typedef void (*grape_demo_configure_fn)(grape_config_t *config);
typedef esp_err_t (*grape_demo_run_fn)(grape_context_t *grape);

typedef struct {
    uint32_t id;
    const char *name;
    grape_demo_configure_fn configure;
    grape_demo_run_fn run;
} grape_demo_t;

size_t grape_demo_count(void);
const grape_demo_t *grape_demo_at(size_t index);
const grape_demo_t *grape_demo_find(uint32_t id);
void grape_demo_log_available(void);

#ifdef __cplusplus
}
#endif
