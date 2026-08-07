#include <float.h>
#include <stdlib.h>

#include "grape_internal.h"

/* Transforms local surface coordinates into screen coordinates */
static void transform_point(const grape_surface_t *surface, float lx, float ly, float *sx, float *sy)
{
    float dx = (lx - surface->transform.origin_x) * surface->transform.scale_x;
    float dy = (ly - surface->transform.origin_y) * surface->transform.scale_y;

    *sx = surface->transform.x + surface->cos_rotation * dx - surface->sin_rotation * dy;
    *sy = surface->transform.y + surface->sin_rotation * dx + surface->cos_rotation * dy;
}

grape_rect_t grape_surface_calculate_bounds(const grape_surface_t *surface)
{
    if (!surface || !surface->texture || !surface->visible) {
        return (grape_rect_t){0, 0, 0, 0};
    }

    float width = (float)surface->texture->width;
    float height = (float)surface->texture->height;
    float xs[4];
    float ys[4];

    transform_point(surface, 0.0f, 0.0f, &xs[0], &ys[0]);
    transform_point(surface, width, 0.0f, &xs[1], &ys[1]);
    transform_point(surface, 0.0f, height, &xs[2], &ys[2]);
    transform_point(surface, width, height, &xs[3], &ys[3]);

    float min_x = xs[0];
    float max_x = xs[0];
    float min_y = ys[0];
    float max_y = ys[0];

    for (int i = 1; i < 4; ++i) {
        if (xs[i] < min_x) min_x = xs[i];
        if (xs[i] > max_x) max_x = xs[i];
        if (ys[i] < min_y) min_y = ys[i];
        if (ys[i] > max_y) max_y = ys[i];
    }

    int32_t x0 = (int32_t)floorf(min_x);
    int32_t y0 = (int32_t)floorf(min_y);
    int32_t x1 = (int32_t)ceilf(max_x);
    int32_t y1 = (int32_t)ceilf(max_y);

    return (grape_rect_t){x0, y0, x1 - x0, y1 - y0};
}

void grape_surface_recache(grape_surface_t *surface)
{
    surface->cos_rotation = cosf(surface->transform.rotation);
    surface->sin_rotation = sinf(surface->transform.rotation);
    surface->bounds = grape_surface_calculate_bounds(surface);
}

void grape_surface_remove(grape_context_t *context, grape_surface_t *surface)
{
    if (surface->prev) {
        surface->prev->next = surface->next;
    } else if (context->surfaces == surface) {
        context->surfaces = surface->next;
    }

    if (surface->next) {
        surface->next->prev = surface->prev;
    }

    surface->prev = NULL;
    surface->next = NULL;
}

void grape_surface_insert_sorted(grape_context_t *context, grape_surface_t *surface)
{
    if (!context->surfaces) {
        context->surfaces = surface;
        return;
    }

    grape_surface_t *cursor = context->surfaces;
    grape_surface_t *previous = NULL;

    while (cursor && cursor->z <= surface->z) {
        previous = cursor;
        cursor = cursor->next;
    }

    surface->prev = previous;
    surface->next = cursor;

    if (previous) {
        previous->next = surface;
    } else {
        context->surfaces = surface;
    }

    if (cursor) {
        cursor->prev = surface;
    }
}

static esp_err_t surface_damage_change(grape_surface_t *surface, grape_rect_t old_bounds)
{
    if (surface->visible) {
        grape_damage_add(surface->context, surface->bounds);
    }
    if (!grape_rect_empty(old_bounds)) {
        grape_damage_add(surface->context, old_bounds);
    }
    return ESP_OK;
}

esp_err_t grape_surface_create(grape_context_t *context, grape_texture_t *texture, grape_surface_t **out_surface)
{
    if (!context || !texture || !out_surface || texture->context != context) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_surface_t *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        return ESP_ERR_NO_MEM;
    }

    surface->context = context;
    surface->texture = texture;
    surface->transform = (grape_transform_t)GRAPE_TRANSFORM_DEFAULT();
    surface->tint = (grape_color_t){255, 255, 255, 255};
    surface->opacity = 255;
    surface->visible = true;
    surface->z = 0;
    texture->ref_count++;

    grape_surface_recache(surface);
    grape_surface_insert_sorted(context, surface);
    grape_damage_add(context, surface->bounds);

    *out_surface = surface;
    return ESP_OK;
}

