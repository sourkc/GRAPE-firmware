#include <float.h>
#include <stdlib.h>

#include "grape_internal.h"

/**
 * Transforms local surface coordinates into screen coordinates
 *
 * @param surface GRAPE surface
 * @param lx Local surface X coordinate
 * @param ly Local surface Y coordinate
 * @param sx Returns screen X coordinate
 * @param sy Returns screen Y coordinate
 */
static void transform_point(const grape_surface_t *surface, float lx, float ly, float *sx, float *sy)
{
    float dx = (lx - surface->transform.origin_x) * surface->transform.scale_x;
    float dy = (ly - surface->transform.origin_y) * surface->transform.scale_y;

    *sx = surface->transform.x + surface->cos_rotation * dx - surface->sin_rotation * dy;
    *sy = surface->transform.y + surface->sin_rotation * dx + surface->cos_rotation * dy;
}

/**
 * Calculates bounds of a surface on the screen
 *
 * @param surface GRAPE surface
 * @return Bounds rect of the surface
 */
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

/**
 * Recalculates derived/cached values that we need from a surface's
 * transform, so the renderer doesn't re-do expensive transform math every pixel
 *
 * @param surface GRAPE surface
 */
void grape_surface_recache(grape_surface_t *surface)
{
    GRAPE_TIME_SCOPE(SURFACE_RECACHE);

    // Cache sine and cosine of rotation
    surface->cos_rotation = cosf(surface->transform.rotation);
    surface->sin_rotation = sinf(surface->transform.rotation);

    // Cached values needed for three-shear rotation backend
    if (surface->context->rotation_backend == GRAPE_ROTATION_BACKEND_THREE_SHEAR) {
        surface->normalized_rotation =
            atan2f(surface->sin_rotation, surface->cos_rotation);
        surface->shear_x_coefficient =
            -tanf(surface->normalized_rotation * 0.5f);
        surface->shear_cache_valid = true;
    } else {
        surface->shear_cache_valid = false;
    }

    // Cache inverse scale for screen-to-local mapping
    float inv_scale_x = 1.0f / surface->transform.scale_x;
    float inv_scale_y = 1.0f / surface->transform.scale_y;

    // Cache the inverse affine transform for local X
    surface->local_x_from_screen_x = surface->cos_rotation * inv_scale_x;
    surface->local_x_from_screen_y = surface->sin_rotation * inv_scale_x;
    surface->local_x_offset = surface->transform.origin_x
                       - surface->local_x_from_screen_x * surface->transform.x
                       - surface->local_x_from_screen_y * surface->transform.y;

    // Same thing for local Y
    surface->local_y_from_screen_x = -surface->sin_rotation * inv_scale_y;
    surface->local_y_from_screen_y = surface->cos_rotation * inv_scale_y;
    surface->local_y_offset = surface->transform.origin_y
                       - surface->local_y_from_screen_x * surface->transform.x
                       - surface->local_y_from_screen_y * surface->transform.y;

    surface->bounds = grape_surface_calculate_bounds(surface);

}

/**
 * Remove a surface from the context's linked Z-sorted surface list
 * NOTE: if you want to destroy a surface use grape_surface_destroy
 *
 * @param context GRAPE context
 * @param surface GRAPE surface
 */
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

/**
 * Inserts a surface into the context's surface list in ascending Z order
 *
 * @param context GRAPE context
 * @param surface GRAPE surface to insert
 */
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

/**
 * For convenience :)
 * NOTICE: might grow into more logic in the future, please keep this
 */
#define mark_surface_coverage(surface) grape_damage_add_surface_coverage(surface)

/**
 * Creates a GRAPE surface
 *
 * @param context GRAPE context
 * @param texture Texture for the surface
 * @param out_surface Returns a surface
 * @return Returns ESP_OK on success or an error code
 */
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

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        grape_surface_remove(context, surface);
        texture->ref_count--;
        free(surface);
        return ret;
    }

    *out_surface = surface;
    return ESP_OK;
}

