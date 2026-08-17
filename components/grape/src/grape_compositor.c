#include <math.h>

#include "grape_internal.h"
#include "grape_shader_runtime.h"

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} rgba8_t;

/**
 * Computes the memory address of pixel (x, y) inside the current
 * render target
 *
 * @param context GRAPE context
 * @param x Pixel x coordinate
 * @param y Pixel y coordinate
 * @param bpp Bytes per pixel
 * @return Memory address of pixel inside the render target
 */
static inline uint8_t *target_pixel_address(grape_context_t *context,
                                           int32_t x,
                                           int32_t y,
                                           size_t bpp)
{
    return (uint8_t *)context->render_target.pixels +
           (size_t)y * context->render_target.stride +
           (size_t)x * bpp;
}

/**
 * Multiplies 2 8-bit normalized (0-255) values to
 * produce an 8-bit normalized value (0-255)
 *
 * @param a First 8-bit normalized value
 * @param b Second 8-bit normalized value
 * @return Result of normalized 8-bit multiplication
 */
static inline uint8_t mul8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint16_t)a * b + 127U) / 255U);
}

/**
 * Blend an RGBA8 source over an opaque RGB destination
 *
 * @param dst Background color
 * @param src Color getting blended over the background color
 * @return Blended color
 */
static __attribute__((always_inline)) inline rgba8_t blend_over(rgba8_t dst, rgba8_t src)
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

/**
 * Converts an RGBA8 color into raw RGB565 bytes
 *
 * @param dst Destination bytes
 * @param color RGB565 color
 */
static inline void write_rgb565(uint8_t *dst, rgba8_t color)
{
    uint16_t pixel = (uint16_t)(((color.r >> 3) << 11) | ((color.g >> 2) << 5) | (color.b >> 3));
    dst[0] = (uint8_t)(pixel & 0xFF);
    dst[1] = (uint8_t)(pixel >> 8);
}

/**
 * Converts an RGBA8 color into raw RGB888 bytes
 *
 * @param dst Destination bytes
 * @param color RGB8 color
 */
static inline void write_rgb888(uint8_t *dst, rgba8_t color)
{
    dst[0] = color.r;
    dst[1] = color.g;
    dst[2] = color.b;
}

/**
 * Converts raw RGB565 bytes into an RGBA8 color
 *
 * @param src Raw color bytes
 * @return RGB565 color
 */
static __attribute__((always_inline)) inline rgba8_t read_rgb565(const uint8_t *src)
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

/**
 * Converts raw RGB888 bytes into an RGBA8 color
 *
 * @param src Raw color bytes
 * @return RGB8 color
 */
static inline rgba8_t read_rgb888(const uint8_t *src)
{
    return (rgba8_t) {
        .r = src[0],
        .g = src[1],
        .b = src[2],
        .a = 255,
    };
}

/**
 * Composites an RGBA source pixel over a framebuffer pixel.
 *
 * @param dst Destination pixel in bytes
 * @param output_format Output color format
 * @param source Source pixel to composite
 */
static __attribute__((always_inline)) inline void composite_source_pixel(uint8_t *dst, grape_pixel_format_t output_format, rgba8_t source)
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

/**
 * Maps the center of a screen pixel into a surface's local texture
 * coordinates using the cached inverse affine transform.
 *
 * This is evaluated once at the beginning of each raster row. Subsequent
 * pixels advance the local coordinates using the cached horizontal
 * transform coefficients.
 *
 * @param surface The surface being rendered
 * @param x Screen X coordinate
 * @param y Screen Y coordinate
 * @param local_x Returned local texture X coordinate
 * @param local_y Returned local texture Y coordinate
 */