esp_err_t grape_surface_destroy(grape_surface_t *surface)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_context_t *context = surface->context;
    if (surface->visible) {
        grape_damage_add(context, surface->bounds);
    }

    grape_surface_remove(context, surface);
    if (surface->texture && surface->texture->ref_count) {
        surface->texture->ref_count--;
    }
    free(surface);
    return ESP_OK;
}

esp_err_t grape_surface_set_texture(grape_surface_t *surface, grape_texture_t *texture)
{
    if (!surface || !texture || texture->context != surface->context) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_rect_t old_bounds = surface->visible ? surface->bounds : (grape_rect_t){0};
    if (surface->texture && surface->texture->ref_count) {
        surface->texture->ref_count--;
    }
    surface->texture = texture;
    texture->ref_count++;
    grape_surface_recache(surface);
    return surface_damage_change(surface, old_bounds);
}

esp_err_t grape_surface_set_transform(grape_surface_t *surface, const grape_transform_t *transform)
{
    if (!surface || !transform || fabsf(transform->scale_x) < FLT_EPSILON || fabsf(transform->scale_y) < FLT_EPSILON) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_rect_t old_bounds = surface->visible ? surface->bounds : (grape_rect_t){0};
    surface->transform = *transform;
    grape_surface_recache(surface);
    return surface_damage_change(surface, old_bounds);
}

esp_err_t grape_surface_set_position(grape_surface_t *surface, float x, float y)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    grape_transform_t transform = surface->transform;
    transform.x = x;
    transform.y = y;
    return grape_surface_set_transform(surface, &transform);
}

esp_err_t grape_surface_set_scale(grape_surface_t *surface, float scale_x, float scale_y)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    grape_transform_t transform = surface->transform;
    transform.scale_x = scale_x;
    transform.scale_y = scale_y;
    return grape_surface_set_transform(surface, &transform);
}

esp_err_t grape_surface_set_rotation(grape_surface_t *surface, float radians)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    grape_transform_t transform = surface->transform;
    transform.rotation = radians;
    return grape_surface_set_transform(surface, &transform);
}

esp_err_t grape_surface_set_origin(grape_surface_t *surface, float origin_x, float origin_y)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    grape_transform_t transform = surface->transform;
    transform.origin_x = origin_x;
    transform.origin_y = origin_y;
    return grape_surface_set_transform(surface, &transform);
}

esp_err_t grape_surface_set_z(grape_surface_t *surface, int32_t z)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->z == z) {
        return ESP_OK;
    }

    grape_rect_t bounds = surface->visible ? surface->bounds : (grape_rect_t){0};
    grape_surface_remove(surface->context, surface);
    surface->z = z;
    grape_surface_insert_sorted(surface->context, surface);
    return grape_damage_add(surface->context, bounds);
}

esp_err_t grape_surface_set_opacity(grape_surface_t *surface, uint8_t opacity)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->opacity == opacity) {
        return ESP_OK;
    }
    surface->opacity = opacity;
    return surface->visible ? grape_damage_add(surface->context, surface->bounds) : ESP_OK;
}

esp_err_t grape_surface_set_tint(grape_surface_t *surface, grape_color_t tint)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    surface->tint = tint;
    return surface->visible ? grape_damage_add(surface->context, surface->bounds) : ESP_OK;
}

esp_err_t grape_surface_set_visible(grape_surface_t *surface, bool visible)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->visible == visible) {
        return ESP_OK;
    }

    grape_rect_t old_bounds = surface->visible ? surface->bounds : (grape_rect_t){0};
    surface->visible = visible;
    grape_surface_recache(surface);
    return surface_damage_change(surface, old_bounds);
}

const grape_transform_t *grape_surface_transform(const grape_surface_t *surface)
{
    return surface ? &surface->transform : NULL;
}

int32_t grape_surface_z(const grape_surface_t *surface)
{
    return surface ? surface->z : 0;
}

uint8_t grape_surface_opacity(const grape_surface_t *surface)
{
    return surface ? surface->opacity : 0;
}

grape_color_t grape_surface_tint(const grape_surface_t *surface)
{
    return surface ? surface->tint : (grape_color_t){0, 0, 0, 0};
}

bool grape_surface_visible(const grape_surface_t *surface)
{
    return surface ? surface->visible : false;
}

grape_texture_t *grape_surface_texture(grape_surface_t *surface)
{
    return surface ? surface->texture : NULL;
}
