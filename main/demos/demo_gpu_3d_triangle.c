#include <stdint.h>

#include "demo_internal.h"
#include "esp_log.h"

static const char *TAG = "GRAPE_DEMO";

#define GPU_3D_DEMO_WIDTH 320U
#define GPU_3D_DEMO_HEIGHT 240U
#define GPU_3D_DEMO_SCALE 2.0f

typedef struct {
    float x;
    float y;
} gpu_3d_demo_vertex_t;

static grape_texture_t *s_gpu_3d_demo_texture;
static grape_surface_t *s_gpu_3d_demo_surface;
static grape_gpu_context_t *s_gpu_3d_demo_context;
static grape_gpu_buffer_t *s_gpu_3d_demo_vertex_buffer;
static grape_gpu_pipeline_t *s_gpu_3d_demo_pipeline;

esp_err_t grape_demo_gpu_3d_triangle_run(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_texture_desc_t texture_desc = {
        .width = GPU_3D_DEMO_WIDTH,
        .height = GPU_3D_DEMO_HEIGHT,
        .format = GRAPE_PIXEL_FORMAT_RGBA8888,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    esp_err_t ret = grape_texture_create(grape, &texture_desc, &s_gpu_3d_demo_texture);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_surface_create(grape, s_gpu_3d_demo_texture, &s_gpu_3d_demo_surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_transform_t transform = GRAPE_TRANSFORM_DEFAULT();
    transform.scale_x = GPU_3D_DEMO_SCALE;
    transform.scale_y = GPU_3D_DEMO_SCALE;
    transform.x = ((float)display->width -
                   (float)GPU_3D_DEMO_WIDTH * GPU_3D_DEMO_SCALE) * 0.5f;
    transform.y = ((float)display->height -
                   (float)GPU_3D_DEMO_HEIGHT * GPU_3D_DEMO_SCALE) * 0.5f;
    ret = grape_surface_set_transform(s_gpu_3d_demo_surface, &transform);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_gpu_context_create(grape, &s_gpu_3d_demo_context);
    if (ret != ESP_OK) {
        return ret;
    }

    static const gpu_3d_demo_vertex_t vertices[] = {
        { -0.75f, -0.65f },
        {  0.75f, -0.65f },
        {  0.00f,  0.75f },
    };

    grape_gpu_buffer_desc_t buffer_desc = {
        .size = sizeof(vertices),
        .usage = GRAPE_GPU_BUFFER_VERTEX,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    ret = grape_gpu_buffer_create(
        s_gpu_3d_demo_context,
        &buffer_desc,
        &s_gpu_3d_demo_vertex_buffer
    );
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_gpu_buffer_write(
        s_gpu_3d_demo_vertex_buffer,
        0U,
        vertices,
        sizeof(vertices)
    );
    if (ret != ESP_OK) {
        return ret;
    }

    grape_gpu_pipeline_desc_t pipeline_desc = {
        .vertex_layout = {
            .stride = sizeof(gpu_3d_demo_vertex_t),
            .attribute_count = 1U,
            .attributes = {
                {
                    .location = 0U,
                    .format = GRAPE_GPU_VERTEX_FORMAT_F32X2,
                    .offset = 0U,
                },
            },
        },
        .topology = GRAPE_GPU_TOPOLOGY_TRIANGLE_LIST,
        .vertex_program = GRAPE_GPU_VERTEX_PROGRAM_CLIP_SPACE,
        .fragment_program = GRAPE_GPU_FRAGMENT_PROGRAM_SOLID_COLOR,
        .solid_color = { .r = 166, .g = 209, .b = 137, .a = 255 },
    };
    ret = grape_gpu_pipeline_create(
        s_gpu_3d_demo_context,
        &pipeline_desc,
        &s_gpu_3d_demo_pipeline
    );
    if (ret != ESP_OK) {
        return ret;
    }

    grape_gpu_render_pass_desc_t pass_desc = {
        .color_attachment = s_gpu_3d_demo_texture,
        .color_load_op = GRAPE_GPU_LOAD_OP_CLEAR,
        .clear_color = { .r = 0, .g = 0, .b = 0, .a = 0 },
    };
    ret = grape_gpu_begin_render_pass(s_gpu_3d_demo_context, &pass_desc);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_gpu_bind_pipeline(s_gpu_3d_demo_context, s_gpu_3d_demo_pipeline);
    if (ret == ESP_OK) {
        ret = grape_gpu_bind_vertex_buffer(
            s_gpu_3d_demo_context,
            s_gpu_3d_demo_vertex_buffer
        );
    }
    if (ret == ESP_OK) {
        ret = grape_gpu_draw(s_gpu_3d_demo_context, 0U, 3U);
    }

    esp_err_t end_ret = grape_gpu_end_render_pass(s_gpu_3d_demo_context);
    if (ret != ESP_OK) {
        return ret;
    }
    if (end_ret != ESP_OK) {
        return end_ret;
    }

    ESP_LOGI(TAG,
             "GPU 3D milestone 1: solid triangle, %ux%u RGBA8888 target",
             GPU_3D_DEMO_WIDTH,
             GPU_3D_DEMO_HEIGHT);
    return grape_present(grape);
}
