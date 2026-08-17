#pragma once

#include <math.h>

#include "grape/grape_shader.h"

struct grape_shader_kernel_args {
    const uint8_t *texture_pixels;
    size_t texture_stride;
    uint32_t texture_width;
    uint32_t texture_height;
    grape_pixel_format_t texture_format;
    uint32_t surface_width;
    uint32_t surface_height;
    uint8_t *target_pixels;
    size_t target_stride;
    grape_pixel_format_t target_format;
    grape_rect_t clipped;
    grape_color_t tint;
    uint8_t opacity;
    float local_x_from_screen_x;
    float local_x_from_screen_y;
    float local_x_offset;
    float local_y_from_screen_x;
    float local_y_from_screen_y;
    float local_y_offset;
};

static inline uint8_t grape_shader_mul8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint16_t)a * b + 127U) / 255U);
}

static inline uint8_t grape_shader_float_to_u8(float value)
{
    if (value <= 0.0f) {
        return 0U;
    }
    if (value >= 1.0f) {
        return 255U;
    }
    return (uint8_t)(value * 255.0f + 0.5f);
}

static inline float grape_shader_smoothstepf(float edge0, float edge1, float x)
{
    float t = (x - edge0) / (edge1 - edge0);
    t = fminf(fmaxf(t, 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline grape_shader_vec2_t grape_shader_normalize_vec2(grape_shader_vec2_t v)
{
    float inverse_length = 1.0f / sqrtf(v.x * v.x + v.y * v.y);
    return (grape_shader_vec2_t) { v.x * inverse_length, v.y * inverse_length };
}

static inline grape_shader_vec3_t grape_shader_normalize_vec3(grape_shader_vec3_t v)
{
    float inverse_length = 1.0f / sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    return (grape_shader_vec3_t) { v.x * inverse_length, v.y * inverse_length, v.z * inverse_length };
}

static inline grape_shader_vec4_t grape_shader_normalize_vec4(grape_shader_vec4_t v)
{
    float inverse_length = 1.0f / sqrtf(v.x * v.x + v.y * v.y + v.z * v.z + v.w * v.w);
    return (grape_shader_vec4_t) {
        v.x * inverse_length,
        v.y * inverse_length,
        v.z * inverse_length,
        v.w * inverse_length,
    };
}

static inline grape_shader_vec2_t grape_shader_reflect_vec2(
    grape_shader_vec2_t incident,
    grape_shader_vec2_t normal
)
{
    float scale = 2.0f * (normal.x * incident.x + normal.y * incident.y);
    return (grape_shader_vec2_t) {
        incident.x - scale * normal.x,
        incident.y - scale * normal.y,
    };
}

static inline grape_shader_vec3_t grape_shader_reflect_vec3(
    grape_shader_vec3_t incident,
    grape_shader_vec3_t normal
)
{
    float scale = 2.0f * (normal.x * incident.x + normal.y * incident.y + normal.z * incident.z);
    return (grape_shader_vec3_t) {
        incident.x - scale * normal.x,
        incident.y - scale * normal.y,
        incident.z - scale * normal.z,
    };
}

static inline grape_shader_vec4_t grape_shader_reflect_vec4(
    grape_shader_vec4_t incident,
    grape_shader_vec4_t normal
)
{
    float scale = 2.0f * (
        normal.x * incident.x + normal.y * incident.y +
        normal.z * incident.z + normal.w * incident.w
    );
    return (grape_shader_vec4_t) {
        incident.x - scale * normal.x,
        incident.y - scale * normal.y,
        incident.z - scale * normal.z,
        incident.w - scale * normal.w,
    };
}

static inline grape_shader_vec4_t grape_shader_source_a8(
    const grape_shader_kernel_args_t *args,
    int32_t tx,
    int32_t ty
)
{
    const uint8_t *row = args->texture_pixels + (size_t)ty * args->texture_stride;
    uint8_t alpha = grape_shader_mul8(row[tx], args->tint.a);
    return (grape_shader_vec4_t) {
        .x = (float)args->tint.r / 255.0f,
        .y = (float)args->tint.g / 255.0f,
        .z = (float)args->tint.b / 255.0f,
        .w = (float)alpha / 255.0f,
    };
}

static inline grape_shader_vec4_t grape_shader_source_rgb565(
    const grape_shader_kernel_args_t *args,
    int32_t tx,
    int32_t ty
)
{
    const uint8_t *row = args->texture_pixels + (size_t)ty * args->texture_stride;
    const uint8_t *pixel = row + (size_t)tx * 2U;
    uint16_t packed = (uint16_t)pixel[0] | ((uint16_t)pixel[1] << 8);
    uint8_t r5 = (uint8_t)((packed >> 11) & 0x1FU);
    uint8_t g6 = (uint8_t)((packed >> 5) & 0x3FU);
    uint8_t b5 = (uint8_t)(packed & 0x1FU);
    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));

    return (grape_shader_vec4_t) {
        .x = (float)grape_shader_mul8(r, args->tint.r) / 255.0f,
        .y = (float)grape_shader_mul8(g, args->tint.g) / 255.0f,
        .z = (float)grape_shader_mul8(b, args->tint.b) / 255.0f,
        .w = (float)args->tint.a / 255.0f,
    };
}

static inline grape_shader_vec4_t grape_shader_source_rgb888(
    const grape_shader_kernel_args_t *args,
    int32_t tx,
    int32_t ty
)
{
    const uint8_t *row = args->texture_pixels + (size_t)ty * args->texture_stride;
    const uint8_t *pixel = row + (size_t)tx * 3U;
    return (grape_shader_vec4_t) {
        .x = (float)grape_shader_mul8(pixel[0], args->tint.r) / 255.0f,
        .y = (float)grape_shader_mul8(pixel[1], args->tint.g) / 255.0f,
        .z = (float)grape_shader_mul8(pixel[2], args->tint.b) / 255.0f,
        .w = (float)args->tint.a / 255.0f,
    };
}

static inline grape_shader_vec4_t grape_shader_source_rgba8888(
    const grape_shader_kernel_args_t *args,
    int32_t tx,
    int32_t ty
)
{
    const uint8_t *row = args->texture_pixels + (size_t)ty * args->texture_stride;
    const uint8_t *pixel = row + (size_t)tx * 4U;
    return (grape_shader_vec4_t) {
        .x = (float)grape_shader_mul8(pixel[0], args->tint.r) / 255.0f,
        .y = (float)grape_shader_mul8(pixel[1], args->tint.g) / 255.0f,
        .z = (float)grape_shader_mul8(pixel[2], args->tint.b) / 255.0f,
        .w = (float)grape_shader_mul8(pixel[3], args->tint.a) / 255.0f,
    };
}

static inline void grape_shader_composite_rgb565(
    const grape_shader_kernel_args_t *args,
    uint8_t *dst,
    grape_shader_vec4_t color
)
{
    uint8_t src_r = grape_shader_float_to_u8(color.x);
    uint8_t src_g = grape_shader_float_to_u8(color.y);
    uint8_t src_b = grape_shader_float_to_u8(color.z);
    uint8_t src_a = grape_shader_mul8(grape_shader_float_to_u8(color.w), args->opacity);

    if (src_a == 0U) {
        return;
    }

    if (src_a != 255U) {
        uint16_t packed = (uint16_t)dst[0] | ((uint16_t)dst[1] << 8);
        uint8_t r5 = (uint8_t)((packed >> 11) & 0x1FU);
        uint8_t g6 = (uint8_t)((packed >> 5) & 0x3FU);
        uint8_t b5 = (uint8_t)(packed & 0x1FU);
        uint8_t dst_r = (uint8_t)((r5 << 3) | (r5 >> 2));
        uint8_t dst_g = (uint8_t)((g6 << 2) | (g6 >> 4));
        uint8_t dst_b = (uint8_t)((b5 << 3) | (b5 >> 2));
        uint16_t inv = 255U - src_a;

        src_r = (uint8_t)(((uint16_t)src_r * src_a + (uint16_t)dst_r * inv + 127U) / 255U);
        src_g = (uint8_t)(((uint16_t)src_g * src_a + (uint16_t)dst_g * inv + 127U) / 255U);
        src_b = (uint8_t)(((uint16_t)src_b * src_a + (uint16_t)dst_b * inv + 127U) / 255U);
    }

    uint16_t packed = (uint16_t)(((src_r >> 3) << 11) | ((src_g >> 2) << 5) | (src_b >> 3));
    dst[0] = (uint8_t)(packed & 0xFFU);
    dst[1] = (uint8_t)(packed >> 8);
}

static inline void grape_shader_composite_rgb888(
    const grape_shader_kernel_args_t *args,
    uint8_t *dst,
    grape_shader_vec4_t color
)
{
    uint8_t src_r = grape_shader_float_to_u8(color.x);
    uint8_t src_g = grape_shader_float_to_u8(color.y);
    uint8_t src_b = grape_shader_float_to_u8(color.z);
    uint8_t src_a = grape_shader_mul8(grape_shader_float_to_u8(color.w), args->opacity);

    if (src_a == 0U) {
        return;
    }

    if (src_a != 255U) {
        uint16_t inv = 255U - src_a;
        src_r = (uint8_t)(((uint16_t)src_r * src_a + (uint16_t)dst[0] * inv + 127U) / 255U);
        src_g = (uint8_t)(((uint16_t)src_g * src_a + (uint16_t)dst[1] * inv + 127U) / 255U);
        src_b = (uint8_t)(((uint16_t)src_b * src_a + (uint16_t)dst[2] * inv + 127U) / 255U);
    }

    dst[0] = src_r;
    dst[1] = src_g;
    dst[2] = src_b;
}

static inline void grape_shader_composite_rgba8888(
    const grape_shader_kernel_args_t *args,
    uint8_t *dst,
    grape_shader_vec4_t color
)
{
    uint8_t src_r = grape_shader_float_to_u8(color.x);
    uint8_t src_g = grape_shader_float_to_u8(color.y);
    uint8_t src_b = grape_shader_float_to_u8(color.z);
    uint8_t src_a = grape_shader_mul8(grape_shader_float_to_u8(color.w), args->opacity);

    if (src_a == 0U) {
        return;
    }

    if (src_a == 255U) {
        dst[0] = src_r;
        dst[1] = src_g;
        dst[2] = src_b;
        dst[3] = 255U;
        return;
    }

    const uint8_t dst_a = dst[3];
    const uint32_t inv = 255U - src_a;
    /* Keep enough precision for straight-alpha source-over blending. The
     * shared alpha numerator is scaled by 255; using it directly avoids
     * prematurely rounding the destination contribution before RGB is
     * un-premultiplied again. */
    const uint32_t alpha_numerator =
        (uint32_t)src_a * 255U + (uint32_t)dst_a * inv;

    if (alpha_numerator == 0U) {
        dst[0] = 0U;
        dst[1] = 0U;
        dst[2] = 0U;
        dst[3] = 0U;
        return;
    }

    uint32_t r = (uint32_t)src_r * src_a * 255U +
                 (uint32_t)dst[0] * dst_a * inv;
    uint32_t g = (uint32_t)src_g * src_a * 255U +
                 (uint32_t)dst[1] * dst_a * inv;
    uint32_t b = (uint32_t)src_b * src_a * 255U +
                 (uint32_t)dst[2] * dst_a * inv;

    dst[0] = (uint8_t)((r + alpha_numerator / 2U) / alpha_numerator);
    dst[1] = (uint8_t)((g + alpha_numerator / 2U) / alpha_numerator);
    dst[2] = (uint8_t)((b + alpha_numerator / 2U) / alpha_numerator);
    dst[3] = (uint8_t)((alpha_numerator + 127U) / 255U);
}
