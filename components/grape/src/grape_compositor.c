#include <math.h>

#include "grape_internal.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} rgba8_t;

static inline uint8_t mul8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint16_t)a * b + 127U) / 255U);
}

static inline rgba8_t blend_over(rgba8_t dst, rgba8_t src)
{
    if (src.a == 0) {
        return dst;
    }
    if (src.a == 255) {
        src.a = 255;
        return src;
    }

    uint16_t inv = 255U - src.a;
    // DO NOT rewrite as mul8, it will cause rounding imprecision!
    rgba8_t out = {
        .r = (uint8_t)(((uint16_t)src.r * src.a + (uint16_t)dst.r * inv + 127U) / 255U),
        .g = (uint8_t)(((uint16_t)src.g * src.a + (uint16_t)dst.g * inv + 127U) / 255U),
        .b = (uint8_t)(((uint16_t)src.b * src.a + (uint16_t)dst.b * inv + 127U) / 255U),
        .a = 255,
    };
    return out;
}

static bool sample_surface(const grape_surface_t *surface, float screen_x, float screen_y, rgba8_t *out)
{
    if (!surface->visible || !surface->texture || surface->opacity == 0) {
        return false;
    }

    if (screen_x < surface->bounds.x || screen_y < surface->bounds.y ||
        screen_x >= surface->bounds.x + surface->bounds.width ||
        screen_y >= surface->bounds.y + surface->bounds.height) {
        return false;
    }

    float dx = screen_x - surface->transform.x;
    float dy = screen_y - surface->transform.y;
    float rotated_x = surface->cos_rotation * dx + surface->sin_rotation * dy;
    float rotated_y = -surface->sin_rotation * dx + surface->cos_rotation * dy;
    float local_x = rotated_x / surface->transform.scale_x + surface->transform.origin_x;
    float local_y = rotated_y / surface->transform.scale_y + surface->transform.origin_y;

    // point sampling. Future: blending! (or PPA, idk yet)
    int32_t tx = (int32_t)floorf(local_x);
    int32_t ty = (int32_t)floorf(local_y);

    grape_texture_t *texture = surface->texture;
    if (tx < 0 || ty < 0 || tx >= (int32_t)texture->width || ty >= (int32_t)texture->height) {
        return false;
    }

    const uint8_t *row = texture->pixels + (size_t)ty * texture->stride;
    rgba8_t color = {0, 0, 0, 255};

    switch (texture->format) {
        case GRAPE_PIXEL_FORMAT_RGB565: {
            uint16_t pixel = ((const uint16_t *)row)[tx];
            // Extract
            uint8_t r5 = (uint8_t)((pixel >> 11) & 0x1F);
            uint8_t g6 = (uint8_t)((pixel >> 5) & 0x3F);
            uint8_t b5 = (uint8_t)(pixel & 0x1F);
            // Expand
            color.r = (uint8_t)((r5 << 3) | (r5 >> 2));
            color.g = (uint8_t)((g6 << 2) | (g6 >> 4));
            color.b = (uint8_t)((b5 << 3) | (b5 >> 2));
            color.a = 255;
            break;
        }
        case GRAPE_PIXEL_FORMAT_RGB888: {
            const uint8_t *pixel = row + (size_t)tx * 3U;
            // No need to do anything. color is already RGBA888, we just add the alpha
            color.r = pixel[0];
            color.g = pixel[1];
            color.b = pixel[2];
            color.a = 255;
            break;
        }
        case GRAPE_PIXEL_FORMAT_A8: {
            uint8_t alpha = row[tx];
            color.r = surface->tint.r;
            color.g = surface->tint.g;
            color.b = surface->tint.b;
            color.a = mul8(alpha, surface->tint.a); // Apply tint.
            color.a = mul8(color.a, surface->opacity);
            *out = color;
            return color.a != 0; // Returns early, doesnt reach end tint calculation
        }
        default:
            return false; // Unsupported color format
    }

    // Apply tint
    color.r = mul8(color.r, surface->tint.r);
    color.g = mul8(color.g, surface->tint.g);
    color.b = mul8(color.b, surface->tint.b);
    color.a = mul8(surface->opacity, surface->tint.a);
    *out = color;
    return color.a != 0;
}

