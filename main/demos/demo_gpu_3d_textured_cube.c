#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "demo_internal.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "GRAPE_DEMO";

#define GPU_M3_WIDTH 320U
#define GPU_M3_HEIGHT 240U
#define GPU_M3_SCALE 2.0f
#define GPU_M3_NEAR 0.35f
#define GPU_M3_FAR 20.0f
#define GPU_M3_FOV_Y 1.1344640138f
#define GPU_M3_TEXTURE_SIZE 64U

typedef struct {
    float x;
    float y;
    float z;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
} gpu_m3_vertex_t;

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
            for (uint32_t k = 0U; k < 4U; ++k) {
                result.m[row * 4U + column] +=
                    a.m[row * 4U + k] * b.m[k * 4U + column];
            }
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

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3U) << 11U) |
                      ((uint16_t)(g >> 2U) << 5U) |
                      (uint16_t)(b >> 3U));
}

static void build_debug_texture(grape_texture_t *texture)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    const size_t stride = grape_texture_stride(texture);
    for (uint32_t y = 0U; y < GPU_M3_TEXTURE_SIZE; ++y) {
        uint16_t *row = (uint16_t *)(pixels + (size_t)y * stride);
        for (uint32_t x = 0U; x < GPU_M3_TEXTURE_SIZE; ++x) {
            const bool checker = (((x / 8U) ^ (y / 8U)) & 1U) != 0U;
            uint8_t r = checker ? 166U : 239U;
            uint8_t g = checker ? 209U : 159U;
            uint8_t b = checker ? 137U : 118U;

            /* Asymmetric markers make mirrored/affine UV bugs obvious. */
            if (x < 8U && y < 8U) {
                r = 231U; g = 130U; b = 132U;
            } else if (x >= 56U && y < 8U) {
                r = 140U; g = 170U; b = 238U;
            } else if (x < 8U && y >= 56U) {
                r = 229U; g = 200U; b = 144U;
            } else if (x >= 56U && y >= 56U) {
                r = 202U; g = 158U; b = 230U;
            }
            if (x == 31U || x == 32U || y == 31U || y == 32U) {
                r = 198U; g = 208U; b = 245U;
            }
            row[x] = pack_rgb565(r, g, b);
        }
    }
}

#define V(px, py, pz, tu, tv, cr, cg, cb) \
    { (px), (py), (pz), (tu), (tv), (cr), (cg), (cb), 1.0f }

