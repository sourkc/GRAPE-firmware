#include <math.h>
#include <string.h>

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

static __attribute__((always_inline)) inline void affine_row_start(const grape_surface_t *surface, int32_t x, int32_t y,
                                    float *local_x, float *local_y);

static __attribute__((always_inline)) inline void composite_source_pixel(
    uint8_t *dst, grape_pixel_format_t output_format, rgba8_t source);

static inline rgba8_t read_rgba8888(const uint8_t *src)
{
    return (rgba8_t) {
        .r = src[0],
        .g = src[1],
        .b = src[2],
        .a = src[3],
    };
}

static inline uint8_t coverage_mul4(uint8_t alpha,
                                                                    uint8_t covered_samples)
{
    return (uint8_t)(((uint16_t)alpha * covered_samples + 2U) >> 2);
}

/**
 * Computes 4x rotated-grid coverage for the transformed surface rectangle.
 *
 * The common interior case exits after the cached min/max sample extents. Only
 * boundary pixels evaluate all four sample positions. For partial pixels the
 * returned shade point is the centroid of the covered samples, which keeps the
 * single texture/shader evaluation inside the primitive even when the pixel
 * centre itself is outside.
 */
static inline uint8_t surface_coverage_4x(
    const grape_surface_t *surface,
    float center_x,
    float center_y,
    float *shade_x,
    float *shade_y)
{
    const float width = (float)surface->width;
    const float height = (float)surface->height;
    const float min_x = center_x + surface->aa_local_min_dx;
    const float max_x = center_x + surface->aa_local_max_dx;
    const float min_y = center_y + surface->aa_local_min_dy;
    const float max_y = center_y + surface->aa_local_max_dy;

    if (min_x >= 0.0f && max_x < width &&
        min_y >= 0.0f && max_y < height) {
        *shade_x = center_x;
        *shade_y = center_y;
        return 4U;
    }

    if (max_x < 0.0f || min_x >= width ||
        max_y < 0.0f || min_y >= height) {
        return 0U;
    }

    float sum_x = 0.0f;
    float sum_y = 0.0f;
    uint8_t count = 0U;
    for (size_t i = 0U; i < 4U; ++i) {
        const float sample_x = center_x + surface->aa_local_dx[i];
        const float sample_y = center_y + surface->aa_local_dy[i];
        if (sample_x >= 0.0f && sample_y >= 0.0f &&
            sample_x < width && sample_y < height) {
            sum_x += sample_x;
            sum_y += sample_y;
            count++;
        }
    }

    if (count == 0U) {
        return 0U;
    }

    static const float reciprocal[5] = {0.0f, 1.0f, 0.5f, 1.0f / 3.0f, 0.25f};
    *shade_x = sum_x * reciprocal[count];
    *shade_y = sum_y * reciprocal[count];
    return count;
}

static inline void surface_map_texture_float(
    const grape_surface_t *surface,
    float local_x,
    float local_y,
    float *texture_x,
    float *texture_y)
{
    *texture_x = local_x * surface->texture_from_local_x_scale +
                 surface->texture_from_local_x_offset;
    *texture_y = local_y * surface->texture_from_local_y_scale +
                 surface->texture_from_local_y_offset;
}

static inline int32_t clamp_texture_index(int32_t value,
                                                                         int32_t limit)
{
    if (value < 0) {
        return 0;
    }
    if (value >= limit) {
        return limit - 1;
    }
    return value;
}

static inline int32_t wrap_texture_index(int32_t value,
                                                                        int32_t limit)
{
    int32_t wrapped = value % limit;
    return wrapped < 0 ? wrapped + limit : wrapped;
}

typedef struct {
    int32_t x0;
    int32_t x1;
    int32_t y0;
    int32_t y1;
    uint8_t valid_mask;
    uint32_t w00;
    uint32_t w10;
    uint32_t w01;
    uint32_t w11;
} bilinear_sample_t;

#define BILINEAR_VALID_00 0x01U
#define BILINEAR_VALID_10 0x02U
#define BILINEAR_VALID_01 0x04U
#define BILINEAR_VALID_11 0x08U

/**
 * Builds integer bilinear weights and resolves address semantics once per
 * destination pixel. STRETCH/COVER clamp, TILE wraps, and FIT/CENTER use a
 * transparent border so the contained/native image fades into its letterbox.
 */