static inline void write_rgb565(uint8_t *dst, rgba8_t color)
{
    uint16_t pixel = (uint16_t)(((color.r >> 3) << 11) | ((color.g >> 2) << 5) | (color.b >> 3));
    dst[0] = (uint8_t)(pixel & 0xFF);
    dst[1] = (uint8_t)(pixel >> 8);
}

static inline void write_rgb888(uint8_t *dst, rgba8_t color)
{
    dst[0] = color.r;
    dst[1] = color.g;
    dst[2] = color.b;
}

static inline rgba8_t read_rgb565(const uint8_t *src)
{
    uint16_t pixel =
        (uint16_t)src[0] |
        ((uint16_t)src[1] << 8);

    uint8_t r5 = (uint8_t)((pixel >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((pixel >> 5) & 0x3F);
    uint8_t b5 = (uint8_t)(pixel & 0x1F);

    return (rgba8_t) {
        .r = (uint8_t)((r5 << 3) | (r5 >> 2)),
        .g = (uint8_t)((g6 << 2) | (g6 >> 4)),
        .b = (uint8_t)((b5 << 3) | (b5 >> 2)),
        .a = 255,
    };
}

static inline rgba8_t read_rgb888(const uint8_t *src)
{
    return (rgba8_t) {
        .r = src[0],
        .g = src[1],
        .b = src[2],
        .a = 255,
    };
}

esp_err_t grape_compositor_render(grape_context_t *context, grape_rect_t rect)
{
    if (grape_rect_empty(rect)) {
        return ESP_OK;
    }

    size_t bpp = grape_bytes_per_pixel(context->display_info.format);

    size_t required =
        (size_t)rect.width *
        (size_t)rect.height *
        bpp;

    if (required > context->scratch_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    rgba8_t background = {
        .r = context->background.r,
        .g = context->background.g,
        .b = context->background.b,
        .a = 255,
    };

    // Fill damaged rect with background
    for (int32_t y = 0; y < rect.height; ++y) {
        for (int32_t x = 0; x < rect.width; ++x) {
            size_t offset =
                ((size_t)y * rect.width + x) * bpp;

            uint8_t *dst =
                context->scratch + offset;

            if (context->display_info.format ==
                GRAPE_PIXEL_FORMAT_RGB565) {

                write_rgb565(dst, background);

            } else {

                write_rgb888(dst, background);
            }
        }
    }

    // Composite surfaces (Z-ordered)
    for (grape_surface_t *surface = context->surfaces;
         surface;
         surface = surface->next) {

        if (!surface->visible ||
            !surface->texture ||
            surface->opacity == 0) {
            continue;
        }

        grape_rect_t clipped =
            grape_rect_intersection(
                rect,
                surface->bounds
            );

        if (grape_rect_empty(clipped)) {
            continue;
        }

        for (int32_t y = clipped.y;
             y < clipped.y + clipped.height;
             ++y) {

            for (int32_t x = clipped.x;
                 x < clipped.x + clipped.width;
                 ++x) {

                rgba8_t source;

                if (!sample_surface(
                        surface,
                        (float)x + 0.5f,
                        (float)y + 0.5f,
                        &source)) {
                    continue;
                }

                // Convert screen cords to scratch buffer cords
                int32_t scratch_x = x - rect.x;
                int32_t scratch_y = y - rect.y;

                size_t offset =
                    ((size_t)scratch_y * rect.width +
                     scratch_x) *
                    bpp;

                uint8_t *dst =
                    context->scratch + offset;

                // Opaque pixels don't need blending
                if (source.a == 255) {

                    if (context->display_info.format ==
                        GRAPE_PIXEL_FORMAT_RGB565) {

                        write_rgb565(dst, source);

                    } else {

                        write_rgb888(dst, source);
                    }

                    continue;
                }

                rgba8_t destination;

                if (context->display_info.format ==
                    GRAPE_PIXEL_FORMAT_RGB565) {

                    destination = read_rgb565(dst);

                } else {

                    destination = read_rgb888(dst);
                }

                rgba8_t result =
                    blend_over(destination, source);

                if (context->display_info.format ==
                    GRAPE_PIXEL_FORMAT_RGB565) {

                    write_rgb565(dst, result);

                } else {

                    write_rgb888(dst, result);
                }
            }
        }
    }

    return grape_display_blit(
        context->display,
        rect,
        context->scratch
    );
}