esp_err_t grape_demo_gpu_3d_textured_cube_run(grape_context_t *grape)
{
    (void)TAG;
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_texture_t *color_target = NULL;
    grape_texture_t *texture = NULL;
    grape_surface_t *surface = NULL;
    grape_gpu_context_t *gpu = NULL;
    grape_gpu_depth_buffer_t *depth = NULL;
    grape_gpu_buffer_t *vertex_buffer = NULL;
    grape_gpu_buffer_t *index_buffer = NULL;
    grape_gpu_pipeline_t *pipeline = NULL;

    const grape_texture_desc_t target_desc = {
        .width = GPU_M3_WIDTH,
        .height = GPU_M3_HEIGHT,
        .format = GRAPE_PIXEL_FORMAT_RGBA8888,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    esp_err_t ret = grape_texture_create(grape, &target_desc, &color_target);
    if (ret != ESP_OK) return ret;

    const grape_texture_desc_t source_desc = {
        .width = GPU_M3_TEXTURE_SIZE,
        .height = GPU_M3_TEXTURE_SIZE,
        .format = GRAPE_PIXEL_FORMAT_RGB565,
        .memory = GRAPE_MEMORY_INTERNAL,
    };
    ret = grape_texture_create(grape, &source_desc, &texture);
    if (ret != ESP_OK) return ret;
    build_debug_texture(texture);
    ret = grape_texture_invalidate(texture);
    if (ret != ESP_OK) return ret;

    grape_surface_desc_t surface_desc = GRAPE_SURFACE_DESC_TEXTURE(color_target);
    surface_desc.texture_filter = GRAPE_TEXTURE_FILTER_NEAREST;
    surface_desc.transform.scale_x = GPU_M3_SCALE;
    surface_desc.transform.scale_y = GPU_M3_SCALE;
    surface_desc.transform.x = ((float)display->width - (float)GPU_M3_WIDTH * GPU_M3_SCALE) * 0.5f;
    surface_desc.transform.y = ((float)display->height - (float)GPU_M3_HEIGHT * GPU_M3_SCALE) * 0.5f;
    ret = grape_surface_create(grape, &surface_desc, &surface);
    if (ret != ESP_OK) return ret;

    ret = grape_gpu_context_create(grape, &gpu);
    if (ret != ESP_OK) return ret;

    const grape_gpu_depth_buffer_desc_t depth_desc = {
        .width = GPU_M3_WIDTH,
        .height = GPU_M3_HEIGHT,
        .format = GRAPE_GPU_DEPTH_D16,
        .memory = GRAPE_MEMORY_PSRAM,
        .sample_count = GRAPE_GPU_SAMPLE_COUNT_4,
    };
    ret = grape_gpu_depth_buffer_create(gpu, &depth_desc, &depth);
    if (ret != ESP_OK) return ret;

    static const gpu_m3_vertex_t vertices[] = {
        /* -Z */
        V(-0.8f,-0.8f,-0.8f, 0.0f,2.0f, 1.00f,0.95f,0.95f),
        V( 0.8f,-0.8f,-0.8f, 2.0f,2.0f, 0.85f,1.00f,0.95f),
        V( 0.8f, 0.8f,-0.8f, 2.0f,0.0f, 0.95f,0.95f,1.00f),
        V(-0.8f, 0.8f,-0.8f, 0.0f,0.0f, 1.00f,1.00f,1.00f),
        /* +Z */
        V(-0.8f,-0.8f, 0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
        V( 0.8f,-0.8f, 0.8f, 2.0f,2.0f, 0.90f,1.00f,0.90f),
        V( 0.8f, 0.8f, 0.8f, 2.0f,0.0f, 0.90f,0.95f,1.00f),
        V(-0.8f, 0.8f, 0.8f, 0.0f,0.0f, 1.00f,0.90f,0.95f),
        /* -X */
        V(-0.8f,-0.8f,-0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
        V(-0.8f,-0.8f, 0.8f, 2.0f,2.0f, 0.90f,1.00f,1.00f),
        V(-0.8f, 0.8f, 0.8f, 2.0f,0.0f, 1.00f,0.90f,1.00f),
        V(-0.8f, 0.8f,-0.8f, 0.0f,0.0f, 1.00f,1.00f,0.90f),
        /* +X */
        V( 0.8f,-0.8f,-0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
        V( 0.8f, 0.8f,-0.8f, 0.0f,0.0f, 0.90f,1.00f,1.00f),
        V( 0.8f, 0.8f, 0.8f, 2.0f,0.0f, 1.00f,0.90f,1.00f),
        V( 0.8f,-0.8f, 0.8f, 2.0f,2.0f, 1.00f,1.00f,0.90f),
        /* -Y */
        V(-0.8f,-0.8f,-0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
        V( 0.8f,-0.8f,-0.8f, 2.0f,2.0f, 0.95f,1.00f,0.90f),
        V( 0.8f,-0.8f, 0.8f, 2.0f,0.0f, 0.90f,0.95f,1.00f),
        V(-0.8f,-0.8f, 0.8f, 0.0f,0.0f, 1.00f,0.90f,0.95f),
        /* +Y */
        V(-0.8f, 0.8f,-0.8f, 0.0f,2.0f, 1.00f,1.00f,1.00f),
        V(-0.8f, 0.8f, 0.8f, 0.0f,0.0f, 0.90f,1.00f,0.95f),
        V( 0.8f, 0.8f, 0.8f, 2.0f,0.0f, 0.95f,0.90f,1.00f),
        V( 0.8f, 0.8f,-0.8f, 2.0f,2.0f, 1.00f,0.95f,0.90f),
    };
#undef V

    static const uint16_t indices[] = {
         0, 2, 1,   0, 3, 2,
         4, 5, 6,   4, 6, 7,
         8, 9,10,   8,10,11,
        12,13,14,  12,14,15,
        16,17,18,  16,18,19,
        20,21,22,  20,22,23,
    };

    const grape_gpu_buffer_desc_t vertex_desc = {
        .size = sizeof(vertices), .usage = GRAPE_GPU_BUFFER_VERTEX, .memory = GRAPE_MEMORY_DEFAULT,
    };
    ret = grape_gpu_buffer_create(gpu, &vertex_desc, &vertex_buffer);
    if (ret != ESP_OK) return ret;
    ret = grape_gpu_buffer_write(vertex_buffer, 0U, vertices, sizeof(vertices));
    if (ret != ESP_OK) return ret;

    const grape_gpu_buffer_desc_t index_desc = {
        .size = sizeof(indices), .usage = GRAPE_GPU_BUFFER_INDEX, .memory = GRAPE_MEMORY_DEFAULT,
    };
    ret = grape_gpu_buffer_create(gpu, &index_desc, &index_buffer);
    if (ret != ESP_OK) return ret;
    ret = grape_gpu_buffer_write(index_buffer, 0U, indices, sizeof(indices));
    if (ret != ESP_OK) return ret;

    const grape_gpu_pipeline_desc_t pipeline_desc = {
        .vertex_layout = {
            .stride = sizeof(gpu_m3_vertex_t),
            .attribute_count = 3U,
            .attributes = {
                { .location = 0U, .format = GRAPE_GPU_VERTEX_FORMAT_F32X3, .offset = offsetof(gpu_m3_vertex_t, x) },
                { .location = 1U, .format = GRAPE_GPU_VERTEX_FORMAT_F32X2, .offset = offsetof(gpu_m3_vertex_t, u) },
                { .location = 2U, .format = GRAPE_GPU_VERTEX_FORMAT_F32X4, .offset = offsetof(gpu_m3_vertex_t, r) },
            },
        },
        .topology = GRAPE_GPU_TOPOLOGY_TRIANGLE_LIST,
        .vertex_program = GRAPE_GPU_VERTEX_PROGRAM_MVP,
        .fragment_program = GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR,
        .solid_color = {255U,255U,255U,255U},
        .cull_mode = GRAPE_GPU_CULL_BACK,
        .front_face = GRAPE_GPU_FRONT_FACE_CW,
        .depth = { .test_enable = true, .write_enable = true, .compare_op = GRAPE_GPU_COMPARE_LESS },
        .sampler = {
            .filter = GRAPE_GPU_FILTER_LINEAR,
            .address_u = GRAPE_GPU_ADDRESS_REPEAT,
            .address_v = GRAPE_GPU_ADDRESS_REPEAT,
        },
        .sample_count = GRAPE_GPU_SAMPLE_COUNT_4,
    };
    ret = grape_gpu_pipeline_create(gpu, &pipeline_desc, &pipeline);
    if (ret != ESP_OK) return ret;

    const grape_gpu_mat4_t projection = mat4_perspective_lh(
        GPU_M3_FOV_Y,
        (float)GPU_M3_WIDTH / (float)GPU_M3_HEIGHT,
        GPU_M3_NEAR,
        GPU_M3_FAR
    );

    ESP_LOGI(TAG, "GPU 3D M3: perspective-correct textured cube, RGB565 linear/repeat, vertex color, 4x MSAA");

    const int64_t start_us = esp_timer_get_time();
    int64_t fps_start_us = start_us;
    uint32_t frame_count = 0U;

    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        const float seconds = (float)(now_us - start_us) / 1000000.0f;
        const float z = 3.40f + 0.65f * sinf(seconds * 0.31f);
        const grape_gpu_mat4_t rotation = mat4_multiply(
            mat4_rotation_y(seconds * 0.70f),
            mat4_rotation_x(seconds * 0.93f)
        );
        const grape_gpu_mat4_t model = mat4_multiply(mat4_translation(0.0f, 0.0f, z), rotation);
        const grape_gpu_mat4_t mvp = mat4_multiply(projection, model);

        const grape_gpu_render_pass_desc_t pass = {
            .color_attachment = color_target,
            .color_load_op = GRAPE_GPU_LOAD_OP_CLEAR,
            .clear_color = {0U,0U,0U,0U},
            .depth_attachment = depth,
            .depth_load_op = GRAPE_GPU_LOAD_OP_CLEAR,
            .clear_depth = 1.0f,
            .sample_count = GRAPE_GPU_SAMPLE_COUNT_4,
        };
        ret = grape_gpu_begin_render_pass(gpu, &pass);
        if (ret != ESP_OK) return ret;

        ret = grape_gpu_set_push_constants(
            gpu, offsetof(grape_gpu_builtin_constants_t, mvp), &mvp, sizeof(mvp)
        );
        if (ret == ESP_OK) ret = grape_gpu_bind_pipeline(gpu, pipeline);
        if (ret == ESP_OK) ret = grape_gpu_bind_vertex_buffer(gpu, vertex_buffer);
        if (ret == ESP_OK) ret = grape_gpu_bind_index_buffer(gpu, index_buffer, GRAPE_GPU_INDEX_U16);
        if (ret == ESP_OK) ret = grape_gpu_bind_texture(gpu, 0U, texture);
        if (ret == ESP_OK) ret = grape_gpu_draw_indexed(gpu, 0U, 36U, 0);

        const esp_err_t end_ret = grape_gpu_end_render_pass(gpu);
        if (ret != ESP_OK) return ret;
        if (end_ret != ESP_OK) return end_ret;

        ret = grape_present(grape);
        if (ret != ESP_OK) return ret;

        ++frame_count;
        const int64_t elapsed_us = esp_timer_get_time() - fps_start_us;
        if (elapsed_us >= 1000000) {
            const float elapsed_seconds = (float)elapsed_us / 1000000.0f;
            printf("GPU 3D M3 textured cube 4x MSAA FPS: %.2f\n", (float)frame_count / elapsed_seconds);
            frame_count = 0U;
            fps_start_us += elapsed_us;
        }
    }
}
