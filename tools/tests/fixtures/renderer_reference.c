/* Frozen reference functions from the pre-patch current tree; host tests only. */
static inline uint8_t gpu_mul8(uint8_t a, uint8_t b)
{
    return (uint8_t)(((uint16_t)a * b + 127U) / 255U);
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

static inline size_t occupancy_index(const grape_texture_t *texture,
                                     uint32_t x,
                                     uint32_t y)
{
    return (size_t)y * texture->occupancy_columns + x;
}

static inline void occupancy_set(uint8_t *bitmap, size_t index)
{
    bitmap[index >> 3U] |= (uint8_t)(1U << (index & 7U));
}

static inline bool texture_format_has_alpha(grape_pixel_format_t format)
{
    return format == GRAPE_PIXEL_FORMAT_A8 ||
           format == GRAPE_PIXEL_FORMAT_RGBA8888;
}

static inline uint8_t texture_alpha_at(const grape_texture_t *texture,
                                       const uint8_t *row,
                                       uint32_t x)
{
    if (texture->format == GRAPE_PIXEL_FORMAT_A8) {
        return row[x];
    }

    return row[(size_t)x * 4U + 3U];
}

esp_err_t grape_texture_rebuild_occupancy(grape_texture_t *texture)
{
    if (!texture) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!texture_format_has_alpha(texture->format)) {
        texture->occupancy_occupied_count =
            (size_t)texture->occupancy_columns * texture->occupancy_rows;
        texture->occupancy_all_full = true;
        texture->occupancy_all_empty = false;
        return ESP_OK;
    }

    if (!texture->occupancy || texture->occupancy_bitmap_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(texture->occupancy, 0, texture->occupancy_bitmap_size);

    const uint32_t cell_size = CONFIG_GRAPE_TEXTURE_OCCUPANCY_CELL_SIZE;
    size_t occupied_count = 0;
    size_t cell_count = (size_t)texture->occupancy_columns * texture->occupancy_rows;

    for (uint32_t cell_y = 0; cell_y < texture->occupancy_rows; ++cell_y) {
        uint32_t y0 = cell_y * cell_size;
        uint32_t y1 = y0 + cell_size;
        if (y1 > texture->height) {
            y1 = texture->height;
        }

        for (uint32_t cell_x = 0; cell_x < texture->occupancy_columns; ++cell_x) {
            uint32_t x0 = cell_x * cell_size;
            uint32_t x1 = x0 + cell_size;
            if (x1 > texture->width) {
                x1 = texture->width;
            }

            bool occupied = false;
            for (uint32_t y = y0; y < y1 && !occupied; ++y) {
                const uint8_t *row = texture->pixels + (size_t)y * texture->stride;
                for (uint32_t x = x0; x < x1; ++x) {
                    if (texture_alpha_at(texture, row, x) != 0U) {
                        occupied = true;
                        break;
                    }
                }
            }

            if (occupied) {
                occupancy_set(
                    texture->occupancy,
                    occupancy_index(texture, cell_x, cell_y)
                );
                occupied_count++;
            }
        }
    }

    texture->occupancy_occupied_count = occupied_count;
    texture->occupancy_all_empty = occupied_count == 0;
    texture->occupancy_all_full = occupied_count == cell_count;
    return ESP_OK;
}

static inline uint8_t *target_pixel_address(grape_context_t *context,
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

static inline void write_rgb565(uint8_t *dst, rgba8_t color)
{
    uint16_t pixel = (uint16_t)(((color.r >> 3) << 11) | ((color.g >> 2) << 5) | (color.b >> 3));
    dst[0] = (uint8_t)(pixel & 0xFF);
    dst[1] = (uint8_t)(pixel >> 8);
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

static inline void write_rgb888(uint8_t *dst, rgba8_t color)
{
    dst[0] = color.r;
    dst[1] = color.g;
    dst[2] = color.b;
}

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