static inline void build_bilinear_sample(
    const grape_surface_t *surface,
    float texture_x,
    float texture_y,
    bilinear_sample_t *sample)
{
    const float centered_x = texture_x - 0.5f;
    const float centered_y = texture_y - 0.5f;
    const int32_t source_x0 = grape_floor_to_i32(centered_x);
    const int32_t source_y0 = grape_floor_to_i32(centered_y);
    const int32_t source_x1 = source_x0 + 1;
    const int32_t source_y1 = source_y0 + 1;
    const float frac_x = centered_x - (float)source_x0;
    const float frac_y = centered_y - (float)source_y0;

    uint32_t fx = (uint32_t)(frac_x * 256.0f + 0.5f);
    uint32_t fy = (uint32_t)(frac_y * 256.0f + 0.5f);
    if (fx > 256U) fx = 256U;
    if (fy > 256U) fy = 256U;
    const uint32_t inv_x = 256U - fx;
    const uint32_t inv_y = 256U - fy;
    sample->w00 = inv_x * inv_y;
    sample->w10 = fx * inv_y;
    sample->w01 = inv_x * fy;
    sample->w11 = fx * fy;

    const int32_t width = (int32_t)surface->texture->width;
    const int32_t height = (int32_t)surface->texture->height;

    if (surface->texture_mode == GRAPE_SURFACE_TEXTURE_TILE) {
        sample->x0 = wrap_texture_index(source_x0, width);
        sample->x1 = wrap_texture_index(source_x1, width);
        sample->y0 = wrap_texture_index(source_y0, height);
        sample->y1 = wrap_texture_index(source_y1, height);
        sample->valid_mask = 0x0FU;
        return;
    }

    if (surface->texture_mode == GRAPE_SURFACE_TEXTURE_FIT ||
        surface->texture_mode == GRAPE_SURFACE_TEXTURE_CENTER) {
        const bool x0_valid = source_x0 >= 0 && source_x0 < width;
        const bool x1_valid = source_x1 >= 0 && source_x1 < width;
        const bool y0_valid = source_y0 >= 0 && source_y0 < height;
        const bool y1_valid = source_y1 >= 0 && source_y1 < height;
        sample->x0 = clamp_texture_index(source_x0, width);
        sample->x1 = clamp_texture_index(source_x1, width);
        sample->y0 = clamp_texture_index(source_y0, height);
        sample->y1 = clamp_texture_index(source_y1, height);
        sample->valid_mask =
            (uint8_t)((x0_valid && y0_valid ? BILINEAR_VALID_00 : 0U) |
                      (x1_valid && y0_valid ? BILINEAR_VALID_10 : 0U) |
                      (x0_valid && y1_valid ? BILINEAR_VALID_01 : 0U) |
                      (x1_valid && y1_valid ? BILINEAR_VALID_11 : 0U));
        return;
    }

    sample->x0 = clamp_texture_index(source_x0, width);
    sample->x1 = clamp_texture_index(source_x1, width);
    sample->y0 = clamp_texture_index(source_y0, height);
    sample->y1 = clamp_texture_index(source_y1, height);
    sample->valid_mask = 0x0FU;
}

static inline rgba8_t bilinear_mix_rgba(
    rgba8_t c00,
    rgba8_t c10,
    rgba8_t c01,
    rgba8_t c11,
    const bilinear_sample_t *sample)
{
    /*
     * Interpolate premultiplied channels even though GRAPE stores straight
     * alpha. This prevents dark fringes around transparent A8/RGBA edges and
     * around FIT/CENTER's transparent border. Convert back to straight alpha
     * once because the shader/compositor ABI is straight RGBA.
     */
    const uint8_t p00r = mul8(c00.r, c00.a);
    const uint8_t p00g = mul8(c00.g, c00.a);
    const uint8_t p00b = mul8(c00.b, c00.a);
    const uint8_t p10r = mul8(c10.r, c10.a);
    const uint8_t p10g = mul8(c10.g, c10.a);
    const uint8_t p10b = mul8(c10.b, c10.a);
    const uint8_t p01r = mul8(c01.r, c01.a);
    const uint8_t p01g = mul8(c01.g, c01.a);
    const uint8_t p01b = mul8(c01.b, c01.a);
    const uint8_t p11r = mul8(c11.r, c11.a);
    const uint8_t p11g = mul8(c11.g, c11.a);
    const uint8_t p11b = mul8(c11.b, c11.a);

#define BILINEAR_MIX_CHANNEL(a, b, c, d) \
    (uint8_t)(((uint32_t)(a) * sample->w00 + \
               (uint32_t)(b) * sample->w10 + \
               (uint32_t)(c) * sample->w01 + \
               (uint32_t)(d) * sample->w11 + 32768U) >> 16)

    const uint8_t alpha = BILINEAR_MIX_CHANNEL(c00.a, c10.a, c01.a, c11.a);
    if (alpha == 0U) {
        return (rgba8_t){0};
    }

    const uint8_t premul_r = BILINEAR_MIX_CHANNEL(p00r, p10r, p01r, p11r);
    const uint8_t premul_g = BILINEAR_MIX_CHANNEL(p00g, p10g, p01g, p11g);
    const uint8_t premul_b = BILINEAR_MIX_CHANNEL(p00b, p10b, p01b, p11b);
#undef BILINEAR_MIX_CHANNEL

    if (alpha == 255U) {
        return (rgba8_t){premul_r, premul_g, premul_b, 255U};
    }

    const uint32_t half_alpha = (uint32_t)alpha / 2U;
    uint32_t red = ((uint32_t)premul_r * 255U + half_alpha) / alpha;
    uint32_t green = ((uint32_t)premul_g * 255U + half_alpha) / alpha;
    uint32_t blue = ((uint32_t)premul_b * 255U + half_alpha) / alpha;
    if (red > 255U) red = 255U;
    if (green > 255U) green = 255U;
    if (blue > 255U) blue = 255U;
    return (rgba8_t){(uint8_t)red, (uint8_t)green, (uint8_t)blue, alpha};
}

