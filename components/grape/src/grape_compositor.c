#include <math.h>

#include "grape_internal.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} rgba8_t;

static inline uint8_t *render_target_pixel(grape_context_t *context,
                                           int32_t x,
                                           int32_t y,
                                           size_t bpp)
{
    return (uint8_t *)context->render_target.pixels +
           (size_t)y * context->render_target.stride +
           (size_t)x * bpp;
}

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

static inline void composite_source_pixel(uint8_t *dst, grape_pixel_format_t output_format, rgba8_t source)
{
    if (source.a == 0) {
        return;
    }

    if (source.a == 255) {
        if (output_format == GRAPE_PIXEL_FORMAT_RGB565) {
            write_rgb565(dst, source);
        } else {
            write_rgb888(dst, source);
        }
        return;
    }

    rgba8_t destination;
    if (output_format == GRAPE_PIXEL_FORMAT_RGB565) {
        destination = read_rgb565(dst);
    } else {
        destination = read_rgb888(dst);
    }

    rgba8_t result = blend_over(destination, source);
    if (output_format == GRAPE_PIXEL_FORMAT_RGB565) {
        write_rgb565(dst, result);
    } else {
        write_rgb888(dst, result);
    }
}

static inline void affine_row_start(const grape_surface_t *surface, int32_t x, int32_t y,
                                    float *local_x, float *local_y)
{
    float screen_x = (float)x + 0.5f;
    float screen_y = (float)y + 0.5f;

    *local_x = surface->local_x_from_screen_x * screen_x +
               surface->local_x_from_screen_y * screen_y +
               surface->local_x_offset;

    *local_y = surface->local_y_from_screen_x * screen_x +
               surface->local_y_from_screen_y * screen_y +
               surface->local_y_offset;
}

static void raster_surface_a8(grape_context_t *context, const grape_surface_t *surface,
                              grape_rect_t damage_rect, grape_rect_t clipped, size_t bpp)
{
    const grape_texture_t *texture = surface->texture;
    const float texture_width = (float)texture->width;
    const float texture_height = (float)texture->height;
    (void)damage_rect;

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        float local_x;
        float local_y;
        affine_row_start(surface, clipped.x, y, &local_x, &local_y);

        uint8_t *dst = render_target_pixel(context, clipped.x, y, bpp);

        for (int32_t x = 0; x < clipped.width; ++x) {
            if (local_x >= 0.0f && local_y >= 0.0f &&
                local_x < texture_width && local_y < texture_height) {

                int32_t tx = (int32_t)local_x;
                int32_t ty = (int32_t)local_y;
                const uint8_t *row = texture->pixels + (size_t)ty * texture->stride;
                uint8_t alpha = row[tx];

                if (alpha != 0) {
                    rgba8_t source = {
                        .r = surface->tint.r,
                        .g = surface->tint.g,
                        .b = surface->tint.b,
                        .a = mul8(mul8(alpha, surface->tint.a), surface->opacity),
                    };
                    composite_source_pixel(dst, context->display_info.format, source);
                }
            }

            local_x += surface->local_x_from_screen_x;
            local_y += surface->local_y_from_screen_x;
            dst += bpp;
        }
    }
}

