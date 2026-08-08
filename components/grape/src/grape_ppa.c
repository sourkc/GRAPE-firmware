#include <math.h>

#include "esp_log.h"
#include "grape_internal.h"

static ppa_blend_color_mode_t blend_color_mode(grape_pixel_format_t format)
{
    switch (format) {
        case GRAPE_PIXEL_FORMAT_RGB565:
            return PPA_BLEND_COLOR_MODE_RGB565;
        case GRAPE_PIXEL_FORMAT_RGB888:
            return PPA_BLEND_COLOR_MODE_RGB888;
        default:
            return (ppa_blend_color_mode_t)-1;
    }
}

static ppa_fill_color_mode_t fill_color_mode(grape_pixel_format_t format)
{
    switch (format) {
        case GRAPE_PIXEL_FORMAT_RGB565:
            return PPA_FILL_COLOR_MODE_RGB565;
        case GRAPE_PIXEL_FORMAT_RGB888:
            return PPA_FILL_COLOR_MODE_RGB888;
        default:
            return (ppa_fill_color_mode_t)-1;
    }
}

static ppa_srm_color_mode_t srm_color_mode(grape_pixel_format_t format)
{
    switch (format) {
        case GRAPE_PIXEL_FORMAT_A8:
            return PPA_SRM_COLOR_MODE_GRAY8;
        case GRAPE_PIXEL_FORMAT_RGB565:
            return PPA_SRM_COLOR_MODE_RGB565;
        case GRAPE_PIXEL_FORMAT_RGB888:
            return PPA_SRM_COLOR_MODE_RGB888;
        default:
            return (ppa_srm_color_mode_t)-1;
    }
}

static bool render_target_ppa_compatible(const grape_context_t *context)
{
    if (!context || !context->render_target.pixels ||
        !context->render_target.ppa_compatible) {
        return false;
    }

    size_t bpp = grape_bytes_per_pixel(context->render_target.format);
    if (bpp == 0 || context->render_target.width > SIZE_MAX / bpp) {
        return false;
    }

    return context->render_target.stride ==
               (size_t)context->render_target.width * bpp &&
           context->render_target.buffer_size <= UINT32_MAX;
}

static esp_err_t register_client(ppa_operation_t operation, ppa_client_handle_t *out_client)
{
    ppa_client_config_t config = {
        .oper_type = operation,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };

    return ppa_register_client(&config, out_client);
}

esp_err_t grape_ppa_init(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = register_client(PPA_OPERATION_FILL, &context->ppa_fill);
    if (ret != ESP_OK) {
        goto fail;
    }

    ret = register_client(PPA_OPERATION_BLEND, &context->ppa_blend);
    if (ret != ESP_OK) {
        goto fail;
    }

    ret = register_client(PPA_OPERATION_SRM, &context->ppa_srm);
    if (ret != ESP_OK) {
        goto fail;
    }

    return ESP_OK;

fail:
    grape_ppa_deinit(context);
    return ret;
}

void grape_ppa_deinit(grape_context_t *context)
{
    if (!context) {
        return;
    }

    if (context->ppa_srm) {
        ppa_unregister_client(context->ppa_srm);
        context->ppa_srm = NULL;
    }
    if (context->ppa_blend) {
        ppa_unregister_client(context->ppa_blend);
        context->ppa_blend = NULL;
    }
    if (context->ppa_fill) {
        ppa_unregister_client(context->ppa_fill);
        context->ppa_fill = NULL;
    }
}