static __attribute__((always_inline)) inline void affine_row_start(const grape_surface_t *surface, int32_t x, int32_t y,
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

/**
 * Rasters and applies tint to an A8 surface using
 * Inverse Affine Transform (see /docs/MATH.md#affine-rotation)
 *
 * WARNING: the reason that these functions are separate
 *          is to save CPU cycles. We DO NOT want to do if else
 *          here because that would be too expensive to do hundreds
 *          of thousands of times per frame.
 *
 * @param context GRAPE context
 * @param surface Surface to raster
 * @param damage_rect Damage rectangle (currently unused)
 * @param clipped Intersection of the dirty region and the surface's bounds
 * @param bpp Bytes per pixel in the render target format
 */
static void raster_surface_a8(grape_context_t *context, const grape_surface_t *surface,
                              grape_rect_t damage_rect, grape_rect_t clipped, size_t bpp)
{
    const grape_texture_t *texture = surface->texture;
    const float texture_width = (float)texture->width;
    const float texture_height = (float)texture->height;
    const int32_t raster_width = clipped.width;
    const float local_x_step = surface->local_x_from_screen_x;
    const float local_y_step = surface->local_y_from_screen_x;
    const uint8_t *texture_pixels = texture->pixels;
    const size_t texture_stride = texture->stride;
    const uint8_t tint_r = surface->tint.r;
    const uint8_t tint_g = surface->tint.g;
    const uint8_t tint_b = surface->tint.b;
    const uint8_t tint_a = surface->tint.a;
    const uint8_t opacity = surface->opacity;
    const grape_pixel_format_t output_format = context->display_info.format;
    (void)damage_rect;

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) { // Iterate over rows
        float local_x;
        float local_y;
        affine_row_start(surface, clipped.x, y, &local_x, &local_y);

        uint8_t *dst = target_pixel_address(context, clipped.x, y, bpp);

        for (int32_t x = 0; x < raster_width; ++x) { // Iterate over columns
            if (local_x >= 0.0f && local_y >= 0.0f &&
                local_x < texture_width && local_y < texture_height) { // If in bounds

                // Convert floating point local coordinates to texture coordinates
                int32_t tx = (int32_t)local_x;
                int32_t ty = (int32_t)local_y;
                // and find the corresponding texel in memory
                const uint8_t *row = texture_pixels + (size_t)ty * texture_stride;
                uint8_t alpha = row[tx];

                if (alpha != 0) {
                    rgba8_t source = {
                        .r = tint_r,
                        .g = tint_g,
                        .b = tint_b,
                        .a = mul8(mul8(alpha, tint_a), opacity),
                    }; // Apply tint and transparency
                    composite_source_pixel(dst, output_format, source);
                }
            }

            // Move the local coordinates by the affine transform constants
            local_x += local_x_step;
            local_y += local_y_step;
            dst += bpp;
        }
    }
}

/**
 * Rasters an RGB565 surface using
 * Inverse Affine Transform (see /docs/MATH.md#affine-rotation)
 *
 * WARNING: the reason that these functions are separate
 *          is to save CPU cycles. We DO NOT want to do if else
 *          here because that would be too expensive to do hundreds
 *          of thousands of times per frame.
 *
 * @param context GRAPE context
 * @param surface Surface to raster
 * @param damage_rect Damage rectangle (currently unused)
 * @param clipped Intersection of the dirty region and the surface's bounds
 * @param bpp Bytes per pixel in the render target format
 */
