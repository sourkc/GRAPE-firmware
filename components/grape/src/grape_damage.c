#include <limits.h>

#include "grape_internal.h"

bool grape_rect_empty(grape_rect_t rect)
{
    return rect.width <= 0 || rect.height <= 0;
}

grape_rect_t grape_rect_intersection(grape_rect_t a, grape_rect_t b)
{
    int32_t x0 = a.x > b.x ? a.x : b.x;
    int32_t y0 = a.y > b.y ? a.y : b.y;
    int32_t ax1 = a.x + a.width;
    int32_t ay1 = a.y + a.height;
    int32_t bx1 = b.x + b.width;
    int32_t by1 = b.y + b.height;
    int32_t x1 = ax1 < bx1 ? ax1 : bx1;
    int32_t y1 = ay1 < by1 ? ay1 : by1;

    if (x1 <= x0 || y1 <= y0) {
        return (grape_rect_t){0, 0, 0, 0};
    }

    return (grape_rect_t){x0, y0, x1 - x0, y1 - y0};
}

grape_rect_t grape_rect_union(grape_rect_t a, grape_rect_t b)
{
    if (grape_rect_empty(a)) {
        return b;
    }
    if (grape_rect_empty(b)) {
        return a;
    }

    int32_t x0 = a.x < b.x ? a.x : b.x;
    int32_t y0 = a.y < b.y ? a.y : b.y;
    int32_t ax1 = a.x + a.width;
    int32_t ay1 = a.y + a.height;
    int32_t bx1 = b.x + b.width;
    int32_t by1 = b.y + b.height;
    int32_t x1 = ax1 > bx1 ? ax1 : bx1;
    int32_t y1 = ay1 > by1 ? ay1 : by1;

    return (grape_rect_t){x0, y0, x1 - x0, y1 - y0};
}

bool grape_rect_touches(grape_rect_t a, grape_rect_t b)
{
    if (grape_rect_empty(a) || grape_rect_empty(b)) {
        return false;
    }

    int32_t ax1 = a.x + a.width;
    int32_t ay1 = a.y + a.height;
    int32_t bx1 = b.x + b.width;
    int32_t by1 = b.y + b.height;

    return a.x <= bx1 && b.x <= ax1 && a.y <= by1 && b.y <= ay1;
}

esp_err_t grape_damage_add(grape_context_t *context, grape_rect_t rect)
{
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_ADD
    int64_t profile_start_us = grape_profile_timestamp();
#endif

    grape_rect_t screen = {
        .x = 0,
        .y = 0,
        .width = (int32_t)context->display_info.width,
        .height = (int32_t)context->display_info.height,
    };

    rect = grape_rect_intersection(rect, screen);
    if (grape_rect_empty(rect)) {
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_ADD
        grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_ADD, grape_profile_timestamp() - profile_start_us);
#endif
        return ESP_OK;
    }

    bool merged;
    do {
        merged = false;
        for (size_t i = 0; i < context->damage_count; ++i) {
            if (!grape_rect_touches(context->damage[i], rect)) {
                continue;
            }

            rect = grape_rect_union(context->damage[i], rect);
            context->damage[i] = context->damage[context->damage_count - 1];
            context->damage_count--;
            merged = true;
            break;
        }
    } while (merged);

    if (context->damage_count < CONFIG_GRAPE_MAX_DAMAGE_RECTS) {
        context->damage[context->damage_count++] = rect;
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_ADD
        grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_ADD, grape_profile_timestamp() - profile_start_us);
#endif
        return ESP_OK;
    }

    grape_rect_t combined = rect;
    for (size_t i = 0; i < context->damage_count; ++i) {
        combined = grape_rect_union(combined, context->damage[i]);
    }

    context->damage[0] = grape_rect_intersection(combined, screen);
    context->damage_count = 1;
#if GRAPE_PROFILE_ENABLE && GRAPE_PROFILE_DAMAGE_ADD
    grape_profile_record(GRAPE_PROFILE_METRIC_DAMAGE_ADD, grape_profile_timestamp() - profile_start_us);
#endif
    return ESP_OK;
}

void grape_damage_all(grape_context_t *context)
{
    context->damage_count = 1;
    context->damage[0] = (grape_rect_t){
        .x = 0,
        .y = 0,
        .width = (int32_t)context->display_info.width,
        .height = (int32_t)context->display_info.height,
    };
}