esp_err_t grape_ppa_fill(grape_context_t *context, grape_rect_t rect, grape_color_t color)
{
    if (!context || grape_rect_empty(rect)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!context->ppa_fill) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!render_target_ppa_compatible(context)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ppa_fill_color_mode_t color_mode = fill_color_mode(context->display_info.format);
    if ((int)color_mode < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ppa_fill_oper_config_t config = {
        .out = {
            .buffer = context->render_target.pixels,
            .buffer_size = (uint32_t)context->render_target.buffer_size,
            .pic_w = context->render_target.width,
            .pic_h = context->render_target.height,
            .block_offset_x = (uint32_t)rect.x,
            .block_offset_y = (uint32_t)rect.y,
            .fill_cm = color_mode,
        },
        .fill_block_w = (uint32_t)rect.width,
        .fill_block_h = (uint32_t)rect.height,
        .fill_argb_color = {
            .a = 255,
            .r = color.r,
            .g = color.g,
            .b = color.b,
        },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_fill(context->ppa_fill, &config);
}

static bool is_identity_linear_transform(const grape_surface_t *surface)
{
    const float epsilon = 0.0001f;

    return fabsf(surface->transform.scale_x - 1.0f) <= epsilon &&
           fabsf(surface->transform.scale_y - 1.0f) <= epsilon &&
           fabsf(surface->sin_rotation) <= epsilon &&
           fabsf(surface->cos_rotation - 1.0f) <= epsilon;
}

static int32_t point_sample_screen_origin(float transformed_origin)
{
    return (int32_t)ceilf(transformed_origin - 0.5f);
}

static esp_err_t blend_a8_image(
    grape_context_t *context,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    grape_rect_t image_rect,
    grape_rect_t damage_rect,
    grape_color_t tint,
    uint8_t opacity,
    bool *handled
)
{
    if (!context || !pixels || !handled || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *handled = false;

    if (!context->ppa_blend) {
        return ESP_OK;
    }
    if (!render_target_ppa_compatible(context)) {
        return ESP_OK;
    }

    ppa_blend_color_mode_t display_mode = blend_color_mode(context->display_info.format);
    if ((int)display_mode < 0) {
        return ESP_OK;
    }

    grape_rect_t clipped = grape_rect_intersection(damage_rect, image_rect);
    if (grape_rect_empty(clipped)) {
        *handled = true;
        return ESP_OK;
    }

    uint8_t alpha_scale = (uint8_t)(((uint16_t)tint.a * opacity + 127U) / 255U);
    if (alpha_scale == 0) {
        *handled = true;
        return ESP_OK;
    }

    uint32_t bg_x = (uint32_t)clipped.x;
    uint32_t bg_y = (uint32_t)clipped.y;
    uint32_t fg_x = (uint32_t)(clipped.x - image_rect.x);
    uint32_t fg_y = (uint32_t)(clipped.y - image_rect.y);

    ppa_blend_oper_config_t config = {
        .in_bg = {
            .buffer = context->render_target.pixels,
            .pic_w = context->render_target.width,
            .pic_h = context->render_target.height,
            .block_w = (uint32_t)clipped.width,
            .block_h = (uint32_t)clipped.height,
            .block_offset_x = bg_x,
            .block_offset_y = bg_y,
            .blend_cm = display_mode,
        },
        .in_fg = {
            .buffer = (void *)pixels,
            .pic_w = width,
            .pic_h = height,
            .block_w = (uint32_t)clipped.width,
            .block_h = (uint32_t)clipped.height,
            .block_offset_x = fg_x,
            .block_offset_y = fg_y,
            .blend_cm = PPA_BLEND_COLOR_MODE_A8,
        },
        .out = {
            .buffer = context->render_target.pixels,
            .buffer_size = (uint32_t)context->render_target.buffer_size,
            .pic_w = context->render_target.width,
            .pic_h = context->render_target.height,
            .block_offset_x = bg_x,
            .block_offset_y = bg_y,
            .blend_cm = display_mode,
        },
        .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_alpha_update_mode = alpha_scale == 255 ? PPA_ALPHA_NO_CHANGE : PPA_ALPHA_SCALE,
        .fg_alpha_scale_ratio = (float)alpha_scale / 255.0f,
        .fg_fix_rgb_val = {
            .r = tint.r,
            .g = tint.g,
            .b = tint.b,
        },
        .bg_ck_en = false,
        .fg_ck_en = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret;
    GRAPE_TIME_BLOCK(PPA_BLEND_HW) {
        ret = ppa_do_blend(context->ppa_blend, &config);
    }
    if (ret == ESP_OK) {
        *handled = true;
    }
    return ret;
}


static esp_err_t rotated_image_bounds(
    const grape_shear_image_t *input_image,
    bool clockwise,
    grape_rect_t *out_bounds
)
{
    if (!input_image || !out_bounds || input_image->width == 0 || input_image->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    float left = input_image->left;
    float top = input_image->top;
    float right = input_image->left + (float)input_image->width;
    float bottom = input_image->top + (float)input_image->height;

    float m00 = clockwise ? 0.0f : 0.0f;
    float m01 = clockwise ? 1.0f : -1.0f;
    float m10 = clockwise ? -1.0f : 1.0f;
    float m11 = clockwise ? 0.0f : 0.0f;

    float xs[4] = {
        m00 * left + m01 * top,
        m00 * right + m01 * top,
        m00 * left + m01 * bottom,
        m00 * right + m01 * bottom,
    };

    float ys[4] = {
        m10 * left + m11 * top,
        m10 * right + m11 * top,
        m10 * left + m11 * bottom,
        m10 * right + m11 * bottom,
    };

    float min_x = xs[0];
    float max_x = xs[0];
    float min_y = ys[0];
    float max_y = ys[0];

    for (size_t i = 1; i < 4; ++i) {
        if (xs[i] < min_x) {
            min_x = xs[i];
        }
        if (xs[i] > max_x) {
            max_x = xs[i];
        }
        if (ys[i] < min_y) {
            min_y = ys[i];
        }
        if (ys[i] > max_y) {
            max_y = ys[i];
        }
    }

    int32_t bound_left = (int32_t)floorf(min_x + 0.0001f);
    int32_t bound_top = (int32_t)floorf(min_y + 0.0001f);
    int32_t bound_right = (int32_t)ceilf(max_x - 0.0001f);
    int32_t bound_bottom = (int32_t)ceilf(max_y - 0.0001f);

    if (bound_right <= bound_left || bound_bottom <= bound_top) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_bounds = (grape_rect_t) {
        .x = bound_left,
        .y = bound_top,
        .width = bound_right - bound_left,
        .height = bound_bottom - bound_top,
    };

    return ESP_OK;
}

esp_err_t grape_ppa_rotate_a8(
    grape_context_t *context,
    const grape_shear_image_t *input_image,
    bool clockwise,
    uint8_t *output_buffer,
    size_t output_buffer_size,
    grape_shear_image_t *out_image
)
{
    if (!context || !input_image || !input_image->pixels || !output_buffer || !out_image) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!context->ppa_srm || input_image->width == 0 || input_image->height == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ppa_srm_color_mode_t color_mode = srm_color_mode(GRAPE_PIXEL_FORMAT_A8);
    if ((int)color_mode < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (input_image->stride == 0 || input_image->stride < input_image->width) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_rect_t output_bounds;
    esp_err_t ret = rotated_image_bounds(input_image, clockwise, &output_bounds);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t required_size = (size_t)output_bounds.width * (size_t)output_bounds.height;
    if (required_size > output_buffer_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t ppa_output_size =
    output_buffer_size &
    ~(CONFIG_CACHE_L2_CACHE_LINE_SIZE - 1U);

    if (ppa_output_size < required_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (((uintptr_t)output_buffer %
     CONFIG_CACHE_L2_CACHE_LINE_SIZE) != 0) {
        return ESP_ERR_INVALID_ARG;
     }

    ppa_srm_oper_config_t config = {
        .in = {
            .buffer = input_image->pixels,
            .pic_w = (uint32_t)input_image->stride,
            .pic_h = input_image->height,
            .block_w = input_image->width,
            .block_h = input_image->height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = color_mode,
        },
        .out = {
            .buffer = output_buffer,
            .buffer_size = ppa_output_size,
            .pic_w = (uint32_t)output_bounds.width,
            .pic_h = (uint32_t)output_bounds.height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = color_mode,
        },
        .rotation_angle = clockwise ?
            PPA_SRM_ROTATION_ANGLE_270 :
            PPA_SRM_ROTATION_ANGLE_90,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .alpha_fix_val = 0,
        .alpha_scale_ratio = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
        .user_data = NULL,
    };

    ESP_LOGI(
        "grape_ppa",
        "SRM out=%p size=%u in=%p %ux%u -> %ux%u",
        config.out.buffer,
        (unsigned)config.out.buffer_size,
        config.in.buffer,
        config.in.block_w,
        config.in.block_h,
        config.out.pic_w,
        config.out.pic_h
    );
    GRAPE_TIME_BLOCK(PPA_ROTATE) {
        ret = ppa_do_scale_rotate_mirror(context->ppa_srm, &config);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    *out_image = (grape_shear_image_t) {
        .pixels = output_buffer,
        .stride = (size_t)output_bounds.width,
        .left = (float)output_bounds.x,
        .top = (float)output_bounds.y,
        .width = (uint32_t)output_bounds.width,
        .height = (uint32_t)output_bounds.height,
    };

    return ESP_OK;
}

esp_err_t grape_ppa_blend_surface(grape_context_t *context, const grape_surface_t *surface,
                                  grape_rect_t damage_rect, bool *handled)
{
    if (!context || !surface || !handled) {
        return ESP_ERR_INVALID_ARG;
    }

    *handled = false;

    if (!context->ppa_blend || !surface->texture ||
        surface->texture->format != GRAPE_PIXEL_FORMAT_A8 ||
        !is_identity_linear_transform(surface)) {
        return ESP_OK;
    }

    float left = surface->transform.x - surface->transform.origin_x;
    float top = surface->transform.y - surface->transform.origin_y;

    grape_rect_t surface_rect = {
        .x = point_sample_screen_origin(left),
        .y = point_sample_screen_origin(top),
        .width = (int32_t)surface->texture->width,
        .height = (int32_t)surface->texture->height,
    };

    return blend_a8_image(
        context,
        surface->texture->pixels,
        surface->texture->width,
        surface->texture->height,
        surface_rect,
        damage_rect,
        surface->tint,
        surface->opacity,
        handled
    );
}

esp_err_t grape_ppa_blend_a8_image(
    grape_context_t *context,
    const uint8_t *pixels,
    uint32_t width,
    uint32_t height,
    float screen_left,
    float screen_top,
    grape_rect_t damage_rect,
    grape_color_t tint,
    uint8_t opacity,
    bool *handled
)
{
    if (!handled) {
        return ESP_ERR_INVALID_ARG;
    }

    *handled = false;

    if (width > INT32_MAX || height > INT32_MAX ||
        !isfinite(screen_left) || !isfinite(screen_top)) {
        return ESP_OK;
    }

    grape_rect_t image_rect = {
        .x = point_sample_screen_origin(screen_left),
        .y = point_sample_screen_origin(screen_top),
        .width = (int32_t)width,
        .height = (int32_t)height,
    };

    return blend_a8_image(
        context,
        pixels,
        width,
        height,
        image_rect,
        damage_rect,
        tint,
        opacity,
        handled
    );
}