static inline rgba8_t sample_raw_a8(
    const grape_surface_t *surface,
    int32_t x,
    int32_t y)
{
    const uint8_t *row = surface->texture->pixels + (size_t)y * surface->texture->stride;
    return (rgba8_t){
        surface->tint.r,
        surface->tint.g,
        surface->tint.b,
        mul8(row[x], surface->tint.a),
    };
}

static inline rgba8_t sample_raw_rgb565(
    const grape_surface_t *surface,
    int32_t x,
    int32_t y)
{
    const uint8_t *row = surface->texture->pixels + (size_t)y * surface->texture->stride;
    rgba8_t color = read_rgb565(row + (size_t)x * 2U);
    color.r = mul8(color.r, surface->tint.r);
    color.g = mul8(color.g, surface->tint.g);
    color.b = mul8(color.b, surface->tint.b);
    color.a = surface->tint.a;
    return color;
}

static inline rgba8_t sample_raw_rgb888(
    const grape_surface_t *surface,
    int32_t x,
    int32_t y)
{
    const uint8_t *row = surface->texture->pixels + (size_t)y * surface->texture->stride;
    rgba8_t color = read_rgb888(row + (size_t)x * 3U);
    color.r = mul8(color.r, surface->tint.r);
    color.g = mul8(color.g, surface->tint.g);
    color.b = mul8(color.b, surface->tint.b);
    color.a = surface->tint.a;
    return color;
}

static inline rgba8_t sample_raw_rgba8888(
    const grape_surface_t *surface,
    int32_t x,
    int32_t y)
{
    const uint8_t *row = surface->texture->pixels + (size_t)y * surface->texture->stride;
    rgba8_t color = read_rgba8888(row + (size_t)x * 4U);
    color.r = mul8(color.r, surface->tint.r);
    color.g = mul8(color.g, surface->tint.g);
    color.b = mul8(color.b, surface->tint.b);
    color.a = mul8(color.a, surface->tint.a);
    return color;
}

#define DEFINE_NEAREST_SAMPLER(name, raw_sampler) \
    static inline bool name( \
        const grape_surface_t *surface, float local_x, float local_y, rgba8_t *out) \
    { \
        int32_t tx; \
        int32_t ty; \
        if (surface->texture_mapping_identity) { \
            if (local_x < 0.0f || local_y < 0.0f || \
                local_x >= (float)surface->texture->width || \
                local_y >= (float)surface->texture->height) { \
                return false; \
            } \
            tx = (int32_t)local_x; \
            ty = (int32_t)local_y; \
        } else { \
            if (!grape_surface_map_texture_point(surface, local_x, local_y, &tx, &ty)) { \
                return false; \
            } \
        } \
        *out = raw_sampler(surface, tx, ty); \
        return true; \
    }

DEFINE_NEAREST_SAMPLER(sample_nearest_a8, sample_raw_a8)
DEFINE_NEAREST_SAMPLER(sample_nearest_rgb565, sample_raw_rgb565)
DEFINE_NEAREST_SAMPLER(sample_nearest_rgb888, sample_raw_rgb888)
DEFINE_NEAREST_SAMPLER(sample_nearest_rgba8888, sample_raw_rgba8888)
#undef DEFINE_NEAREST_SAMPLER

#define DEFINE_LINEAR_SAMPLER(name, raw_sampler) \
    static inline bool name( \
        const grape_surface_t *surface, float local_x, float local_y, rgba8_t *out) \
    { \
        float texture_x; \
        float texture_y; \
        surface_map_texture_float(surface, local_x, local_y, &texture_x, &texture_y); \
        bilinear_sample_t sample; \
        build_bilinear_sample(surface, texture_x, texture_y, &sample); \
        rgba8_t c00 = (sample.valid_mask & BILINEAR_VALID_00) \
            ? raw_sampler(surface, sample.x0, sample.y0) : (rgba8_t){0}; \
        rgba8_t c10 = (sample.valid_mask & BILINEAR_VALID_10) \
            ? raw_sampler(surface, sample.x1, sample.y0) : (rgba8_t){0}; \
        rgba8_t c01 = (sample.valid_mask & BILINEAR_VALID_01) \
            ? raw_sampler(surface, sample.x0, sample.y1) : (rgba8_t){0}; \
        rgba8_t c11 = (sample.valid_mask & BILINEAR_VALID_11) \
            ? raw_sampler(surface, sample.x1, sample.y1) : (rgba8_t){0}; \
        *out = bilinear_mix_rgba(c00, c10, c01, c11, &sample); \
        return out->a != 0U; \
    }