static void raster_surface_rgb565(grape_context_t *context, const grape_surface_t *surface,
                                  grape_rect_t damage_rect, grape_rect_t clipped, size_t bpp)
{
    const grape_texture_t *texture = surface->texture;
    const float texture_width = (float)texture->width;
    const float texture_height = (float)texture->height;
    const int32_t raster_width = clipped.width;
    const float local_x_step = surface->local_x_from_screen_x;
    const float local_y_step = surface->local_y_from_screen_x;
    const uint8_t *texture_pixels = texture->pixels;
    const size_t texture_stride = texture->stride;
    const uint8_t tint_r = surface->tint.r;
    const uint8_t tint_g = surface->tint.g;
    const uint8_t tint_b = surface->tint.b;
    const grape_pixel_format_t output_format = context->display_info.format;
    (void)damage_rect;
    const uint8_t surface_alpha = mul8(surface->opacity, surface->tint.a);

    if (surface_alpha == 0) {
        return;
    }

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) { // Iterate over rows
        float local_x;
        float local_y;
        affine_row_start(surface, clipped.x, y, &local_x, &local_y);

        uint8_t *dst = target_pixel_address(context, clipped.x, y, bpp);

        for (int32_t x = 0; x < raster_width; ++x) { // Iterate over columns
            if (local_x >= 0.0f && local_y >= 0.0f &&
                local_x < texture_width && local_y < texture_height) { // If in bounds

                // Convert floating point local coordinates to texture coordinates
                int32_t tx = (int32_t)local_x;
                int32_t ty = (int32_t)local_y;
                // and find the corresponding texel in memory
                const uint8_t *row = texture_pixels + (size_t)ty * texture_stride;
                uint16_t pixel = ((const uint16_t *)row)[tx];

                // Split RGB565 pixel into its R5, G6 and B5 components
                uint8_t r5 = (uint8_t)((pixel >> 11) & 0x1F);
                uint8_t g6 = (uint8_t)((pixel >> 5) & 0x3F);
                uint8_t b5 = (uint8_t)(pixel & 0x1F);

                rgba8_t source = {
                    .r = mul8((uint8_t)((r5 << 3) | (r5 >> 2)), tint_r),
                    .g = mul8((uint8_t)((g6 << 2) | (g6 >> 4)), tint_g),
                    .b = mul8((uint8_t)((b5 << 3) | (b5 >> 2)), tint_b),
                    .a = surface_alpha,
                }; // Apply tint and transparency

                composite_source_pixel(dst, output_format, source);
            }

            // Move the local coordinates by the affine transform constants
            local_x += local_x_step;
            local_y += local_y_step;
            dst += bpp;
        }
    }
}

/**
 * Rasters an RGB888 surface using
 * Inverse Affine Transform (see /docs/MATH.md#affine-rotation)
 *
 * WARNING: the reason that these functions are separate
 *          is to save CPU cycles. We DO NOT want to do if else
 *          here because that would be too expensive to do hundreds
 *          of thousands of times per frame.
 *
 * @param context GRAPE context
 * @param surface Surface to raster
 * @param damage_rect Damage rectangle (currently unused)
 * @param clipped Intersection of the dirty region and the surface's bounds
 * @param bpp Bytes per pixel in the render target format
 */
static void raster_surface_rgb888(grape_context_t *context, const grape_surface_t *surface,
                                  grape_rect_t damage_rect, grape_rect_t clipped, size_t bpp)
{
    const grape_texture_t *texture = surface->texture;
    const float texture_width = (float)texture->width;
    const float texture_height = (float)texture->height;
    const int32_t raster_width = clipped.width;
    const float local_x_step = surface->local_x_from_screen_x;
    const float local_y_step = surface->local_y_from_screen_x;
    const uint8_t *texture_pixels = texture->pixels;
    const size_t texture_stride = texture->stride;
    const uint8_t tint_r = surface->tint.r;
    const uint8_t tint_g = surface->tint.g;
    const uint8_t tint_b = surface->tint.b;
    const grape_pixel_format_t output_format = context->display_info.format;
    (void)damage_rect;
    const uint8_t surface_alpha = mul8(surface->opacity, surface->tint.a);

    if (surface_alpha == 0) {
        return;
    }

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) { // Iterate over rows
        float local_x;
        float local_y;
        affine_row_start(surface, clipped.x, y, &local_x, &local_y);

        uint8_t *dst = target_pixel_address(context, clipped.x, y, bpp);

        for (int32_t x = 0; x < raster_width; ++x) { // Iterate over columns
            if (local_x >= 0.0f && local_y >= 0.0f &&
                local_x < texture_width && local_y < texture_height) { // If in bounds

                // Convert floating point local coordinates to texture coordinates
                int32_t tx = (int32_t)local_x;
                int32_t ty = (int32_t)local_y;
                // and find the corresponding texel in memory
                const uint8_t *row = texture_pixels + (size_t)ty * texture_stride;
                const uint8_t *pixel = row + (size_t)tx * 3U;

                rgba8_t source = {
                    .r = mul8(pixel[0], tint_r),
                    .g = mul8(pixel[1], tint_g),
                    .b = mul8(pixel[2], tint_b),
                    .a = surface_alpha,
                }; // Apply tint and transparency

                composite_source_pixel(dst, output_format, source);
            }

            // Move the local coordinates by the affine transform constants
            local_x += local_x_step;
            local_y += local_y_step;
            dst += bpp;
        }
    }
}

