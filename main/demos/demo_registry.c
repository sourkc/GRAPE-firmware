#include "demo_internal.h"

#include <inttypes.h>

#include "esp_log.h"

static const char *TAG = "GRAPE_DEMO";

static const grape_demo_t s_demos[] = {
    {
        .id = GRAPE_DEMO_GPU_PPA_TRIANGLE,
        .name = "gpu_ppa_triangle",
        .configure = NULL,
        .run = grape_demo_gpu_ppa_triangle_run,
    },
    {
        .id = GRAPE_DEMO_MOVING_SQUARES,
        .name = "moving_squares",
        .configure = NULL,
        .run = grape_demo_moving_squares_run,
    },
    {
        .id = GRAPE_DEMO_SHADER_RAYMARCH,
        .name = "shader_raymarch",
        .configure = NULL,
        .run = grape_demo_shader_raymarch_run,
    },
    {
        .id = GRAPE_DEMO_FONT,
        .name = "font",
        .configure = grape_demo_font_configure,
        .run = grape_demo_font_run,
    },
    {
        .id = GRAPE_DEMO_VECTOR_SVG,
        .name = "vector_svg",
        .configure = NULL,
        .run = grape_demo_vector_svg_run,
    },
    {
        .id = GRAPE_DEMO_GPU_3D_TRIANGLE,
        .name = "gpu_3d_triangle",
        .configure = NULL,
        .run = grape_demo_gpu_3d_triangle_run,
    },
    {
        .id = GRAPE_DEMO_SURFACE_FEATURES,
        .name = "surface_features",
        .configure = NULL,
        .run = grape_demo_surface_features_run,
    },
    {
        .id = GRAPE_DEMO_AA,
        .name = "aa",
        .configure = NULL,
        .run = grape_demo_aa_run,
    },
    {
        .id = GRAPE_DEMO_SCREENSHOT,
        .name = "screenshot",
        .configure = NULL,
        .run = grape_demo_screenshot_run,
    },
    {
        .id = GRAPE_DEMO_AA_ROTATE_CHECKER,
        .name = "aa_rotate_checker",
        .configure = NULL,
        .run = grape_demo_aa_rotate_checker_run,
    },
    {
        .id = GRAPE_DEMO_GPU_3D_CUBE,
        .name = "gpu_3d_cube",
        .configure = NULL,
        .run = grape_demo_gpu_3d_cube_run,
    },
    {
        .id = GRAPE_DEMO_GPU_3D_TEXTURED_CUBE,
        .name = "gpu_3d_textured_cube",
        .configure = NULL,
        .run = grape_demo_gpu_3d_textured_cube_run,
    },
};

size_t grape_demo_count(void)
{
    return sizeof(s_demos) / sizeof(s_demos[0]);
}

const grape_demo_t *grape_demo_at(size_t index)
{
    if (index >= grape_demo_count()) {
        return NULL;
    }
    return &s_demos[index];
}

const grape_demo_t *grape_demo_find(uint32_t id)
{
    for (size_t i = 0; i < grape_demo_count(); ++i) {
        if (s_demos[i].id == id) {
            return &s_demos[i];
        }
    }
    return NULL;
}

void grape_demo_log_available(void)
{
    ESP_LOGI(TAG, "Available demos:");
    for (size_t i = 0; i < grape_demo_count(); ++i) {
        ESP_LOGI(TAG, "  %" PRIu32 " - %s", s_demos[i].id, s_demos[i].name);
    }
}
