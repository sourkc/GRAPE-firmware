#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "grape_gpu_internal.h"
#include "grape_internal.h"

static inline uint8_t gpu_mul8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint16_t)a * b + 127U) / 255U);
}

static inline int32_t gpu_clamp_index(int32_t value, int32_t limit)
{
    if (value < 0) {
        return 0;
    }
    if (value >= limit) {
        return limit - 1;
    }
    return value;
}

static inline int32_t gpu_wrap_index(int32_t value, int32_t limit)
{
    int32_t wrapped = value % limit;
    return wrapped < 0 ? wrapped + limit : wrapped;
}

static inline int32_t gpu_address_index(int32_t value,
                                        int32_t limit,
                                        grape_gpu_address_mode_t mode)
{
    return mode == GRAPE_GPU_ADDRESS_REPEAT
        ? gpu_wrap_index(value, limit)
        : gpu_clamp_index(value, limit);
}

static inline grape_color_t gpu_read_texel(const grape_texture_t *texture,
                                           int32_t x,
                                           int32_t y)
{
    const uint8_t *row = texture->pixels + (size_t)y * texture->stride;
    switch (texture->format) {
        case GRAPE_PIXEL_FORMAT_A8:
            return (grape_color_t) {255U, 255U, 255U, row[x]};
        case GRAPE_PIXEL_FORMAT_RGB565: {
            const uint16_t packed = (uint16_t)row[(size_t)x * 2U] |
                                    ((uint16_t)row[(size_t)x * 2U + 1U] << 8U);
            const uint8_t r5 = (uint8_t)((packed >> 11U) & 0x1FU);
            const uint8_t g6 = (uint8_t)((packed >> 5U) & 0x3FU);
            const uint8_t b5 = (uint8_t)(packed & 0x1FU);
            return (grape_color_t) {
                .r = (uint8_t)((r5 << 3U) | (r5 >> 2U)),
                .g = (uint8_t)((g6 << 2U) | (g6 >> 4U)),
                .b = (uint8_t)((b5 << 3U) | (b5 >> 2U)),
                .a = 255U,
            };
        }
        case GRAPE_PIXEL_FORMAT_RGB888: {
            const uint8_t *pixel = row + (size_t)x * 3U;
            return (grape_color_t) { pixel[0], pixel[1], pixel[2], 255U };
        }
        case GRAPE_PIXEL_FORMAT_RGBA8888: {
            const uint8_t *pixel = row + (size_t)x * 4U;
            return (grape_color_t) { pixel[0], pixel[1], pixel[2], pixel[3] };
        }
        default:
            return (grape_color_t) {0};
    }
}

static inline grape_color_t gpu_mix_bilinear(grape_color_t c00,
                                               grape_color_t c10,
                                               grape_color_t c01,
                                               grape_color_t c11,
                                               uint32_t w00,
                                               uint32_t w10,
                                               uint32_t w01,
                                               uint32_t w11)
{
    const uint8_t p00r = gpu_mul8(c00.r, c00.a);
    const uint8_t p00g = gpu_mul8(c00.g, c00.a);
    const uint8_t p00b = gpu_mul8(c00.b, c00.a);
    const uint8_t p10r = gpu_mul8(c10.r, c10.a);
    const uint8_t p10g = gpu_mul8(c10.g, c10.a);
    const uint8_t p10b = gpu_mul8(c10.b, c10.a);
    const uint8_t p01r = gpu_mul8(c01.r, c01.a);
    const uint8_t p01g = gpu_mul8(c01.g, c01.a);
    const uint8_t p01b = gpu_mul8(c01.b, c01.a);
    const uint8_t p11r = gpu_mul8(c11.r, c11.a);
    const uint8_t p11g = gpu_mul8(c11.g, c11.a);
    const uint8_t p11b = gpu_mul8(c11.b, c11.a);

#define MIX(a, b, c, d) \
    (uint8_t)(((uint32_t)(a) * w00 + (uint32_t)(b) * w10 + \
               (uint32_t)(c) * w01 + (uint32_t)(d) * w11 + 32768U) >> 16U)

    const uint8_t alpha = MIX(c00.a, c10.a, c01.a, c11.a);
    if (alpha == 0U) {
        return (grape_color_t) {0};
    }

    const uint8_t premul_r = MIX(p00r, p10r, p01r, p11r);
    const uint8_t premul_g = MIX(p00g, p10g, p01g, p11g);
    const uint8_t premul_b = MIX(p00b, p10b, p01b, p11b);
#undef MIX

    if (alpha == 255U) {
        return (grape_color_t) { premul_r, premul_g, premul_b, 255U };
    }

    const uint32_t half_alpha = alpha / 2U;
    uint32_t r = ((uint32_t)premul_r * 255U + half_alpha) / alpha;
    uint32_t g = ((uint32_t)premul_g * 255U + half_alpha) / alpha;
    uint32_t b = ((uint32_t)premul_b * 255U + half_alpha) / alpha;
    if (r > 255U) r = 255U;
    if (g > 255U) g = 255U;
    if (b > 255U) b = 255U;
    return (grape_color_t) {(uint8_t)r, (uint8_t)g, (uint8_t)b, alpha};
}

