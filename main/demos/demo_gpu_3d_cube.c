#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "demo_internal.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "GRAPE_DEMO";

#define GPU_3D_CUBE_WIDTH 320U
#define GPU_3D_CUBE_HEIGHT 240U
#define GPU_3D_CUBE_SCALE 2.0f
#define GPU_3D_CUBE_NEAR 0.35f
#define GPU_3D_CUBE_FAR 20.0f
#define GPU_3D_CUBE_FOV_Y 1.1344640138f /* 65 degrees */

typedef struct {
    float x;
    float y;
    float z;
} gpu_3d_cube_vertex_t;

static grape_gpu_mat4_t mat4_identity(void)
{
    return (grape_gpu_mat4_t) {
        .m = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        },
    };
}

static grape_gpu_mat4_t mat4_multiply(grape_gpu_mat4_t a, grape_gpu_mat4_t b)
{
    grape_gpu_mat4_t result = {0};
    for (uint32_t row = 0U; row < 4U; ++row) {
        for (uint32_t column = 0U; column < 4U; ++column) {
            float value = 0.0f;
            for (uint32_t k = 0U; k < 4U; ++k) {
                value += a.m[row * 4U + k] * b.m[k * 4U + column];
            }
            result.m[row * 4U + column] = value;
        }
    }
    return result;
}

static grape_gpu_mat4_t mat4_rotation_x(float angle)
{
    const float c = cosf(angle);
    const float s = sinf(angle);
    grape_gpu_mat4_t result = mat4_identity();
    result.m[5] = c;
    result.m[6] = -s;
    result.m[9] = s;
    result.m[10] = c;
    return result;
}

static grape_gpu_mat4_t mat4_rotation_y(float angle)
{
    const float c = cosf(angle);
    const float s = sinf(angle);
    grape_gpu_mat4_t result = mat4_identity();
    result.m[0] = c;
    result.m[2] = s;
    result.m[8] = -s;
    result.m[10] = c;
    return result;
}

static grape_gpu_mat4_t mat4_translation(float x, float y, float z)
{
    grape_gpu_mat4_t result = mat4_identity();
    result.m[3] = x;
    result.m[7] = y;
    result.m[11] = z;
    return result;
}

static grape_gpu_mat4_t mat4_perspective_lh(float fov_y,
                                             float aspect,
                                             float near_z,
                                             float far_z)
{
    const float f = 1.0f / tanf(fov_y * 0.5f);
    const float depth_scale = far_z / (far_z - near_z);
    return (grape_gpu_mat4_t) {
        .m = {
            f / aspect, 0.0f, 0.0f, 0.0f,
            0.0f, f, 0.0f, 0.0f,
            0.0f, 0.0f, depth_scale, -near_z * depth_scale,
            0.0f, 0.0f, 1.0f, 0.0f,
        },
    };
}