static void raster_surface_rgb565(grape_context_t *context, const grape_surface_t *surface,
                                  grape_rect_t damage_rect, grape_rect_t clipped, size_t bpp)
{
    const grape_texture_t *texture = surface->texture;
    const float texture_width = (float)texture->width;
    const float texture_height = (float)texture->height;
    (void)damage_rect;
    const uint8_t surface_alpha = mul8(surface->opacity, surface->tint.a);

    if (surface_alpha == 0) {
        return;
    }

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        float local_x;
        float local_y;
        affine_row_start(surface, clipped.x, y, &local_x, &local_y);

        uint8_t *dst = render_target_pixel(context, clipped.x, y, bpp);

        for (int32_t x = 0; x < clipped.width; ++x) {
            if (local_x >= 0.0f && local_y >= 0.0f &&
                local_x < texture_width && local_y < texture_height) {

                int32_t tx = (int32_t)local_x;
                int32_t ty = (int32_t)local_y;
                const uint8_t *row = texture->pixels + (size_t)ty * texture->stride;
                uint16_t pixel = ((const uint16_t *)row)[tx];

                uint8_t r5 = (uint8_t)((pixel >> 11) & 0x1F);
                uint8_t g6 = (uint8_t)((pixel >> 5) & 0x3F);
                uint8_t b5 = (uint8_t)(pixel & 0x1F);

                rgba8_t source = {
                    .r = mul8((uint8_t)((r5 << 3) | (r5 >> 2)), surface->tint.r),
                    .g = mul8((uint8_t)((g6 << 2) | (g6 >> 4)), surface->tint.g),
                    .b = mul8((uint8_t)((b5 << 3) | (b5 >> 2)), surface->tint.b),
                    .a = surface_alpha,
                };

                composite_source_pixel(dst, context->display_info.format, source);
            }

            local_x += surface->local_x_from_screen_x;
            local_y += surface->local_y_from_screen_x;
            dst += bpp;
        }
    }
}

static void raster_surface_rgb888(grape_context_t *context, const grape_surface_t *surface,
                                  grape_rect_t damage_rect, grape_rect_t clipped, size_t bpp)
{
    const grape_texture_t *texture = surface->texture;
    const float texture_width = (float)texture->width;
    const float texture_height = (float)texture->height;
    (void)damage_rect;
    const uint8_t surface_alpha = mul8(surface->opacity, surface->tint.a);

    if (surface_alpha == 0) {
        return;
    }

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        float local_x;
        float local_y;
        affine_row_start(surface, clipped.x, y, &local_x, &local_y);

        uint8_t *dst = render_target_pixel(context, clipped.x, y, bpp);

        for (int32_t x = 0; x < clipped.width; ++x) {
            if (local_x >= 0.0f && local_y >= 0.0f &&
                local_x < texture_width && local_y < texture_height) {

                int32_t tx = (int32_t)local_x;
                int32_t ty = (int32_t)local_y;
                const uint8_t *row = texture->pixels + (size_t)ty * texture->stride;
                const uint8_t *pixel = row + (size_t)tx * 3U;

                rgba8_t source = {
                    .r = mul8(pixel[0], surface->tint.r),
                    .g = mul8(pixel[1], surface->tint.g),
                    .b = mul8(pixel[2], surface->tint.b),
                    .a = surface_alpha,
                };

                composite_source_pixel(dst, context->display_info.format, source);
            }

            local_x += surface->local_x_from_screen_x;
            local_y += surface->local_y_from_screen_x;
            dst += bpp;
        }
    }
}

static void raster_surface_cpu(grape_context_t *context, const grape_surface_t *surface,
                               grape_rect_t damage_rect, grape_rect_t clipped, size_t bpp)
{
    switch (surface->texture->format) {
        case GRAPE_PIXEL_FORMAT_A8:
            raster_surface_a8(context, surface, damage_rect, clipped, bpp);
            break;
        case GRAPE_PIXEL_FORMAT_RGB565:
            raster_surface_rgb565(context, surface, damage_rect, clipped, bpp);
            break;
        case GRAPE_PIXEL_FORMAT_RGB888:
            raster_surface_rgb888(context, surface, damage_rect, clipped, bpp);
            break;
        default:
            break;
    }
}

