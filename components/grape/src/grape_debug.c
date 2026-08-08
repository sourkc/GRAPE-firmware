#include <string.h>

#include "grape_internal.h"

#define GRAPE_DEBUG_LAYER_BIT(layer) (1U << (uint32_t)(layer))
#define GRAPE_DEBUG_DAMAGE_BORDER_WIDTH 1

static const grape_color_t damage_fill = {
    .r = 255,
    .g = 32,
    .b = 32,
    .a = 40,
};

static const grape_color_t damage_border = {
    .r = 255,
    .g = 32,
    .b = 32,
    .a = 160,
};

typedef struct {
    grape_debug_layer_t layer;
    void (*prepare)(grape_context_t *context);
    void (*render)(grape_context_t *context, grape_rect_t rect);
    void (*finish)(grape_context_t *context);
    void (*abort)(grape_context_t *context);
    void (*disable)(grape_context_t *context);
} grape_debug_layer_ops_t;

static grape_rect_t screen_bounds(const grape_context_t *context)
{
    return (grape_rect_t){
        .x = 0,
        .y = 0,
        .width = (int32_t)context->display_info.width,
        .height = (int32_t)context->display_info.height,
    };
}

static bool layer_enabled(const grape_context_t *context, grape_debug_layer_t layer)
{
    return (context->debug.enabled_mask & GRAPE_DEBUG_LAYER_BIT(layer)) != 0;
}

static void add_render_damage(grape_context_t *context, grape_rect_t rect)
{
    grape_debug_state_t *debug = &context->debug;
    rect = grape_rect_intersection(rect, screen_bounds(context));
    if (grape_rect_empty(rect)) {
        return;
    }

    if (debug->render_damage_count < CONFIG_GRAPE_MAX_DAMAGE_RECTS) {
        debug->render_damage[debug->render_damage_count++] = rect;
        return;
    }

    grape_rect_t combined = rect;
    for (size_t i = 0; i < debug->render_damage_count; ++i) {
        combined = grape_rect_union(combined, debug->render_damage[i]);
    }

    debug->render_damage[0] = grape_rect_intersection(combined, screen_bounds(context));
    debug->render_damage_count = 1;
}

static uint8_t blend_channel(uint8_t destination, uint8_t source, uint8_t alpha)
{
    uint32_t inverse = 255U - alpha;
    return (uint8_t)(((uint32_t)source * alpha +
                      (uint32_t)destination * inverse + 127U) / 255U);
}

static void blend_pixel_rgb565(uint8_t *destination, grape_color_t color)
{
    uint16_t packed = (uint16_t)destination[0] |
                      ((uint16_t)destination[1] << 8);

    uint8_t r5 = (uint8_t)((packed >> 11) & 0x1FU);
    uint8_t g6 = (uint8_t)((packed >> 5) & 0x3FU);
    uint8_t b5 = (uint8_t)(packed & 0x1FU);

    uint8_t r = (uint8_t)((r5 << 3) | (r5 >> 2));
    uint8_t g = (uint8_t)((g6 << 2) | (g6 >> 4));
    uint8_t b = (uint8_t)((b5 << 3) | (b5 >> 2));

    r = blend_channel(r, color.r, color.a);
    g = blend_channel(g, color.g, color.a);
    b = blend_channel(b, color.b, color.a);

    packed = (uint16_t)(((uint16_t)(r >> 3) << 11) |
                        ((uint16_t)(g >> 2) << 5) |
                        (uint16_t)(b >> 3));

    destination[0] = (uint8_t)(packed & 0xFFU);
    destination[1] = (uint8_t)(packed >> 8);
}

static void blend_pixel_rgb888(uint8_t *destination, grape_color_t color)
{
    destination[0] = blend_channel(destination[0], color.r, color.a);
    destination[1] = blend_channel(destination[1], color.g, color.a);
    destination[2] = blend_channel(destination[2], color.b, color.a);
}

