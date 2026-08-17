#include <math.h>
#include <stdint.h>

#include "demo_internal.h"
#include "esp_log.h"
#include "generated_procedural_gradient.h"

static const char *TAG = "GRAPE_DEMO";

#define AA_DEMO_TEXTURE_SIZE 96U
#define AA_DEMO_CHECKER_SIZE 32U

static grape_texture_t *s_a8_texture;
static grape_texture_t *s_rgba_texture;
static grape_texture_t *s_rgb565_texture;
static grape_surface_t *s_surfaces[10];
static size_t s_surface_count;

static uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r >> 3U) << 11U) |
                      ((uint16_t)(g >> 2U) << 5U) |
                      (uint16_t)(b >> 3U));
}

static void fill_a8_circle(grape_texture_t *texture)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    const size_t stride = grape_texture_stride(texture);
    const float center = (float)AA_DEMO_TEXTURE_SIZE * 0.5f;
    const float radius = (float)AA_DEMO_TEXTURE_SIZE * 0.36f;
    const float radius_sq = radius * radius;

    for (uint32_t y = 0U; y < AA_DEMO_TEXTURE_SIZE; ++y) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0U; x < AA_DEMO_TEXTURE_SIZE; ++x) {
            const float dx = ((float)x + 0.5f) - center;
            const float dy = ((float)y + 0.5f) - center;
            row[x] = dx * dx + dy * dy <= radius_sq ? 255U : 0U;
        }
    }
}

static void fill_rgba_shape(grape_texture_t *texture)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    const size_t stride = grape_texture_stride(texture);
    const float center = (float)AA_DEMO_TEXTURE_SIZE * 0.5f;
    const float outer = (float)AA_DEMO_TEXTURE_SIZE * 0.40f;
    const float inner = (float)AA_DEMO_TEXTURE_SIZE * 0.17f;

    for (uint32_t y = 0U; y < AA_DEMO_TEXTURE_SIZE; ++y) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0U; x < AA_DEMO_TEXTURE_SIZE; ++x) {
            uint8_t *pixel = row + (size_t)x * 4U;
            const float dx = ((float)x + 0.5f) - center;
            const float dy = ((float)y + 0.5f) - center;
            const float radius = sqrtf(dx * dx + dy * dy);
            const bool ring = radius <= outer && radius >= inner;
            const bool slash = fabsf(dy - dx * 0.45f) < 5.0f && radius <= outer;
            const bool visible = ring || slash;

            pixel[0] = (uint8_t)(110U + (x * 120U) / AA_DEMO_TEXTURE_SIZE);
            pixel[1] = (uint8_t)(170U + (y * 70U) / AA_DEMO_TEXTURE_SIZE);
            pixel[2] = 145U;
            pixel[3] = visible ? 255U : 0U;
        }
    }
}

static void fill_rgb565_checker(grape_texture_t *texture)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    const size_t stride = grape_texture_stride(texture);

    for (uint32_t y = 0U; y < AA_DEMO_CHECKER_SIZE; ++y) {
        uint16_t *row = (uint16_t *)(pixels + (size_t)y * stride);
        for (uint32_t x = 0U; x < AA_DEMO_CHECKER_SIZE; ++x) {
            const bool checker = (((x / 2U) + (y / 2U)) & 1U) != 0U;
            row[x] = checker
                ? pack_rgb565(166U, 209U, 137U)
                : pack_rgb565(35U, 38U, 52U);
        }
    }
}