DEFINE_LINEAR_SAMPLER(sample_linear_a8, sample_raw_a8)
DEFINE_LINEAR_SAMPLER(sample_linear_rgb565, sample_raw_rgb565)
DEFINE_LINEAR_SAMPLER(sample_linear_rgb888, sample_raw_rgb888)
DEFINE_LINEAR_SAMPLER(sample_linear_rgba8888, sample_raw_rgba8888)
#undef DEFINE_LINEAR_SAMPLER

typedef bool (*surface_sample_fn_t)(const grape_surface_t *surface,
                                    float local_x,
                                    float local_y,
                                    rgba8_t *out);

static surface_sample_fn_t surface_select_sampler(const grape_surface_t *surface)
{
    if (!surface || !surface->texture) {
        return NULL;
    }

    const bool linear = surface->texture_filter == GRAPE_TEXTURE_FILTER_LINEAR;
    switch (surface->texture->format) {
        case GRAPE_PIXEL_FORMAT_A8:
            return linear ? sample_linear_a8 : sample_nearest_a8;
        case GRAPE_PIXEL_FORMAT_RGB565:
            return linear ? sample_linear_rgb565 : sample_nearest_rgb565;
        case GRAPE_PIXEL_FORMAT_RGB888:
            return linear ? sample_linear_rgb888 : sample_nearest_rgb888;
        case GRAPE_PIXEL_FORMAT_RGBA8888:
            return linear ? sample_linear_rgba8888 : sample_nearest_rgba8888;
        default:
            return NULL;
    }
}

#define DEFINE_TEXTURED_RASTER(name, sampler, use_aa) \
    static void name(grape_context_t *context, const grape_surface_t *surface, \
                     grape_rect_t clipped, size_t bpp) \
    { \
        const int32_t raster_width = clipped.width; \
        const float local_x_step = surface->local_x_from_screen_x; \
        const float local_y_step = surface->local_y_from_screen_x; \
        const float surface_width = (float)surface->width; \
        const float surface_height = (float)surface->height; \
        const grape_pixel_format_t output_format = context->display_info.format; \
        const uint8_t opacity = surface->opacity; \
        for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) { \
            float local_x; \
            float local_y; \
            affine_row_start(surface, clipped.x, y, &local_x, &local_y); \
            uint8_t *dst = target_pixel_address(context, clipped.x, y, bpp); \
            for (int32_t x = 0; x < raster_width; ++x) { \
                float shade_x = local_x; \
                float shade_y = local_y; \
                uint8_t coverage = 4U; \
                bool inside = true; \
                if (use_aa) { \
                    coverage = surface_coverage_4x(surface, local_x, local_y, &shade_x, &shade_y); \
                    inside = coverage != 0U; \
                } else { \
                    inside = local_x >= 0.0f && local_y >= 0.0f && \
                             local_x < surface_width && local_y < surface_height; \
                } \
                if (inside) { \
                    rgba8_t source; \
                    if (sampler(surface, shade_x, shade_y, &source)) { \
                        source.a = mul8(source.a, opacity); \
                        if (use_aa && coverage != 4U) { \
                            source.a = coverage_mul4(source.a, coverage); \
                        } \
                        composite_source_pixel(dst, output_format, source); \
                    } \
                } \
                local_x += local_x_step; \
                local_y += local_y_step; \
                dst += bpp; \
            } \
        } \
    }