static esp_err_t raster_surface_three_shear_a8(
    grape_context_t *context,
    const grape_surface_t *surface,
    grape_rect_t damage_rect,
    grape_rect_t clipped,
    size_t bpp,
    bool *handled
)
{
    if (!handled) {
        return ESP_ERR_INVALID_ARG;
    }

    *handled = false;

    grape_shear_image_t image;
    esp_err_t ret = grape_shear_rotate_a8(context, surface, &image);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    *handled = true;

    if (surface->tint.a == 0 || surface->opacity == 0) {
        return ESP_OK;
    }

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_SHEAR_COMPOSITE
    int64_t shear_composite_start_us = grape_profile_timestamp();
#endif

    bool ppa_handled = false;
    ret = grape_ppa_blend_a8_image(
        context,
        image.pixels,
        image.width,
        image.height,
        surface->transform.x + image.left,
        surface->transform.y + image.top,
        damage_rect,
        surface->tint,
        surface->opacity,
        &ppa_handled
    );
    if (ret != ESP_OK) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_SHEAR_COMPOSITE
        grape_profile_record(GRAPE_PROFILE_METRIC_SHEAR_COMPOSITE,
                             grape_profile_timestamp() - shear_composite_start_us);
#endif
        return ret;
    }

    if (ppa_handled) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_SHEAR_COMPOSITE
        grape_profile_record(GRAPE_PROFILE_METRIC_SHEAR_COMPOSITE,
                             grape_profile_timestamp() - shear_composite_start_us);
#endif
        return ESP_OK;
    }

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        float local_y =
            ((float)y + 0.5f) -
            surface->transform.y;

        int64_t source_y =
            (int64_t)floorf(local_y - image.top);

        if (source_y < 0 || source_y >= (int64_t)image.height) {
            continue;
        }

        float first_local_x =
            ((float)clipped.x + 0.5f) -
            surface->transform.x;

        int64_t source_x =
            (int64_t)floorf(first_local_x - image.left);
        int64_t destination_x = 0;

        if (source_x < 0) {
            destination_x = -source_x;
            source_x = 0;
        }

        if (destination_x >= clipped.width ||
            source_x >= (int64_t)image.width) {
            continue;
        }

        size_t available_destination =
            (size_t)clipped.width - (size_t)destination_x;
        size_t available_source =
            (size_t)image.width - (size_t)source_x;
        size_t pixel_count =
            available_destination < available_source
                ? available_destination
                : available_source;

        const uint8_t *source =
            image.pixels +
            (size_t)source_y * image.stride +
            (size_t)source_x;

        uint8_t *destination = render_target_pixel(
            context,
            clipped.x + (int32_t)destination_x,
            y,
            bpp
        );

        for (size_t i = 0; i < pixel_count; ++i) {
            uint8_t alpha = source[i];
            if (alpha != 0) {
                rgba8_t color = {
                    .r = surface->tint.r,
                    .g = surface->tint.g,
                    .b = surface->tint.b,
                    .a = mul8(
                        mul8(alpha, surface->tint.a),
                        surface->opacity
                    ),
                };
                composite_source_pixel(
                    destination,
                    context->display_info.format,
                    color
                );
            }

            destination += bpp;
        }
    }

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_SHEAR_COMPOSITE
    grape_profile_record(GRAPE_PROFILE_METRIC_SHEAR_COMPOSITE,
                         grape_profile_timestamp() - shear_composite_start_us);
#endif

    return ESP_OK;
}

static void fill_background_cpu(grape_context_t *context, grape_rect_t rect, size_t bpp, rgba8_t background)
{
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        uint8_t *dst = render_target_pixel(context, rect.x, y, bpp);
        for (int32_t x = 0; x < rect.width; ++x) {

            if (context->display_info.format == GRAPE_PIXEL_FORMAT_RGB565) {
                write_rgb565(dst, background);
            } else {
                write_rgb888(dst, background);
            }
            dst += bpp;
        }
    }
}

esp_err_t grape_compositor_render(grape_context_t *context, grape_rect_t rect)
{
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_COMPOSITOR
    int64_t compositor_start_us = grape_profile_timestamp();
#endif

    if (grape_rect_empty(rect)) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_COMPOSITOR
        grape_profile_record(GRAPE_PROFILE_METRIC_COMPOSITOR, grape_profile_timestamp() - compositor_start_us);
#endif
        return ESP_OK;
    }

    size_t bpp = grape_bytes_per_pixel(context->display_info.format);
    if (!context->render_target.pixels ||
        context->render_target.format != context->display_info.format ||
        context->render_target.width != context->display_info.width ||
        context->render_target.height != context->display_info.height ||
        rect.x < 0 || rect.y < 0 ||
        (uint32_t)(rect.x + rect.width) > context->render_target.width ||
        (uint32_t)(rect.y + rect.height) > context->render_target.height) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_COMPOSITOR
        grape_profile_record(GRAPE_PROFILE_METRIC_COMPOSITOR, grape_profile_timestamp() - compositor_start_us);
