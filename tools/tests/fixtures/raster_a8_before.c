static void raster_surface_a8_before(grape_context_t *context, const grape_surface_t *surface,
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