/**
 * Destroys a GRAPE surface
 * @param surface GRAPE surface to destroy
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_surface_destroy(grape_surface_t *surface)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_context_t *context = surface->context;
    grape_surface_remove(context, surface);
    if (surface->texture && surface->texture->ref_count) {
        surface->texture->ref_count--;
    }
    free(surface);
    return ESP_OK;
}

/**
 * Sets the texture of a given surface
 *
 * @param surface GRAPE surface
 * @param texture GRAPE texture to set
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_surface_set_texture(grape_surface_t *surface, grape_texture_t *texture)
{
    if (!surface || !texture || texture->context != surface->context) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    if (surface->texture && surface->texture->ref_count) {
        surface->texture->ref_count--;
    }
    surface->texture = texture;
    texture->ref_count++;
    grape_surface_recache(surface);

    return mark_surface_coverage(surface);
}

/**
 * Sets the transform of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @param transform Transform to set
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_surface_set_transform(grape_surface_t *surface, const grape_transform_t *transform)
{
    if (!surface || !transform || fabsf(transform->scale_x) < FLT_EPSILON || fabsf(transform->scale_y) < FLT_EPSILON) {
        return ESP_ERR_INVALID_ARG;
    }

    GRAPE_TIME_SCOPE(SURFACE_TRANSFORM);

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret == ESP_OK) {
        surface->transform = *transform;
        grape_surface_recache(surface);
        ret = mark_surface_coverage(surface);
    }

    return ret;
}

/**
 * Sets the screen position of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @param x Screen X coordinate
 * @param y Screen Y coordinate
 * @return ESP_OK on success or an error code
 */
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

/**
 * Sets the scale of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @param scale_x Horizontal scale
 * @param scale_y Vertical scale
 * @return ESP_OK on success or an error code
 */
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

/**
 * Sets the rotation of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @param radians Rotation in radians
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_surface_set_rotation(grape_surface_t *surface, float radians)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    grape_transform_t transform = surface->transform;
    transform.rotation = radians;
    return grape_surface_set_transform(surface, &transform);
}

/**
 * Sets the transform origin of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @param origin_x Local X coordinate of the transform origin
 * @param origin_y Local Y coordinate of the transform origin
 * @return ESP_OK on success or an error code
 */
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

/**
 * Sets the Z position of a GRAPE surface and re-sorts it in the surface list
 *
 * @param surface GRAPE surface
 * @param z New Z position
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_surface_set_z(grape_surface_t *surface, int32_t z)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->z == z) {
        return ESP_OK;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_surface_remove(surface->context, surface);
    surface->z = z;
    grape_surface_insert_sorted(surface->context, surface);
    return ESP_OK;
}

/**
 * Sets the opacity of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @param opacity Opacity from 0 to 255
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_surface_set_opacity(grape_surface_t *surface, uint8_t opacity)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->opacity == opacity) {
        return ESP_OK;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    surface->opacity = opacity;
    return mark_surface_coverage(surface);
}

/**
 * Sets the tint of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @param tint Tint color
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_surface_set_tint(grape_surface_t *surface, grape_color_t tint)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }

    if (surface->tint.r == tint.r &&
        surface->tint.g == tint.g &&
        surface->tint.b == tint.b &&
        surface->tint.a == tint.a) {
        return ESP_OK;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    surface->tint = tint;
    return mark_surface_coverage(surface);
}

/**
 * Sets a GRAPE surface's visibility
 *
 * @param surface GRAPE surface
 * @param visible Surface visibility
 * @return ESP_OK on success or an error code
 */
esp_err_t grape_surface_set_visible(grape_surface_t *surface, bool visible)
{
    if (!surface) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->visible == visible) {
        return ESP_OK;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    surface->visible = visible;
    grape_surface_recache(surface);
    return mark_surface_coverage(surface);
}

/**
 * Gets the transform of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @return Pointer to the surface transform, or NULL if surface is NULL
 */
const grape_transform_t *grape_surface_transform(const grape_surface_t *surface)
{
    return surface ? &surface->transform : NULL;
}

/**
 * Gets the Z position of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @return Surface Z position, or 0 if surface is NULL
 */
int32_t grape_surface_z(const grape_surface_t *surface)
{
    return surface ? surface->z : 0;
}

/**
 * Gets the opacity of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @return Surface opacity, or 0 if surface is NULL
 */
uint8_t grape_surface_opacity(const grape_surface_t *surface)
{
    return surface ? surface->opacity : 0;
}

/**
 * Gets the tint of a GRAPE surface
 *
 * @param surface GRAPE surface
 * @return Surface tint, or transparent black if surface is NULL
 */
grape_color_t grape_surface_tint(const grape_surface_t *surface)
{
    return surface ? surface->tint : (grape_color_t){0, 0, 0, 0};
}

/**
 * Gets a GRAPE surface's visibility
 *
 * @param surface GRAPE surface
 * @return Surface visibility, or false if surface is NULL
 */
bool grape_surface_visible(const grape_surface_t *surface)
{
    return surface ? surface->visible : false;
}

/**
 * Gets the texture used by a GRAPE surface
 *
 * @param surface GRAPE surface
 * @return Surface texture, or NULL if surface is NULL
 */
grape_texture_t *grape_surface_texture(grape_surface_t *surface)
{
    return surface ? surface->texture : NULL;
}