/**
 * Rasters an RGBA8888 surface using Inverse Affine Transform
 * (see /docs/MATH.md#affine-rotation).
 *
 * RGBA8888 textures store straight (non-premultiplied) alpha. Texture alpha,
 * tint alpha and surface opacity are multiplied together before the source is
 * composited over the opaque display render target.
 */
static void raster_surface_rgba8888(grape_context_t *context, const grape_surface_t *surface,
                                    grape_rect_t damage_rect, grape_rect_t clipped, size_t bpp)
{
    const grape_texture_t *texture = surface->texture;
    const float texture_width = (float)texture->width;
    const float texture_height = (float)texture->height;
    const int32_t raster_width = clipped.width;
    const float local_x_step = surface->local_x_from_screen_x;
    const float local_y_step = surface->local_y_from_screen_x;
    const uint8_t *texture_pixels = texture->pixels;
    const size_t texture_stride = texture->stride;
    const uint8_t tint_r = surface->tint.r;
    const uint8_t tint_g = surface->tint.g;
    const uint8_t tint_b = surface->tint.b;
    const uint8_t surface_alpha = mul8(surface->opacity, surface->tint.a);
    const grape_pixel_format_t output_format = context->display_info.format;
    (void)damage_rect;

    if (surface_alpha == 0U) {
        return;
    }

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        float local_x;
        float local_y;
        affine_row_start(surface, clipped.x, y, &local_x, &local_y);

        uint8_t *dst = target_pixel_address(context, clipped.x, y, bpp);

        for (int32_t x = 0; x < raster_width; ++x) {
            if (local_x >= 0.0f && local_y >= 0.0f &&
                local_x < texture_width && local_y < texture_height) {
                int32_t tx = (int32_t)local_x;
                int32_t ty = (int32_t)local_y;
                const uint8_t *row = texture_pixels + (size_t)ty * texture_stride;
                const uint8_t *pixel = row + (size_t)tx * 4U;
                uint8_t alpha = mul8(pixel[3], surface_alpha);

                if (alpha != 0U) {
                    rgba8_t source = {
                        .r = mul8(pixel[0], tint_r),
                        .g = mul8(pixel[1], tint_g),
                        .b = mul8(pixel[2], tint_b),
                        .a = alpha,
                    };
                    composite_source_pixel(dst, output_format, source);
                }
            }

            local_x += local_x_step;
            local_y += local_y_step;
            dst += bpp;
        }
    }
}

/**
 * Rasters any given surface using Inverse Affine Transform (see /docs/MATH.md#affine-rotation)
 *
 * @param context GRAPE context
 * @param surface Surface to raster
 * @param damage_rect Damage rectangle
 * @param clipped Intersection of the dirty region and the surface's bounds
 * @param bpp Bytes per pixel in the render target format
 */
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
        case GRAPE_PIXEL_FORMAT_RGBA8888:
            raster_surface_rgba8888(context, surface, damage_rect, clipped, bpp);
            break;
        default:
            break;
    }
}