DEFINE_TEXTURED_RASTER(raster_quality_a8_nearest_noaa, sample_nearest_a8, 0)
DEFINE_TEXTURED_RASTER(raster_quality_a8_nearest_aa, sample_nearest_a8, 1)
DEFINE_TEXTURED_RASTER(raster_quality_a8_linear_noaa, sample_linear_a8, 0)
DEFINE_TEXTURED_RASTER(raster_quality_a8_linear_aa, sample_linear_a8, 1)
DEFINE_TEXTURED_RASTER(raster_quality_rgb565_nearest_noaa, sample_nearest_rgb565, 0)
DEFINE_TEXTURED_RASTER(raster_quality_rgb565_nearest_aa, sample_nearest_rgb565, 1)
DEFINE_TEXTURED_RASTER(raster_quality_rgb565_linear_noaa, sample_linear_rgb565, 0)
DEFINE_TEXTURED_RASTER(raster_quality_rgb565_linear_aa, sample_linear_rgb565, 1)
DEFINE_TEXTURED_RASTER(raster_quality_rgb888_nearest_noaa, sample_nearest_rgb888, 0)
DEFINE_TEXTURED_RASTER(raster_quality_rgb888_nearest_aa, sample_nearest_rgb888, 1)
DEFINE_TEXTURED_RASTER(raster_quality_rgb888_linear_noaa, sample_linear_rgb888, 0)
DEFINE_TEXTURED_RASTER(raster_quality_rgb888_linear_aa, sample_linear_rgb888, 1)
DEFINE_TEXTURED_RASTER(raster_quality_rgba8888_nearest_noaa, sample_nearest_rgba8888, 0)
DEFINE_TEXTURED_RASTER(raster_quality_rgba8888_nearest_aa, sample_nearest_rgba8888, 1)
DEFINE_TEXTURED_RASTER(raster_quality_rgba8888_linear_noaa, sample_linear_rgba8888, 0)
DEFINE_TEXTURED_RASTER(raster_quality_rgba8888_linear_aa, sample_linear_rgba8888, 1)
#undef DEFINE_TEXTURED_RASTER

static void raster_surface_quality_dispatch(grape_context_t *context,
                                            const grape_surface_t *surface,
                                            grape_rect_t clipped,
                                            size_t bpp)
{
    const bool linear = surface->texture_filter == GRAPE_TEXTURE_FILTER_LINEAR;
    const bool aa = surface->aa == GRAPE_SURFACE_AA_COVERAGE_4X;

#define DISPATCH_QUALITY(format_name) \
    do { \
        if (linear) { \
            if (aa) raster_quality_##format_name##_linear_aa(context, surface, clipped, bpp); \
            else raster_quality_##format_name##_linear_noaa(context, surface, clipped, bpp); \
        } else { \
            if (aa) raster_quality_##format_name##_nearest_aa(context, surface, clipped, bpp); \
            else raster_quality_##format_name##_nearest_noaa(context, surface, clipped, bpp); \
        } \
    } while (0)

    switch (surface->texture->format) {
        case GRAPE_PIXEL_FORMAT_A8:
            DISPATCH_QUALITY(a8);
            break;
        case GRAPE_PIXEL_FORMAT_RGB565:
            DISPATCH_QUALITY(rgb565);
            break;
        case GRAPE_PIXEL_FORMAT_RGB888:
            DISPATCH_QUALITY(rgb888);
            break;
        case GRAPE_PIXEL_FORMAT_RGBA8888:
            DISPATCH_QUALITY(rgba8888);
            break;
        default:
            break;
    }
#undef DISPATCH_QUALITY
}

static inline void evaluate_shader_chain_pixel(
    const grape_surface_t *surface,
    rgba8_t source,
    float local_x,
    float local_y,
    float inv_width,
    float inv_height,
    uint8_t coverage,
    uint8_t *dst,
    grape_pixel_format_t output_format)
{
    const float surface_width = (float)surface->width;
    const float surface_height = (float)surface->height;
    grape_shader_eval_args_t args = {
        .source_color = {
            (float)source.r / 255.0f,
            (float)source.g / 255.0f,
            (float)source.b / 255.0f,
            (float)source.a / 255.0f,
        },
        .uv = {local_x * inv_width, local_y * inv_height},
        .local_position = {local_x, local_y},
        .surface_size = {surface_width, surface_height},
        .frag_coord = {local_x, surface_height - local_y},
    };

    grape_shader_vec4_t output = args.source_color;
    for (size_t shader_index = 0U; shader_index < surface->shader_count; ++shader_index) {
        const grape_surface_shader_instance_t *shader = &surface->shaders[shader_index];
        args.source_color = output;
        output = shader->program->eval(&args, shader->uniforms);
    }

    rgba8_t final_color = {
        .r = grape_shader_float_to_u8(output.x),
        .g = grape_shader_float_to_u8(output.y),
        .b = grape_shader_float_to_u8(output.z),
        .a = mul8(grape_shader_float_to_u8(output.w), surface->opacity),
    };
    if (coverage != 4U) {
        final_color.a = coverage_mul4(final_color.a, coverage);
    }
    composite_source_pixel(dst, output_format, final_color);
}