#endif
        return ESP_ERR_INVALID_STATE;
    }

    rgba8_t background = {
        .r = context->background.r,
        .g = context->background.g,
        .b = context->background.b,
        .a = 255,
    };

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_PPA_FILL
    int64_t ppa_fill_start_us = grape_profile_timestamp();
#endif
    esp_err_t ret = grape_ppa_fill(context, rect, context->background);
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_PPA_FILL
    grape_profile_record(GRAPE_PROFILE_METRIC_PPA_FILL, grape_profile_timestamp() - ppa_fill_start_us);
#endif

    if (ret == ESP_ERR_NOT_SUPPORTED) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_CPU_FILL
        int64_t cpu_fill_start_us = grape_profile_timestamp();
#endif
        fill_background_cpu(context, rect, bpp, background);
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_CPU_FILL
        grape_profile_record(GRAPE_PROFILE_METRIC_CPU_FILL, grape_profile_timestamp() - cpu_fill_start_us);
#endif
    } else if (ret != ESP_OK) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_COMPOSITOR
        grape_profile_record(GRAPE_PROFILE_METRIC_COMPOSITOR, grape_profile_timestamp() - compositor_start_us);
#endif
        return ret;
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

        bool handled = false;

        if (context->rotation_backend == GRAPE_ROTATION_BACKEND_AUTO) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_PPA_BLEND_DISPATCH
            int64_t ppa_blend_dispatch_start_us = grape_profile_timestamp();
#endif
            ret = grape_ppa_blend_surface(context, surface, rect, &handled);
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_PPA_BLEND_DISPATCH
            grape_profile_record(GRAPE_PROFILE_METRIC_PPA_BLEND_DISPATCH,
                                 grape_profile_timestamp() - ppa_blend_dispatch_start_us);
#endif
            if (ret != ESP_OK) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_COMPOSITOR
                grape_profile_record(GRAPE_PROFILE_METRIC_COMPOSITOR, grape_profile_timestamp() - compositor_start_us);
#endif
                return ret;
            }
            if (handled) {
                continue;
            }
        }

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_CPU_SURFACE_RASTER
        int64_t cpu_surface_start_us = grape_profile_timestamp();
#endif

        if (context->rotation_backend == GRAPE_ROTATION_BACKEND_THREE_SHEAR) {
            ret = raster_surface_three_shear_a8(
                context,
                surface,
                rect,
                clipped,
                bpp,
                &handled
            );
            if (ret != ESP_OK) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_CPU_SURFACE_RASTER
                grape_profile_record(GRAPE_PROFILE_METRIC_CPU_SURFACE_RASTER,
                                     grape_profile_timestamp() - cpu_surface_start_us);
#endif
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_COMPOSITOR
                grape_profile_record(GRAPE_PROFILE_METRIC_COMPOSITOR, grape_profile_timestamp() - compositor_start_us);
#endif
                return ret;
            }
        }

        if (!handled) {
            raster_surface_cpu(context, surface, rect, clipped, bpp);
        }

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_CPU_SURFACE_RASTER
        grape_profile_record(GRAPE_PROFILE_METRIC_CPU_SURFACE_RASTER,
                             grape_profile_timestamp() - cpu_surface_start_us);
#endif
    }

    grape_debug_render(context, rect);

#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_COMPOSITOR
    grape_profile_record(GRAPE_PROFILE_METRIC_COMPOSITOR, grape_profile_timestamp() - compositor_start_us);
#endif

    return ESP_OK;
}