esp_err_t grape_demo_gpu_3d_cube_run(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_texture_t *color_target = NULL;
    grape_surface_t *surface = NULL;
    grape_gpu_context_t *gpu = NULL;
    grape_gpu_depth_buffer_t *depth = NULL;
    grape_gpu_buffer_t *vertex_buffer = NULL;
    grape_gpu_buffer_t *index_buffer = NULL;
    grape_gpu_pipeline_t *pipeline = NULL;

    const grape_texture_desc_t texture_desc = {
        .width = GPU_3D_CUBE_WIDTH,
        .height = GPU_3D_CUBE_HEIGHT,
        .format = GRAPE_PIXEL_FORMAT_RGBA8888,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    esp_err_t ret = grape_texture_create(grape, &texture_desc, &color_target);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_surface_desc_t surface_desc = GRAPE_SURFACE_DESC_TEXTURE(color_target);
    surface_desc.texture_filter = GRAPE_TEXTURE_FILTER_NEAREST;
    surface_desc.transform.scale_x = GPU_3D_CUBE_SCALE;
    surface_desc.transform.scale_y = GPU_3D_CUBE_SCALE;
    surface_desc.transform.x = ((float)display->width -
                                (float)GPU_3D_CUBE_WIDTH * GPU_3D_CUBE_SCALE) * 0.5f;
    surface_desc.transform.y = ((float)display->height -
                                (float)GPU_3D_CUBE_HEIGHT * GPU_3D_CUBE_SCALE) * 0.5f;
    ret = grape_surface_create(grape, &surface_desc, &surface);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_gpu_context_create(grape, &gpu);
    if (ret != ESP_OK) {
        return ret;
    }

    const grape_gpu_depth_buffer_desc_t depth_desc = {
        .width = GPU_3D_CUBE_WIDTH,
        .height = GPU_3D_CUBE_HEIGHT,
        .format = GRAPE_GPU_DEPTH_D16,
        .memory = GRAPE_MEMORY_PSRAM,
        .sample_count = GRAPE_GPU_SAMPLE_COUNT_4,
    };
    ret = grape_gpu_depth_buffer_create(gpu, &depth_desc, &depth);
    if (ret != ESP_OK) {
        return ret;
    }

    static const gpu_3d_cube_vertex_t vertices[] = {
        { -0.8f, -0.8f, -0.8f },
        {  0.8f, -0.8f, -0.8f },
        {  0.8f,  0.8f, -0.8f },
        { -0.8f,  0.8f, -0.8f },
        { -0.8f, -0.8f,  0.8f },
        {  0.8f, -0.8f,  0.8f },
        {  0.8f,  0.8f,  0.8f },
        { -0.8f,  0.8f,  0.8f },
    };

    /* Six faces, two outward-wound triangles per face. */
    static const uint16_t indices[] = {
        0, 2, 1,  0, 3, 2, /* -Z */
        4, 5, 6,  4, 6, 7, /* +Z */
        0, 4, 7,  0, 7, 3, /* -X */
        1, 2, 6,  1, 6, 5, /* +X */
        0, 1, 5,  0, 5, 4, /* -Y */
        3, 7, 6,  3, 6, 2, /* +Y */
    };

    const grape_gpu_buffer_desc_t vertex_desc = {
        .size = sizeof(vertices),
        .usage = GRAPE_GPU_BUFFER_VERTEX,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    ret = grape_gpu_buffer_create(gpu, &vertex_desc, &vertex_buffer);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = grape_gpu_buffer_write(vertex_buffer, 0U, vertices, sizeof(vertices));
    if (ret != ESP_OK) {
        return ret;
    }

    const grape_gpu_buffer_desc_t index_desc = {
        .size = sizeof(indices),
        .usage = GRAPE_GPU_BUFFER_INDEX,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    ret = grape_gpu_buffer_create(gpu, &index_desc, &index_buffer);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = grape_gpu_buffer_write(index_buffer, 0U, indices, sizeof(indices));
    if (ret != ESP_OK) {
        return ret;
    }

    const grape_gpu_pipeline_desc_t pipeline_desc = {
        .vertex_layout = {
            .stride = sizeof(gpu_3d_cube_vertex_t),
            .attribute_count = 1U,
            .attributes = {
                {
                    .location = 0U,
                    .format = GRAPE_GPU_VERTEX_FORMAT_F32X3,
                    .offset = 0U,
                },
            },
        },
        .topology = GRAPE_GPU_TOPOLOGY_TRIANGLE_LIST,
        .vertex_program = GRAPE_GPU_VERTEX_PROGRAM_MVP,
        .fragment_program = GRAPE_GPU_FRAGMENT_PROGRAM_PUSH_COLOR,
        .solid_color = { .r = 255, .g = 255, .b = 255, .a = 255 },
        .cull_mode = GRAPE_GPU_CULL_BACK,
        .front_face = GRAPE_GPU_FRONT_FACE_CW,
        .depth = {
            .test_enable = true,
            .write_enable = true,
            .compare_op = GRAPE_GPU_COMPARE_LESS,
        },
        .sample_count = GRAPE_GPU_SAMPLE_COUNT_4,
    };
    ret = grape_gpu_pipeline_create(gpu, &pipeline_desc, &pipeline);
    if (ret != ESP_OK) {
        return ret;
    }

    static const grape_color_t face_colors[6] = {
        { .r = 166, .g = 209, .b = 137, .a = 255 }, /* green */
        { .r = 239, .g = 159, .b = 118, .a = 255 }, /* peach */
        { .r = 140, .g = 170, .b = 238, .a = 255 }, /* blue */
        { .r = 202, .g = 158, .b = 230, .a = 255 }, /* mauve */
        { .r = 231, .g = 130, .b = 132, .a = 255 }, /* red */
        { .r = 229, .g = 200, .b = 144, .a = 255 }, /* yellow */
    };

    const grape_gpu_mat4_t projection = mat4_perspective_lh(
        GPU_3D_CUBE_FOV_Y,
        (float)GPU_3D_CUBE_WIDTH / (float)GPU_3D_CUBE_HEIGHT,
        GPU_3D_CUBE_NEAR,
        GPU_3D_CUBE_FAR
    );

    ESP_LOGI(TAG,
             "GPU 3D M2.5: indexed rotating cube, 4x MSAA, D16 per-sample depth, clipping, culling");
    ESP_LOGI(TAG,
             "render target=%ux%u near=%.2f far=%.1f; cube lightly intersects near plane",
             GPU_3D_CUBE_WIDTH,
             GPU_3D_CUBE_HEIGHT,
             GPU_3D_CUBE_NEAR,
             GPU_3D_CUBE_FAR);

    const int64_t start_us = esp_timer_get_time();
    int64_t fps_start_us = start_us;
    uint32_t frame_count = 0U;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        const float seconds = (float)(now_us - start_us) / 1000000.0f;
        const float z = 3.40f + 0.75f * sinf(seconds * 0.35f);

        const grape_gpu_mat4_t rotation = mat4_multiply(
            mat4_rotation_y(seconds * 0.70f),
            mat4_rotation_x(seconds * 0.93f)
        );
        const grape_gpu_mat4_t model = mat4_multiply(
            mat4_translation(0.0f, 0.0f, z),
            rotation
        );
        const grape_gpu_mat4_t mvp = mat4_multiply(projection, model);

        const grape_gpu_render_pass_desc_t pass = {
            .color_attachment = color_target,
            .color_load_op = GRAPE_GPU_LOAD_OP_CLEAR,
            .clear_color = { .r = 0, .g = 0, .b = 0, .a = 0 },
            .depth_attachment = depth,
            .depth_load_op = GRAPE_GPU_LOAD_OP_CLEAR,
            .clear_depth = 1.0f,
            .sample_count = GRAPE_GPU_SAMPLE_COUNT_4,
        };
        ret = grape_gpu_begin_render_pass(gpu, &pass);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = grape_gpu_set_push_constants(
            gpu,
            offsetof(grape_gpu_builtin_constants_t, mvp),
            &mvp,
            sizeof(mvp)
        );
        if (ret == ESP_OK) {
            ret = grape_gpu_bind_pipeline(gpu, pipeline);
        }
        if (ret == ESP_OK) {
            ret = grape_gpu_bind_vertex_buffer(gpu, vertex_buffer);
        }
        if (ret == ESP_OK) {
            ret = grape_gpu_bind_index_buffer(gpu, index_buffer, GRAPE_GPU_INDEX_U16);
        }

        for (uint32_t face = 0U; ret == ESP_OK && face < 6U; ++face) {
            ret = grape_gpu_set_push_constants(
                gpu,
                offsetof(grape_gpu_builtin_constants_t, color),
                &face_colors[face],
                sizeof(face_colors[face])
            );
            if (ret == ESP_OK) {
                ret = grape_gpu_draw_indexed(gpu, face * 6U, 6U, 0);
            }
        }

        const esp_err_t end_ret = grape_gpu_end_render_pass(gpu);
        if (ret != ESP_OK) {
            return ret;
        }
        if (end_ret != ESP_OK) {
            return end_ret;
        }

        ret = grape_present(grape);
        if (ret != ESP_OK) {
            return ret;
        }

        ++frame_count;
        const int64_t elapsed_us = esp_timer_get_time() - fps_start_us;
        if (elapsed_us >= 1000000) {
            const float elapsed_seconds = (float)elapsed_us / 1000000.0f;
            printf("GPU 3D M2.5 cube 4x MSAA FPS: %.2f\n", (float)frame_count / elapsed_seconds);
            frame_count = 0U;
            fps_start_us += elapsed_us;
        }
    }
}