static void raster_surface_shader_chain(grape_context_t *context,
                                        const grape_surface_t *surface,
                                        grape_rect_t clipped,
                                        size_t bpp)
{
    const float surface_width = (float)surface->width;
    const float surface_height = (float)surface->height;
    const float inv_width = 1.0f / surface_width;
    const float inv_height = 1.0f / surface_height;
    const float local_x_step = surface->local_x_from_screen_x;
    const float local_y_step = surface->local_y_from_screen_x;
    const bool aa = surface->aa == GRAPE_SURFACE_AA_COVERAGE_4X;
    surface_sample_fn_t sampler = surface_select_sampler(surface);

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        float local_x;
        float local_y;
        affine_row_start(surface, clipped.x, y, &local_x, &local_y);
        uint8_t *dst = target_pixel_address(context, clipped.x, y, bpp);

        for (int32_t x = 0; x < clipped.width; ++x) {
            float shade_x = local_x;
            float shade_y = local_y;
            uint8_t coverage = 4U;
            bool inside;
            if (aa) {
                coverage = surface_coverage_4x(surface, local_x, local_y, &shade_x, &shade_y);
                inside = coverage != 0U;
            } else {
                inside = local_x >= 0.0f && local_y >= 0.0f &&
                         local_x < surface_width && local_y < surface_height;
            }

            if (inside) {
                rgba8_t source = {0};
                if (surface->texture && sampler) {
                    /*
                     * Shader chains run for the whole surface rectangle even
                     * when the texture contributes transparent black. A later
                     * stage is allowed to turn transparent input into visible
                     * output, so texture alpha must never suppress evaluation.
                     */
                    (void)sampler(surface, shade_x, shade_y, &source);
                }
                evaluate_shader_chain_pixel(surface, source, shade_x, shade_y,
                                            inv_width, inv_height, coverage,
                                            dst, context->display_info.format);
            }

            local_x += local_x_step;
            local_y += local_y_step;
            dst += bpp;
        }
    }
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
/* Exact 1x/2x opaque RGB565 copies. The bounded half-integer offsets and
 * binary-exact increments preserve the generic affine loop's pixel choices.
 * Other states retain the existing rasterizer, including fractional placement. */
static bool raster_rgb565_copy(grape_context_t *context, const grape_surface_t *surface,
                                grape_rect_t clipped, size_t bpp)
{
    const grape_texture_t *texture = surface->texture;
    const float sx = surface->local_x_from_screen_x;
    const float sy = surface->local_y_from_screen_y;
    const float ox = surface->local_x_offset, oy = surface->local_y_offset;
    if (context->display_info.format != GRAPE_PIXEL_FORMAT_RGB565 || bpp != 2U ||
        texture->format != GRAPE_PIXEL_FORMAT_RGB565 ||
        !surface->texture_mapping_identity || surface->shader_count != 0U ||
        surface->texture_filter != GRAPE_TEXTURE_FILTER_NEAREST || surface->aa != GRAPE_SURFACE_AA_NONE ||
        surface->opacity != 255U || surface->tint.r != 255U || surface->tint.g != 255U ||
        surface->tint.b != 255U || surface->tint.a != 255U ||
        surface->local_x_from_screen_y != 0.0f || surface->local_y_from_screen_x != 0.0f ||
        !(sx == 1.0f || sx == 0.5f) || !(sy == 1.0f || sy == 0.5f) ||
        !isfinite(ox) || !isfinite(oy) || fabsf(ox) > 8192.0f || fabsf(oy) > 8192.0f ||
        texture->width > 8192U || texture->height > 8192U ||
        clipped.x < 0 || clipped.y < 0 || clipped.width <= 0 || clipped.height <= 0 ||
        clipped.x > 8192 || clipped.y > 8192 ||
        clipped.width > 8192 - clipped.x || clipped.height > 8192 - clipped.y) return false;
    if (ox * 2.0f != (float)(int32_t)(ox * 2.0f) ||
        oy * 2.0f != (float)(int32_t)(oy * 2.0f)) return false;
    float first_x, first_y, last_x, last_y;
    affine_row_start(surface, clipped.x, clipped.y, &first_x, &first_y);
    affine_row_start(surface, clipped.x + clipped.width - 1, clipped.y + clipped.height - 1,
                     &last_x, &last_y);
    if (first_x < 0.0f || first_y < 0.0f ||
        last_x >= (float)texture->width || last_y >= (float)texture->height) return false;
    const uint32_t tx = (uint32_t)first_x;
    const uint32_t phase = (uint32_t)(first_x * 2.0f) & 1U;
    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        float lx, ly;
        affine_row_start(surface, clipped.x, y, &lx, &ly);
        const uint8_t *src = texture->pixels + (size_t)(uint32_t)ly * texture->stride + (size_t)tx * 2U;
        uint8_t *dst = target_pixel_address(context, clipped.x, y, 2U);
        if (sx == 1.0f) {
            memcpy(dst, src, (size_t)clipped.width * 2U);
        } else {
            uint32_t remaining = (uint32_t)clipped.width;
            uint32_t run = 2U - phase;
            while (remaining) {
                if (run > remaining) run = remaining;
                const uint8_t lo = src[0], hi = src[1];
                for (uint32_t i = 0; i < run; ++i) { dst[0] = lo; dst[1] = hi; dst += 2U; }
                remaining -= run; src += 2U; run = 2U;
            }
        }
    }
    return true;
}