/**
 * Rasterizes an A8 surface using the three-shear method of rotation
 * (See /docs/MATH.md#three-shear-rotation)
 *
 * NOTICE: This currently only does A8 and fallback to affine for any other format
 *
 * TODO: Implement and benchmark three-shear for other formats and add a
 *       cost-evaluation for auto backend if it's slower than affine in some cases
 *
 * @param context GRAPE context
 * @param surface Surface to raster
 * @param damage_rect Damage rectangle
 * @param clipped Intersection of the dirty region and the surface's bounds
 * @param bpp Bytes per pixel in the render target format
 * @param handled Returns true if we handled the rasterization and false if we didn't. Currently only works for A8
 * @return
 */
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

    // Attempt to rotate using the three-shear method (See /docs/MATH.md#three-shear-rotation)
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

    GRAPE_TIME_SCOPE(SHEAR_COMPOSITE);

    // Attempt to use the PPA to blend the image
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
        return ret;
    }

    if (ppa_handled) {
        return ESP_OK;
    }

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) { // Iterate over rows
        // Find corresponding row in the rotated image
        float local_y =
            ((float)y + 0.5f) -
            surface->transform.y;

        float source_y_f = local_y - image.top;

        // See /docs/PERFORMANCE.md#avoid-int64_t-floorf-in-hot-paths
        // Skip the row before converting: non-negative float-to-int truncation is floor.
        if (source_y_f < 0.0f || source_y_f >= (float)image.height) {
            continue;
        }
        int32_t source_y = (int32_t)source_y_f;

        // Find corresponding starting x in the rotated image
        float first_local_x =
            ((float)clipped.x + 0.5f) -
            surface->transform.x;

        // See /docs/PERFORMANCE.md#avoid-int64_t-floorf-in-hot-paths
        int32_t source_x =
            grape_floor_to_i32(first_local_x - image.left);
        int32_t destination_x = 0;

        if (source_x < 0) {
            destination_x = -source_x;
            source_x = 0;
        }

        if ((uint32_t)destination_x >= (uint32_t)clipped.width ||
            (uint32_t)source_x >= image.width) {
            continue;
        }

        // Calculates the amount of pixels safe to process before
        // running into the destination space edge or the source
        // image edge
        size_t available_destination =
            (size_t)clipped.width - (size_t)destination_x;
        size_t available_source =
            (size_t)image.width - (size_t)source_x;
        size_t pixel_count =
            available_destination < available_source
                ? available_destination
                : available_source;

        // Calculate the starting source and destination pixel addresses
        const uint8_t *source =
            image.pixels +
            (size_t)source_y * image.stride +
            (size_t)source_x;

        uint8_t *destination = target_pixel_address(
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
                }; // Apply tint and transparency
                composite_source_pixel(
                    destination,
                    context->display_info.format,
                    color
                );
            }

            destination += bpp;
        }
    }

    return ESP_OK;
}

/**
 * Fills a rectangle with a certain color on the CPU
 *
 * NOTICE: only use this as fallback if the PPA does not support it or is slower
 *
 * @param context GRAPE context
 * @param rect Rect to fill
 * @param bpp Bytes per pixel in the output color format
 * @param background Background color to fill
 */