static esp_err_t create_texture(grape_context_t *grape,
                                uint32_t width,
                                uint32_t height,
                                grape_pixel_format_t format,
                                grape_texture_t **out_texture)
{
    grape_texture_desc_t desc = {
        .width = width,
        .height = height,
        .format = format,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    return grape_texture_create(grape, &desc, out_texture);
}

static esp_err_t create_pair_surface(grape_context_t *grape,
                                     grape_texture_t *texture,
                                     const grape_surface_shader_desc_t *shaders,
                                     size_t shader_count,
                                     uint32_t width,
                                     uint32_t height,
                                     grape_surface_texture_mode_t texture_mode,
                                     float center_x,
                                     float center_y,
                                     float rotation,
                                     bool quality)
{
    if (s_surface_count >= sizeof(s_surfaces) / sizeof(s_surfaces[0])) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_surface_desc_t desc = GRAPE_SURFACE_DESC_DEFAULT();
    desc.texture = texture;
    desc.width = width;
    desc.height = height;
    desc.texture_mode = texture_mode;
    desc.texture_filter = quality ? GRAPE_TEXTURE_FILTER_LINEAR : GRAPE_TEXTURE_FILTER_NEAREST;
    desc.aa = quality ? GRAPE_SURFACE_AA_COVERAGE_4X : GRAPE_SURFACE_AA_NONE;
    desc.shaders = shaders;
    desc.shader_count = shader_count;
    desc.transform.x = center_x;
    desc.transform.y = center_y;
    desc.transform.origin_x = (float)width * 0.5f;
    desc.transform.origin_y = (float)height * 0.5f;
    desc.transform.rotation = rotation;
    desc.z = (int32_t)s_surface_count;

    return grape_surface_create(grape, &desc, &s_surfaces[s_surface_count++]);
}

esp_err_t grape_demo_aa_run(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display || display->width < 320U || display->height < 640U) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = create_texture(grape, AA_DEMO_TEXTURE_SIZE, AA_DEMO_TEXTURE_SIZE,
                                   GRAPE_PIXEL_FORMAT_A8, &s_a8_texture);
    if (ret != ESP_OK) return ret;
    fill_a8_circle(s_a8_texture);
    ret = grape_texture_invalidate(s_a8_texture);
    if (ret != ESP_OK) return ret;

    ret = create_texture(grape, AA_DEMO_TEXTURE_SIZE, AA_DEMO_TEXTURE_SIZE,
                         GRAPE_PIXEL_FORMAT_RGBA8888, &s_rgba_texture);
    if (ret != ESP_OK) return ret;
    fill_rgba_shape(s_rgba_texture);
    ret = grape_texture_invalidate(s_rgba_texture);
    if (ret != ESP_OK) return ret;

    ret = create_texture(grape, AA_DEMO_CHECKER_SIZE, AA_DEMO_CHECKER_SIZE,
                         GRAPE_PIXEL_FORMAT_RGB565, &s_rgb565_texture);
    if (ret != ESP_OK) return ret;
    fill_rgb565_checker(s_rgb565_texture);
    ret = grape_texture_invalidate(s_rgb565_texture);
    if (ret != ESP_OK) return ret;

    const float left_x = (float)display->width * 0.27f;
    const float right_x = (float)display->width * 0.73f;
    const float row_step = (float)display->height / 5.2f;
    const float start_y = row_step * 0.62f;
    const uint32_t square = display->width >= 600U ? 150U : 112U;
    const uint32_t wide_width = display->width >= 600U ? 180U : 132U;
    const uint32_t wide_height = display->width >= 600U ? 112U : 84U;

#define CREATE_PAIR(texture_, shaders_, shader_count_, width_, height_, mode_, row_, rotation_) \
    do { \
        ret = create_pair_surface(grape, texture_, shaders_, shader_count_, width_, height_, mode_, \
                                  left_x, start_y + row_step * (row_), rotation_, false); \
        if (ret != ESP_OK) return ret; \
        ret = create_pair_surface(grape, texture_, shaders_, shader_count_, width_, height_, mode_, \
                                  right_x, start_y + row_step * (row_), rotation_, true); \
        if (ret != ESP_OK) return ret; \
    } while (0)

    /* Hard A8 interior edge + rotated surface silhouette. */
    CREATE_PAIR(s_a8_texture, NULL, 0U, square, square,
                GRAPE_SURFACE_TEXTURE_STRETCH, 0.0f, 0.33f);
    grape_color_t a8_tint = {166U, 209U, 137U, 255U};
    ret = grape_surface_set_tint(s_surfaces[0], a8_tint);
    if (ret != ESP_OK) return ret;
    ret = grape_surface_set_tint(s_surfaces[1], a8_tint);
    if (ret != ESP_OK) return ret;

    /* Straight-alpha RGBA interior edges. */
    CREATE_PAIR(s_rgba_texture, NULL, 0U, square, square,
                GRAPE_SURFACE_TEXTURE_STRETCH, 1.0f, -0.29f);

    /* High-frequency opaque RGB texture demonstrates reconstruction filtering. */
    CREATE_PAIR(s_rgb565_texture, NULL, 0U, wide_width, wide_height,
                GRAPE_SURFACE_TEXTURE_STRETCH, 2.0f, 0.20f);

    /* TILE verifies bilinear wrap across the texture seam. */
    CREATE_PAIR(s_rgb565_texture, NULL, 0U, wide_width, wide_height,
                GRAPE_SURFACE_TEXTURE_TILE, 3.0f, -0.16f);

    /* Textureless procedural content: only geometric coverage should change. */
    const grape_surface_shader_desc_t procedural[] = {
        {
            .program = &generated_procedural_gradient_program,
            .uniforms = NULL,
        },
    };
    CREATE_PAIR(NULL, procedural, 1U, wide_width, wide_height,
                GRAPE_SURFACE_TEXTURE_STRETCH, 4.0f, 0.24f);

#undef CREATE_PAIR

    ESP_LOGI(TAG, "AA demo: left=nearest/no-AA, right=linear/4x coverage");
    ESP_LOGI(TAG, "rows: A8, RGBA8888, RGB565 stretch, RGB565 tile, shader-only coverage");
    return grape_present(grape);
}