static void draw_filled_rect(grape_context_t *context,
                             grape_rect_t render_rect,
                             grape_rect_t overlay_rect,
                             grape_color_t color)
{
    grape_rect_t clipped = grape_rect_intersection(render_rect, overlay_rect);
    if (grape_rect_empty(clipped) || color.a == 0) {
        return;
    }

    size_t bpp = grape_bytes_per_pixel(context->display_info.format);
    int32_t start_x = clipped.x - render_rect.x;
    int32_t start_y = clipped.y - render_rect.y;

    for (int32_t y = 0; y < clipped.height; ++y) {
        uint8_t *destination = context->scratch +
            (((size_t)(start_y + y) * (size_t)render_rect.width +
              (size_t)start_x) * bpp);

        for (int32_t x = 0; x < clipped.width; ++x) {
            if (context->display_info.format == GRAPE_PIXEL_FORMAT_RGB565) {
                blend_pixel_rgb565(destination, color);
            } else {
                blend_pixel_rgb888(destination, color);
            }
            destination += bpp;
        }
    }
}

static void draw_rect_border(grape_context_t *context,
                             grape_rect_t render_rect,
                             grape_rect_t overlay_rect,
                             grape_color_t color)
{
    if (grape_rect_empty(overlay_rect)) {
        return;
    }

    int32_t border = GRAPE_DEBUG_DAMAGE_BORDER_WIDTH;
    if (overlay_rect.width < border) {
        border = overlay_rect.width;
    }
    if (overlay_rect.height < border) {
        border = overlay_rect.height;
    }
    if (border <= 0) {
        return;
    }

    grape_rect_t top = {
        .x = overlay_rect.x,
        .y = overlay_rect.y,
        .width = overlay_rect.width,
        .height = border,
    };
    grape_rect_t bottom = {
        .x = overlay_rect.x,
        .y = overlay_rect.y + overlay_rect.height - border,
        .width = overlay_rect.width,
        .height = border,
    };
    grape_rect_t left = {
        .x = overlay_rect.x,
        .y = overlay_rect.y + border,
        .width = border,
        .height = overlay_rect.height - 2 * border,
    };
    grape_rect_t right = {
        .x = overlay_rect.x + overlay_rect.width - border,
        .y = overlay_rect.y + border,
        .width = border,
        .height = overlay_rect.height - 2 * border,
    };

    draw_filled_rect(context, render_rect, top, color);
    draw_filled_rect(context, render_rect, bottom, color);
    draw_filled_rect(context, render_rect, left, color);
    draw_filled_rect(context, render_rect, right, color);
}

static void damage_rects_prepare(grape_context_t *context)
{
    grape_debug_state_t *debug = &context->debug;

    debug->damage_rects_current_count = context->damage.final_rect_count;
    if (debug->damage_rects_current_count > 0) {
        memcpy(
            debug->damage_rects_current,
            context->damage.final_rects,
            debug->damage_rects_current_count * sizeof(debug->damage_rects_current[0])
        );
    }

    for (size_t i = 0; i < debug->damage_rects_previous_count; ++i) {
        add_render_damage(context, debug->damage_rects_previous[i]);
    }
}

static void damage_rects_render(grape_context_t *context, grape_rect_t rect)
{
    const grape_debug_state_t *debug = &context->debug;

    for (size_t i = 0; i < debug->damage_rects_current_count; ++i) {
        grape_rect_t damage = debug->damage_rects_current[i];
        if (grape_rect_empty(grape_rect_intersection(rect, damage))) {
            continue;
        }

        draw_filled_rect(context, rect, damage, damage_fill);
        draw_rect_border(context, rect, damage, damage_border);
    }
}

static void damage_rects_finish(grape_context_t *context)
{
    grape_debug_state_t *debug = &context->debug;

    debug->damage_rects_previous_count = debug->damage_rects_current_count;
    if (debug->damage_rects_previous_count > 0) {
        memcpy(
            debug->damage_rects_previous,
            debug->damage_rects_current,
            debug->damage_rects_previous_count * sizeof(debug->damage_rects_previous[0])
        );
    }

    debug->damage_rects_current_count = 0;
}

