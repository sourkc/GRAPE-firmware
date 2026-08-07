#include <math.h>

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

    ppa_fill_color_mode_t color_mode = fill_color_mode(context->display_info.format);
    if ((int)color_mode < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    ppa_fill_oper_config_t config = {
        .out = {
            .buffer = context->scratch,
            .buffer_size = context->scratch_size,
            .pic_w = (uint32_t)rect.width,
            .pic_h = (uint32_t)rect.height,
            .block_offset_x = 0,
            .block_offset_y = 0,
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

    ppa_blend_color_mode_t display_mode = blend_color_mode(context->display_info.format);
    if ((int)display_mode < 0) {
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

    grape_rect_t clipped = grape_rect_intersection(damage_rect, surface_rect);
    if (grape_rect_empty(clipped)) {
        *handled = true;
        return ESP_OK;
    }

    uint8_t alpha_scale = (uint8_t)(((uint16_t)surface->tint.a * surface->opacity + 127U) / 255U);
    if (alpha_scale == 0) {
        *handled = true;
        return ESP_OK;
    }

    uint32_t bg_x = (uint32_t)(clipped.x - damage_rect.x);
    uint32_t bg_y = (uint32_t)(clipped.y - damage_rect.y);
    uint32_t fg_x = (uint32_t)(clipped.x - surface_rect.x);
    uint32_t fg_y = (uint32_t)(clipped.y - surface_rect.y);

    ppa_blend_oper_config_t config = {
        .in_bg = {
            .buffer = context->scratch,
            .pic_w = (uint32_t)damage_rect.width,
            .pic_h = (uint32_t)damage_rect.height,
            .block_w = (uint32_t)clipped.width,
            .block_h = (uint32_t)clipped.height,
            .block_offset_x = bg_x,
            .block_offset_y = bg_y,
            .blend_cm = display_mode,
        },
        .in_fg = {
            .buffer = surface->texture->pixels,
            .pic_w = surface->texture->width,
            .pic_h = surface->texture->height,
            .block_w = (uint32_t)clipped.width,
            .block_h = (uint32_t)clipped.height,
            .block_offset_x = fg_x,
            .block_offset_y = fg_y,
            .blend_cm = PPA_BLEND_COLOR_MODE_A8,
        },
        .out = {
            .buffer = context->scratch,
            .buffer_size = context->scratch_size,
            .pic_w = (uint32_t)damage_rect.width,
            .pic_h = (uint32_t)damage_rect.height,
            .block_offset_x = bg_x,
            .block_offset_y = bg_y,
            .blend_cm = display_mode,
        },
        .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_alpha_update_mode = alpha_scale == 255 ? PPA_ALPHA_NO_CHANGE : PPA_ALPHA_SCALE,
        .fg_alpha_scale_ratio = (float)alpha_scale / 255.0f,
        .fg_fix_rgb_val = {
            .r = surface->tint.r,
            .g = surface->tint.g,
            .b = surface->tint.b,
        },
        .bg_ck_en = false,
        .fg_ck_en = false,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_blend(context->ppa_blend, &config);
    if (ret == ESP_OK) {
        *handled = true;
    }
    return ret;
}