static void fill_background_cpu(grape_context_t *context, grape_rect_t rect, size_t bpp, rgba8_t background)
{
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        uint8_t *dst = target_pixel_address(context, rect.x, y, bpp);
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

/**
 * Renders a rectangle onto the screen
 *
 * @param context GRAPE context
 * @param rect rect to composite
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_compositor_render(grape_context_t *context, grape_rect_t rect)
{
    GRAPE_TIME_SCOPE(COMPOSITOR);

    if (grape_rect_empty(rect)) {
        return ESP_OK;
    }

    size_t bpp = grape_bytes_per_pixel(context->display_info.format);
    if (!context->render_target.pixels ||                                   // Check if the pixel buffer exists
        context->render_target.format != context->display_info.format ||    // Check if the render target format is the same as the display format
        context->render_target.width != context->display_info.width ||      // Check if the render target width matches display width
        context->render_target.height != context->display_info.height ||    // Same for height
        rect.x < 0 || rect.y < 0 ||                                         // Check if coordinates are not outside of the screen
        (uint32_t)(rect.x + rect.width) > context->render_target.width ||   // Same thing
        (uint32_t)(rect.y + rect.height) > context->render_target.height) { // Same thing
        return ESP_ERR_INVALID_STATE;
    }

    rgba8_t background = {
        .r = context->background.r,
        .g = context->background.g,
        .b = context->background.b,
        .a = 255,
    };

    // Attempt to fill the rectangle using the PPA, fallback to CPU
    esp_err_t ret = ESP_FAIL;
    GRAPE_TIME_BLOCK(PPA_FILL) {
        ret = grape_ppa_fill(context, rect, context->background);
    }

    if (ret == ESP_ERR_NOT_SUPPORTED) {
        GRAPE_TIME_BLOCK(CPU_FILL) {
            fill_background_cpu(context, rect, bpp, background);
        }
    } else if (ret != ESP_OK) {
        return ret;
    }

    // Composite surfaces (Z-ordered)
    for (grape_surface_t *surface = context->surfaces;
         surface;
         surface = surface->next) {

        // Check if the surface is even visible
        if (!surface->visible ||
            surface->opacity == 0 ||
            (!surface->texture && !surface->shader)) {
            continue;
        }

        // Crop the damage rectangle to the surface's bounds
        grape_rect_t clipped =
            grape_rect_intersection(
                rect,
                surface->bounds
            );

        if (grape_rect_empty(clipped)) {
            continue;
        }

        // If the shader exists...
        if (surface->shader) {
            grape_shader_kernel_args_t shader_args = {
                .texture_pixels = surface->texture ? surface->texture->pixels : NULL,
                .texture_stride = surface->texture ? surface->texture->stride : 0U,
                .texture_width = surface->texture ? surface->texture->width : 0U,
                .texture_height = surface->texture ? surface->texture->height : 0U,
                .texture_format = surface->texture ? surface->texture->format : GRAPE_PIXEL_FORMAT_A8,
                .surface_width = surface->width,
                .surface_height = surface->height,
                .target_pixels = context->render_target.pixels,
                .target_stride = context->render_target.stride,
                .target_format = context->display_info.format,
                .clipped = clipped,
                .tint = surface->tint,
                .opacity = surface->opacity,
                .local_x_from_screen_x = surface->local_x_from_screen_x,
                .local_x_from_screen_y = surface->local_x_from_screen_y,
                .local_x_offset = surface->local_x_offset,
                .local_y_from_screen_x = surface->local_y_from_screen_x,
                .local_y_from_screen_y = surface->local_y_from_screen_y,
                .local_y_offset = surface->local_y_offset,
            }; // Set all arguments
            GRAPE_TIME_BLOCK(CPU_SURFACE_RASTER) {
                surface->shader->kernel(&shader_args, surface->shader_uniforms); // Run shader inside a timing block
            }
            continue;
        }

        bool handled = false;

        // Attempt to raster the surface using three shear method if backend is set to auto, fallback to CPU
        if (context->rotation_backend == GRAPE_ROTATION_BACKEND_AUTO) {
            GRAPE_TIME_BLOCK(PPA_BLEND_DISPATCH) {
                ret = grape_ppa_blend_surface(context, surface, rect, &handled);
            }
            if (ret != ESP_OK) {
                return ret;
            }
            if (handled) {
                continue;
            }
        }

        GRAPE_TIME_BLOCK(CPU_SURFACE_RASTER) {
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
                    return ret;
                }
            }

            if (!handled) {
                raster_surface_cpu(context, surface, rect, clipped, bpp);
            }
        }
    }

    // Render debug overlays
    grape_debug_render(context, rect);
    return ESP_OK;
}