static void damage_rects_abort(grape_context_t *context)
{
    context->debug.damage_rects_current_count = 0;
}

static void damage_rects_disable(grape_context_t *context)
{
    grape_debug_state_t *debug = &context->debug;

    for (size_t i = 0; i < debug->damage_rects_previous_count; ++i) {
        add_render_damage(context, debug->damage_rects_previous[i]);
    }

    debug->damage_rects_previous_count = 0;
    debug->damage_rects_current_count = 0;
}

static const grape_debug_layer_ops_t debug_layers[] = {
    {
        .layer = GRAPE_DEBUG_LAYER_DAMAGE_RECTS,
        .prepare = damage_rects_prepare,
        .render = damage_rects_render,
        .finish = damage_rects_finish,
        .abort = damage_rects_abort,
        .disable = damage_rects_disable,
    },
};

static const grape_debug_layer_ops_t *find_layer(grape_debug_layer_t layer)
{
    for (size_t i = 0; i < sizeof(debug_layers) / sizeof(debug_layers[0]); ++i) {
        if (debug_layers[i].layer == layer) {
            return &debug_layers[i];
        }
    }

    return NULL;
}

esp_err_t grape_debug_set_layer_enabled(grape_context_t *context,
                                        grape_debug_layer_t layer,
                                        bool enabled)
{
    if (!context || (unsigned)layer >= GRAPE_DEBUG_LAYER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const grape_debug_layer_ops_t *ops = find_layer(layer);
    if (!ops) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool current = layer_enabled(context, layer);
    if (current == enabled) {
        return ESP_OK;
    }

    if (enabled) {
        context->debug.enabled_mask |= GRAPE_DEBUG_LAYER_BIT(layer);
    } else {
        if (ops->disable) {
            ops->disable(context);
        }
        context->debug.enabled_mask &= ~GRAPE_DEBUG_LAYER_BIT(layer);
    }

    return ESP_OK;
}

bool grape_debug_is_layer_enabled(const grape_context_t *context,
                                  grape_debug_layer_t layer)
{
    if (!context || (unsigned)layer >= GRAPE_DEBUG_LAYER_COUNT) {
        return false;
    }

    return layer_enabled(context, layer);
}

esp_err_t grape_debug_prepare_frame(grape_context_t *context)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(debug_layers) / sizeof(debug_layers[0]); ++i) {
        const grape_debug_layer_ops_t *ops = &debug_layers[i];
        if (layer_enabled(context, ops->layer) && ops->prepare) {
            ops->prepare(context);
        }
    }

    return ESP_OK;
}

void grape_debug_render(grape_context_t *context, grape_rect_t rect)
{
    if (!context) {
        return;
    }

    for (size_t i = 0; i < sizeof(debug_layers) / sizeof(debug_layers[0]); ++i) {
        const grape_debug_layer_ops_t *ops = &debug_layers[i];
        if (layer_enabled(context, ops->layer) && ops->render) {
            ops->render(context, rect);
        }
    }
}

void grape_debug_finish_frame(grape_context_t *context)
{
    if (!context) {
        return;
    }

    for (size_t i = 0; i < sizeof(debug_layers) / sizeof(debug_layers[0]); ++i) {
        const grape_debug_layer_ops_t *ops = &debug_layers[i];
        if (layer_enabled(context, ops->layer) && ops->finish) {
            ops->finish(context);
        }
    }

    context->debug.render_damage_count = 0;
}

void grape_debug_reset_frame(grape_context_t *context)
{
    if (!context) {
        return;
    }

    for (size_t i = 0; i < sizeof(debug_layers) / sizeof(debug_layers[0]); ++i) {
        const grape_debug_layer_ops_t *ops = &debug_layers[i];
        if (layer_enabled(context, ops->layer) && ops->abort) {
            ops->abort(context);
        }
    }
}