esp_err_t grape_gpu_sample_texture(const grape_texture_t *texture,
                                   const grape_gpu_sampler_desc_t *sampler,
                                   float u,
                                   float v,
                                   grape_color_t *out_color)
{
    if (!texture || !sampler || !out_color ||
        !isfinite(u) || !isfinite(v) ||
        texture->width == 0U || texture->height == 0U ||
        texture->width > INT32_MAX || texture->height > INT32_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const int32_t width = (int32_t)texture->width;
    const int32_t height = (int32_t)texture->height;

    if (sampler->filter == GRAPE_GPU_FILTER_NEAREST) {
        const float xf = u * (float)texture->width;
        const float yf = v * (float)texture->height;
        int32_t x = grape_floor_to_i32(xf);
        int32_t y = grape_floor_to_i32(yf);
        x = gpu_address_index(x, width, sampler->address_u);
        y = gpu_address_index(y, height, sampler->address_v);
        *out_color = gpu_read_texel(texture, x, y);
        return ESP_OK;
    }

    if (sampler->filter != GRAPE_GPU_FILTER_LINEAR) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const float centered_x = u * (float)texture->width - 0.5f;
    const float centered_y = v * (float)texture->height - 0.5f;
    const int32_t x0_raw = grape_floor_to_i32(centered_x);
    const int32_t y0_raw = grape_floor_to_i32(centered_y);
    const int32_t x1_raw = x0_raw + 1;
    const int32_t y1_raw = y0_raw + 1;
    const float frac_x = centered_x - (float)x0_raw;
    const float frac_y = centered_y - (float)y0_raw;

    uint32_t fx = (uint32_t)(frac_x * 256.0f + 0.5f);
    uint32_t fy = (uint32_t)(frac_y * 256.0f + 0.5f);
    if (fx > 256U) fx = 256U;
    if (fy > 256U) fy = 256U;
    const uint32_t ix = 256U - fx;
    const uint32_t iy = 256U - fy;

    const int32_t x0 = gpu_address_index(x0_raw, width, sampler->address_u);
    const int32_t x1 = gpu_address_index(x1_raw, width, sampler->address_u);
    const int32_t y0 = gpu_address_index(y0_raw, height, sampler->address_v);
    const int32_t y1 = gpu_address_index(y1_raw, height, sampler->address_v);

    *out_color = gpu_mix_bilinear(
        gpu_read_texel(texture, x0, y0),
        gpu_read_texel(texture, x1, y0),
        gpu_read_texel(texture, x0, y1),
        gpu_read_texel(texture, x1, y1),
        ix * iy,
        fx * iy,
        ix * fy,
        fx * fy
    );
    return ESP_OK;
}

static inline float gpu_clamp01(float value)
{
    if (value <= 0.0f) return 0.0f;
    if (value >= 1.0f) return 1.0f;
    return value;
}

static inline float gpu_eval_plane(const grape_gpu_interp_plane_t *plane,
                                   const grape_gpu_triangle_setup_t *setup,
                                   float screen_x,
                                   float screen_y)
{
    const float dx = screen_x - ((float)setup->min_x + 0.5f);
    const float dy = screen_y - ((float)setup->min_y + 0.5f);
    return plane->row_start + plane->step_x * dx + plane->step_y * dy;
}

esp_err_t grape_gpu_shade_fragment(const grape_gpu_triangle_setup_t *setup,
                                  grape_gpu_fragment_program_t program,
                                  const grape_texture_t *texture,
                                  const grape_gpu_sampler_desc_t *sampler,
                                  float screen_x,
                                  float screen_y,
                                  grape_color_t *out_color)
{
    if (!setup || !out_color || !isfinite(screen_x) || !isfinite(screen_y)) {
        return ESP_ERR_INVALID_ARG;
    }

    const float inverse_w = gpu_eval_plane(&setup->inv_w, setup, screen_x, screen_y);
    if (!isfinite(inverse_w) || fabsf(inverse_w) <= 1.0e-12f) {
        return ESP_ERR_INVALID_STATE;
    }
    const float w = 1.0f / inverse_w;

    grape_color_t vertex_color = {255U, 255U, 255U, 255U};
    if (program == GRAPE_GPU_FRAGMENT_PROGRAM_VERTEX_COLOR ||
        program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR) {
        uint8_t *channels = &vertex_color.r;
        for (uint32_t i = 0U; i < 4U; ++i) {
            const float value = gpu_eval_plane(&setup->color_over_w[i], setup, screen_x, screen_y) * w;
            channels[i] = (uint8_t)(gpu_clamp01(value) * 255.0f + 0.5f);
        }
    }

    if (program == GRAPE_GPU_FRAGMENT_PROGRAM_VERTEX_COLOR) {
        *out_color = vertex_color;
        return ESP_OK;
    }

    if (program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE ||
        program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR) {
        if (!texture || !sampler) {
            return ESP_ERR_INVALID_STATE;
        }
        const float u = gpu_eval_plane(&setup->u_over_w, setup, screen_x, screen_y) * w;
        const float v = gpu_eval_plane(&setup->v_over_w, setup, screen_x, screen_y) * w;
        grape_color_t texture_color;
        esp_err_t ret = grape_gpu_sample_texture(texture, sampler, u, v, &texture_color);
        if (ret != ESP_OK) {
            return ret;
        }
        if (program == GRAPE_GPU_FRAGMENT_PROGRAM_TEXTURE_VERTEX_COLOR) {
            texture_color.r = gpu_mul8(texture_color.r, vertex_color.r);
            texture_color.g = gpu_mul8(texture_color.g, vertex_color.g);
            texture_color.b = gpu_mul8(texture_color.b, vertex_color.b);
            texture_color.a = gpu_mul8(texture_color.a, vertex_color.a);
        }
        *out_color = texture_color;
        return ESP_OK;
    }

    return ESP_ERR_NOT_SUPPORTED;
}