static void raster_surface_rgb565(grape_context_t *context, const grape_surface_t *surface,
                                  grape_rect_t damage_rect, grape_rect_t clipped, size_t bpp)
{
    if (raster_rgb565_copy(context, surface, clipped, bpp)) return;
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


typedef struct {
    int32_t left;
    int32_t top;
    uint32_t scale_x;
    uint32_t scale_y;
} rgba8888_axis_scale_t;

/*
 * Detect the deliberately narrow but very common "GPU render target as a
 * surface" case. Keeping this predicate strict means the generic compositor
 * remains the source of truth for rotation, fractional scale, tinting,
 * non-identity texture layouts, filtering, AA, and shader chains.
 *
 * For an integer positive scale and integer transformed top-left, point
 * sampling of destination pixel centres maps exactly to:
 *
 *   src_x = (dst_x - left) / scale_x
 *   src_y = (dst_y - top)  / scale_y
 *
 * so the hot loop needs no inverse-affine floats at all.
 */
static bool rgba8888_axis_integer_scale(
    const grape_surface_t *surface,
    rgba8888_axis_scale_t *out)
{
    if (!surface || !surface->texture || !out ||
        surface->texture->format != GRAPE_PIXEL_FORMAT_RGBA8888 ||
        !surface->texture_mapping_identity ||
        surface->texture_filter != GRAPE_TEXTURE_FILTER_NEAREST ||
        surface->aa != GRAPE_SURFACE_AA_NONE ||
        surface->shader_count != 0U ||
        surface->opacity != 255U ||
        surface->tint.r != 255U ||
        surface->tint.g != 255U ||
        surface->tint.b != 255U ||
        surface->tint.a != 255U ||
        fabsf(surface->sin_rotation) > 0.0001f ||
        fabsf(surface->cos_rotation - 1.0f) > 0.0001f ||
        !isfinite(surface->transform.scale_x) ||
        !isfinite(surface->transform.scale_y) ||
        surface->transform.scale_x < 1.0f ||
        surface->transform.scale_y < 1.0f) {
        return false;
    }

    const float scale_x_rounded = roundf(surface->transform.scale_x);
    const float scale_y_rounded = roundf(surface->transform.scale_y);
    if (fabsf(surface->transform.scale_x - scale_x_rounded) > 0.0001f ||
        fabsf(surface->transform.scale_y - scale_y_rounded) > 0.0001f ||
        scale_x_rounded > 65535.0f ||
        scale_y_rounded > 65535.0f) {
        return false;
    }

    const float left_f =
        surface->transform.x -
        surface->transform.origin_x * surface->transform.scale_x;
    const float top_f =
        surface->transform.y -
        surface->transform.origin_y * surface->transform.scale_y;
    if (!isfinite(left_f) || !isfinite(top_f)) {
        return false;
    }

    const float left_rounded = roundf(left_f);
    const float top_rounded = roundf(top_f);
    if (fabsf(left_f - left_rounded) > 0.0001f ||
        fabsf(top_f - top_rounded) > 0.0001f ||
        left_rounded < (float)INT32_MIN ||
        left_rounded > (float)INT32_MAX ||
        top_rounded < (float)INT32_MIN ||
        top_rounded > (float)INT32_MAX) {
        return false;
    }

    *out = (rgba8888_axis_scale_t) {
        .left = (int32_t)left_rounded,
        .top = (int32_t)top_rounded,
        .scale_x = (uint32_t)scale_x_rounded,
        .scale_y = (uint32_t)scale_y_rounded,
    };
    return true;
}

static __attribute__((always_inline)) inline void
rgba8888_fast_blend_rgb565(uint8_t *dst, const uint8_t *src)
{
    const uint8_t alpha = src[3];
    if (alpha == 0U) {
        return;
    }

    rgba8_t source = {
        .r = src[0],
        .g = src[1],
        .b = src[2],
        .a = alpha,
    };

    if (alpha == 255U) {
        write_rgb565(dst, source);
        return;
    }

    write_rgb565(dst, blend_over(read_rgb565(dst), source));
}

static __attribute__((always_inline)) inline void
rgba8888_fast_blend_rgb888(uint8_t *dst, const uint8_t *src)
{
    const uint8_t alpha = src[3];
    if (alpha == 0U) {
        return;
    }

    if (alpha == 255U) {
        dst[0] = src[0];
        dst[1] = src[1];
        dst[2] = src[2];
        return;
    }

    rgba8_t source = {
        .r = src[0],
        .g = src[1],
        .b = src[2],
        .a = alpha,
    };
    write_rgb888(dst, blend_over(read_rgb888(dst), source));
}

/*
 * Fast path for axis-aligned integer-nearest RGBA8888 surfaces.
 *
 * The run-based X walker is important for scaled GPU targets: at 2x scale we
 * decode/sample one source texel for two destination pixels instead of doing
 * the generic inverse transform and source-address calculation twice.
 */
static void raster_surface_rgba8888_axis_integer(
    grape_context_t *context,
    const grape_surface_t *surface,
    grape_rect_t clipped,
    size_t bpp,
    const rgba8888_axis_scale_t *mapping)
{
    const grape_texture_t *texture = surface->texture;
    const uint8_t *texture_pixels = texture->pixels;
    const size_t texture_stride = texture->stride;
    const uint32_t scale_x = mapping->scale_x;
    const uint32_t scale_y = mapping->scale_y;
    const grape_pixel_format_t output_format = context->display_info.format;

    const int32_t first_local_x = clipped.x - mapping->left;
    const uint32_t first_source_x = (uint32_t)first_local_x / scale_x;
    const uint32_t first_phase_x = (uint32_t)first_local_x % scale_x;

    for (int32_t y = clipped.y; y < clipped.y + clipped.height; ++y) {
        const int32_t local_y = y - mapping->top;
        const uint32_t source_y = (uint32_t)local_y / scale_y;
        const uint8_t *src =
            texture_pixels +
            (size_t)source_y * texture_stride +
            (size_t)first_source_x * 4U;
        uint8_t *dst = target_pixel_address(context, clipped.x, y, bpp);

        int32_t remaining = clipped.width;
        uint32_t phase = first_phase_x;

        while (remaining > 0) {
            uint32_t run = scale_x - phase;
            if (run > (uint32_t)remaining) {
                run = (uint32_t)remaining;
            }

            const uint8_t alpha = src[3];
            if (alpha != 0U) {
                if (output_format == GRAPE_PIXEL_FORMAT_RGB565) {
                    if (alpha == 255U) {
                        const uint16_t packed =
                            (uint16_t)(((uint16_t)(src[0] >> 3) << 11) |
                                       ((uint16_t)(src[1] >> 2) << 5) |
                                       (uint16_t)(src[2] >> 3));
                        const uint8_t packed_lo = (uint8_t)(packed & 0xFFU);
                        const uint8_t packed_hi = (uint8_t)(packed >> 8);
                        for (uint32_t i = 0U; i < run; ++i) {
                            dst[0] = packed_lo;
                            dst[1] = packed_hi;
                            dst += 2U;
                        }
                    } else {
                        for (uint32_t i = 0U; i < run; ++i) {
                            rgba8888_fast_blend_rgb565(dst, src);
                            dst += 2U;
                        }
                    }
                } else {
                    if (alpha == 255U) {
                        for (uint32_t i = 0U; i < run; ++i) {
                            dst[0] = src[0];
                            dst[1] = src[1];
                            dst[2] = src[2];
                            dst += 3U;
                        }
                    } else {
                        for (uint32_t i = 0U; i < run; ++i) {
                            rgba8888_fast_blend_rgb888(dst, src);
                            dst += 3U;
                        }
                    }
                }
            } else {
                dst += (size_t)run * bpp;
            }

            remaining -= (int32_t)run;
            src += 4U;
            phase = 0U;
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
    if (surface->texture->format == GRAPE_PIXEL_FORMAT_RGBA8888) {
        rgba8888_axis_scale_t mapping;
        if (rgba8888_axis_integer_scale(surface, &mapping)) {
            GRAPE_TIME_BLOCK(RGBA8888_AXIS_FAST) {
                raster_surface_rgba8888_axis_integer(
                    context,
                    surface,
                    clipped,
                    bpp,
                    &mapping
                );
            }
            return;
        }
    }

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
            (!surface->texture && surface->shader_count == 0U)) {
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

        if (surface->shader_count > 0U) {
            GRAPE_TIME_BLOCK(CPU_SURFACE_RASTER) {
                raster_surface_shader_chain(context, surface, clipped, bpp);
            }
            continue;
        }

        bool handled = false;
        const bool legacy_fast_path =
            surface->texture_mapping_identity &&
            surface->texture_filter == GRAPE_TEXTURE_FILTER_NEAREST &&
            surface->aa == GRAPE_SURFACE_AA_NONE;

        // Attempt to raster the surface using three shear method if backend is set to auto, fallback to CPU
        if (legacy_fast_path &&
            context->rotation_backend == GRAPE_ROTATION_BACKEND_AUTO) {
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
            if (legacy_fast_path &&
                context->rotation_backend == GRAPE_ROTATION_BACKEND_THREE_SHEAR) {
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
                if (legacy_fast_path) {
                    raster_surface_cpu(context, surface, rect, clipped, bpp);
                } else {
                    raster_surface_quality_dispatch(context, surface, clipped, bpp);
                }
            }
        }
    }

    // Render debug overlays
    grape_debug_render(context, rect);
    return ESP_OK;
}
