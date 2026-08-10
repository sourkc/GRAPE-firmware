#include <math.h>

#include "grape_glyph_cache_internal.h"
#include "grape/grape_text.h"

static void bounds_include(grape_path_bounds_t *bounds,
                           bool *has_bounds,
                           float min_x,
                           float min_y,
                           float max_x,
                           float max_y)
{
    if (!*has_bounds) {
        *bounds = (grape_path_bounds_t){
            .min_x = min_x,
            .min_y = min_y,
            .max_x = max_x,
            .max_y = max_y,
        };
        *has_bounds = true;
        return;
    }

    if (min_x < bounds->min_x) bounds->min_x = min_x;
    if (min_y < bounds->min_y) bounds->min_y = min_y;
    if (max_x > bounds->max_x) bounds->max_x = max_x;
    if (max_y > bounds->max_y) bounds->max_y = max_y;
}

esp_err_t grape_text_measure_codepoints(const grape_font_t *font,
                                        const uint32_t *codepoints,
                                        size_t codepoint_count,
                                        grape_text_measurement_t *out_measurement)
{
    if (!font || (!codepoints && codepoint_count != 0U) || !out_measurement) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_measurement = (grape_text_measurement_t){0};
    float pen_x = 0.0f;

    for (size_t i = 0; i < codepoint_count; ++i) {
        uint16_t glyph_id = 0;
        esp_err_t ret = grape_font_get_glyph_id(font, codepoints[i], &glyph_id);
        if (ret != ESP_OK) {
            return ret;
        }

        grape_font_glyph_info_t info = {0};
        ret = grape_font_get_glyph_info(font, glyph_id, &info);
        if (ret != ESP_OK) {
            return ret;
        }

        if (info.kind != GRAPE_FONT_GLYPH_EMPTY && info.x_max > info.x_min &&
            info.y_max > info.y_min) {
            bounds_include(
                &out_measurement->bounds,
                &out_measurement->has_bounds,
                pen_x + (float)info.x_min,
                -(float)info.y_max,
                pen_x + (float)info.x_max,
                -(float)info.y_min
            );
        }

        grape_font_glyph_metrics_t metrics = {0};
        ret = grape_font_get_glyph_metrics(font, glyph_id, &metrics);
        if (ret != ESP_OK) {
            return ret;
        }

        pen_x += (float)metrics.advance_width;
        if (!isfinite(pen_x)) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    out_measurement->glyph_count = codepoint_count;
    out_measurement->advance_width = pen_x;
    return ESP_OK;
}

esp_err_t grape_text_build_codepoints_path(const grape_font_t *font,
                                            const uint32_t *codepoints,
                                            size_t codepoint_count,
                                            grape_path_t *path,
                                            grape_text_path_info_t *out_info)
{
    if (!font || (!codepoints && codepoint_count != 0U) || !path) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_info) {
        *out_info = (grape_text_path_info_t){0};
    }

    esp_err_t ret = grape_path_clear(path);
    if (ret != ESP_OK) {
        return ret;
    }

    float pen_x = 0.0f;
    for (size_t i = 0; i < codepoint_count; ++i) {
        uint16_t glyph_id = 0;
        ret = grape_font_get_glyph_id(font, codepoints[i], &glyph_id);
        if (ret != ESP_OK) {
            grape_path_clear(path);
            return ret;
        }

        grape_font_glyph_info_t glyph_info = {0};
        ret = grape_font_get_glyph_info(font, glyph_id, &glyph_info);
        if (ret != ESP_OK) {
            grape_path_clear(path);
            return ret;
        }
        if (glyph_info.kind == GRAPE_FONT_GLYPH_COMPOSITE) {
            grape_path_clear(path);
            return ESP_ERR_NOT_SUPPORTED;
        }

        if (glyph_info.kind == GRAPE_FONT_GLYPH_SIMPLE && glyph_info.contour_count > 0) {
            ret = grape_font_append_glyph_path(font, glyph_id, path, pen_x, 0.0f);
            if (ret != ESP_OK) {
                grape_path_clear(path);
                return ret;
            }
        }

        grape_font_glyph_metrics_t metrics = {0};
        ret = grape_font_get_glyph_metrics(font, glyph_id, &metrics);
        if (ret != ESP_OK) {
            grape_path_clear(path);
            return ret;
        }

        pen_x += (float)metrics.advance_width;
        if (!isfinite(pen_x)) {
            grape_path_clear(path);
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (out_info) {
        out_info->glyph_count = codepoint_count;
        out_info->advance_width = pen_x;
    }
    return ESP_OK;
}

static esp_err_t text_raster_dimensions(const grape_text_measurement_t *measurement,
                                        const grape_path_rasterize_config_t *config,
                                        uint32_t *out_width,
                                        uint32_t *out_height,
                                        float *out_origin_x,
                                        float *out_origin_y)
{
    if (!measurement->has_bounds) {
        *out_width = 1U;
        *out_height = 1U;
        *out_origin_x = 0.0f;
        *out_origin_y = 0.0f;
        return ESP_OK;
    }

    double scale = (double)config->pixels_per_unit;
    double min_x_px = floor((double)measurement->bounds.min_x * scale) -
                      config->padding_pixels;
    double min_y_px = floor((double)measurement->bounds.min_y * scale) -
                      config->padding_pixels;
    double max_x_px = ceil((double)measurement->bounds.max_x * scale) +
                      config->padding_pixels;
    double max_y_px = ceil((double)measurement->bounds.max_y * scale) +
                      config->padding_pixels;
    double width = max_x_px - min_x_px;
    double height = max_y_px - min_y_px;

    if (!isfinite(min_x_px) || !isfinite(min_y_px) ||
        !isfinite(max_x_px) || !isfinite(max_y_px) ||
        width < 1.0 || height < 1.0 ||
        width > UINT32_MAX || height > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    *out_origin_x = (float)(min_x_px / scale);
    *out_origin_y = (float)(min_y_px / scale);
    return ESP_OK;
}

static inline void blend_coverage(uint8_t *dst, uint8_t src)
{
    if (src == 0U) {
        return;
    }
    if (src == 255U || *dst == 0U) {
        *dst = src;
        return;
    }

    *dst = (uint8_t)(src +
        ((uint32_t)(*dst) * (255U - src) + 127U) / 255U);
}

static uint8_t sample_a8_bilinear(const grape_texture_t *texture,
                                  float x,
                                  float y)
{
    int32_t x0 = (int32_t)floorf(x);
    int32_t y0 = (int32_t)floorf(y);
    float fx = x - (float)x0;
    float fy = y - (float)y0;

    const uint8_t *pixels = grape_texture_pixels_const(texture);
    size_t stride = grape_texture_stride(texture);
    int32_t width = (int32_t)grape_texture_width(texture);
    int32_t height = (int32_t)grape_texture_height(texture);

    float samples[4] = {0};
    const int32_t xs[2] = {x0, x0 + 1};
    const int32_t ys[2] = {y0, y0 + 1};
    for (uint32_t iy = 0; iy < 2U; ++iy) {
        if (ys[iy] < 0 || ys[iy] >= height) {
            continue;
        }
        const uint8_t *row = pixels + (size_t)ys[iy] * stride;
        for (uint32_t ix = 0; ix < 2U; ++ix) {
            if (xs[ix] >= 0 && xs[ix] < width) {
                samples[iy * 2U + ix] = row[xs[ix]];
            }
        }
    }

    float top = samples[0] + (samples[1] - samples[0]) * fx;
    float bottom = samples[2] + (samples[3] - samples[2]) * fx;
    float value = top + (bottom - top) * fy;
    if (value <= 0.0f) return 0U;
    if (value >= 255.0f) return 255U;
    return (uint8_t)(value + 0.5f);
}

static void blit_a8_integer(grape_texture_t *destination,
                            const grape_texture_t *source,
                            int32_t left,
                            int32_t top)
{
    uint8_t *dst = grape_texture_pixels(destination);
    const uint8_t *src = grape_texture_pixels_const(source);
    size_t dst_stride = grape_texture_stride(destination);
    size_t src_stride = grape_texture_stride(source);
    int32_t dst_width = (int32_t)grape_texture_width(destination);
    int32_t dst_height = (int32_t)grape_texture_height(destination);
    int32_t src_width = (int32_t)grape_texture_width(source);
    int32_t src_height = (int32_t)grape_texture_height(source);

    int32_t source_x0 = left < 0 ? -left : 0;
    int32_t source_y0 = top < 0 ? -top : 0;
    int32_t source_x1 = src_width;
    int32_t source_y1 = src_height;
    if (left + source_x1 > dst_width) source_x1 = dst_width - left;
    if (top + source_y1 > dst_height) source_y1 = dst_height - top;

    if (source_x0 >= source_x1 || source_y0 >= source_y1) {
        return;
    }

    for (int32_t y = source_y0; y < source_y1; ++y) {
        uint8_t *dst_row = dst + (size_t)(top + y) * dst_stride;
        const uint8_t *src_row = src + (size_t)y * src_stride;
        for (int32_t x = source_x0; x < source_x1; ++x) {
            blend_coverage(&dst_row[left + x], src_row[x]);
        }
    }
}

static void blit_a8_translated(grape_texture_t *destination,
                               const grape_texture_t *source,
                               float left,
                               float top)
{
    float rounded_left = roundf(left);
    float rounded_top = roundf(top);
    if (fabsf(left - rounded_left) < 0.0001f &&
        fabsf(top - rounded_top) < 0.0001f) {
        blit_a8_integer(
            destination,
            source,
            (int32_t)rounded_left,
            (int32_t)rounded_top
        );
        return;
    }

    uint8_t *dst = grape_texture_pixels(destination);
    size_t dst_stride = grape_texture_stride(destination);
    int32_t dst_width = (int32_t)grape_texture_width(destination);
    int32_t dst_height = (int32_t)grape_texture_height(destination);
    float src_width = (float)grape_texture_width(source);
    float src_height = (float)grape_texture_height(source);

    int32_t x0 = (int32_t)floorf(left);
    int32_t y0 = (int32_t)floorf(top);
    int32_t x1 = (int32_t)ceilf(left + src_width);
    int32_t y1 = (int32_t)ceilf(top + src_height);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst_width) x1 = dst_width;
    if (y1 > dst_height) y1 = dst_height;

    for (int32_t y = y0; y < y1; ++y) {
        uint8_t *row = dst + (size_t)y * dst_stride;
        float source_y = (float)y - top;
        for (int32_t x = x0; x < x1; ++x) {
            float source_x = (float)x - left;
            uint8_t coverage = sample_a8_bilinear(source, source_x, source_y);
            blend_coverage(&row[x], coverage);
        }
    }
}

esp_err_t grape_text_rasterize_codepoints_a8(
    grape_glyph_cache_t *cache,
    const grape_font_t *font,
    const uint32_t *codepoints,
    size_t codepoint_count,
    const grape_path_rasterize_config_t *config,
    grape_text_raster_t *out_raster)
{
    if (!cache || !font || (!codepoints && codepoint_count != 0U) ||
        !config || !out_raster || !isfinite(config->pixels_per_unit) ||
        config->pixels_per_unit <= 0.0f || config->samples_per_axis == 0U ||
        config->samples_per_axis > 8U) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_raster = (grape_text_raster_t){0};

    grape_text_measurement_t measurement = {0};
    esp_err_t ret = grape_text_measure_codepoints(
        font,
        codepoints,
        codepoint_count,
        &measurement
    );
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t width = 0U;
    uint32_t height = 0U;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    ret = text_raster_dimensions(
        &measurement,
        config,
        &width,
        &height,
        &origin_x,
        &origin_y
    );
    if (ret != ESP_OK) {
        return ret;
    }

    grape_context_t *context = grape_glyph_cache_context(cache);
    if (!context) {
        return ESP_ERR_INVALID_STATE;
    }

    grape_texture_desc_t desc = {
        .width = width,
        .height = height,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = config->memory,
    };
    grape_texture_t *texture = NULL;
    ret = grape_texture_create(context, &desc, &texture);
    if (ret != ESP_OK) {
        return ret;
    }

    float pen_x = 0.0f;
    for (size_t i = 0; i < codepoint_count; ++i) {
        uint16_t glyph_id = 0;
        ret = grape_font_get_glyph_id(font, codepoints[i], &glyph_id);
        if (ret != ESP_OK) {
            goto fail;
        }

        grape_font_glyph_info_t glyph_info = {0};
        ret = grape_font_get_glyph_info(font, glyph_id, &glyph_info);
        if (ret != ESP_OK) {
            goto fail;
        }
        if (glyph_info.kind == GRAPE_FONT_GLYPH_COMPOSITE) {
            ret = ESP_ERR_NOT_SUPPORTED;
            goto fail;
        }

        if (glyph_info.kind == GRAPE_FONT_GLYPH_SIMPLE &&
            glyph_info.contour_count > 0) {
            grape_glyph_cache_raster_t glyph = {0};
            ret = grape_glyph_cache_acquire(
                cache,
                font,
                glyph_id,
                config->pixels_per_unit,
                config->samples_per_axis,
                &glyph
            );
            if (ret != ESP_OK) {
                goto fail;
            }

            float left =
                (pen_x + glyph.path_origin_x - origin_x) *
                config->pixels_per_unit;
            float top =
                (glyph.path_origin_y - origin_y) * config->pixels_per_unit;
            blit_a8_translated(texture, glyph.texture, left, top);
            grape_glyph_cache_release(cache, &glyph);
        }

        grape_font_glyph_metrics_t metrics = {0};
        ret = grape_font_get_glyph_metrics(font, glyph_id, &metrics);
        if (ret != ESP_OK) {
            goto fail;
        }
        pen_x += (float)metrics.advance_width;
    }

    ret = grape_texture_invalidate(texture);
    if (ret != ESP_OK) {
        goto fail;
    }

    *out_raster = (grape_text_raster_t){
        .texture = texture,
        .text_origin_x = origin_x,
        .text_origin_y = origin_y,
        .pixels_per_unit = config->pixels_per_unit,
        .advance_width = measurement.advance_width,
        .glyph_count = measurement.glyph_count,
    };
    return ESP_OK;

fail:
    grape_texture_destroy(texture);
    return ret;
}
