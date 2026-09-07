#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "grape_internal.h"

static bool surface_texture_mode_valid(grape_surface_texture_mode_t mode)
{
    return mode >= GRAPE_SURFACE_TEXTURE_STRETCH &&
           mode <= GRAPE_SURFACE_TEXTURE_CENTER;
}

static bool surface_texture_filter_valid(grape_texture_filter_t filter)
{
    return filter >= GRAPE_TEXTURE_FILTER_NEAREST &&
           filter <= GRAPE_TEXTURE_FILTER_LINEAR;
}

static bool surface_aa_valid(grape_surface_aa_t aa)
{
    return aa >= GRAPE_SURFACE_AA_NONE &&
           aa <= GRAPE_SURFACE_AA_COVERAGE_4X;
}

static void surface_free_shaders(grape_surface_shader_instance_t *shaders, size_t shader_count)
{
    if (!shaders) {
        return;
    }
    for (size_t i = 0U; i < shader_count; ++i) {
        free(shaders[i].uniforms);
    }
    free(shaders);
}

static esp_err_t surface_copy_shaders(const grape_surface_shader_desc_t *descs,
                                      size_t shader_count,
                                      grape_surface_shader_instance_t **out_shaders)
{
    if (!out_shaders || (shader_count > 0U && !descs)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_shaders = NULL;
    if (shader_count == 0U) {
        return ESP_OK;
    }
    if (shader_count > SIZE_MAX / sizeof(grape_surface_shader_instance_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_surface_shader_instance_t *shaders = calloc(shader_count, sizeof(*shaders));
    if (!shaders) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0U; i < shader_count; ++i) {
        const grape_shader_program_t *program = descs[i].program;
        if (!program || !program->eval ||
            (program->uniform_size > 0U && !descs[i].uniforms)) {
            surface_free_shaders(shaders, shader_count);
            return ESP_ERR_INVALID_ARG;
        }

        shaders[i].program = program;
        if (program->uniform_size > 0U) {
            shaders[i].uniforms = malloc(program->uniform_size);
            if (!shaders[i].uniforms) {
                surface_free_shaders(shaders, shader_count);
                return ESP_ERR_NO_MEM;
            }
            memcpy(shaders[i].uniforms, descs[i].uniforms, program->uniform_size);
        }
    }

    *out_shaders = shaders;
    return ESP_OK;
}

static bool surface_shader_chain_can_start_without_texture(
    const grape_surface_shader_instance_t *shaders,
    size_t shader_count)
{
    return shader_count > 0U && shaders && shaders[0].program &&
           (shaders[0].program->flags & GRAPE_SHADER_PROGRAM_USES_SOURCE_COLOR) == 0U;
}

static void surface_recache_texture_mapping(grape_surface_t *surface)
{
    surface->texture_from_local_x_scale = 0.0f;
    surface->texture_from_local_y_scale = 0.0f;
    surface->texture_from_local_x_offset = 0.0f;
    surface->texture_from_local_y_offset = 0.0f;
    surface->texture_local_left = 0.0f;
    surface->texture_local_top = 0.0f;
    surface->texture_local_right = (float)surface->width;
    surface->texture_local_bottom = (float)surface->height;
    surface->texture_mapping_repeat = false;
    surface->texture_mapping_identity = false;

    if (!surface->texture || surface->width == 0U || surface->height == 0U) {
        return;
    }

    const float surface_width = (float)surface->width;
    const float surface_height = (float)surface->height;
    const float texture_width = (float)surface->texture->width;
    const float texture_height = (float)surface->texture->height;

    switch (surface->texture_mode) {
        case GRAPE_SURFACE_TEXTURE_STRETCH:
            surface->texture_from_local_x_scale = texture_width / surface_width;
            surface->texture_from_local_y_scale = texture_height / surface_height;
            break;

        case GRAPE_SURFACE_TEXTURE_TILE:
            surface->texture_from_local_x_scale = 1.0f;
            surface->texture_from_local_y_scale = 1.0f;
            surface->texture_mapping_repeat = true;
            break;

        case GRAPE_SURFACE_TEXTURE_FIT:
        case GRAPE_SURFACE_TEXTURE_COVER: {
            const float scale_x = surface_width / texture_width;
            const float scale_y = surface_height / texture_height;
            const float image_scale = surface->texture_mode == GRAPE_SURFACE_TEXTURE_FIT
                ? fminf(scale_x, scale_y)
                : fmaxf(scale_x, scale_y);
            const float image_width = texture_width * image_scale;
            const float image_height = texture_height * image_scale;
            const float left = (surface_width - image_width) * 0.5f;
            const float top = (surface_height - image_height) * 0.5f;
            const float inverse_scale = 1.0f / image_scale;

            surface->texture_from_local_x_scale = inverse_scale;
            surface->texture_from_local_y_scale = inverse_scale;
            surface->texture_from_local_x_offset = -left * inverse_scale;
            surface->texture_from_local_y_offset = -top * inverse_scale;
            if (surface->texture_mode == GRAPE_SURFACE_TEXTURE_FIT) {
                surface->texture_local_left = left;
                surface->texture_local_top = top;
                surface->texture_local_right = left + image_width;
                surface->texture_local_bottom = top + image_height;
            }
            break;
        }

        case GRAPE_SURFACE_TEXTURE_CENTER: {
            const float left = (surface_width - texture_width) * 0.5f;
            const float top = (surface_height - texture_height) * 0.5f;
            surface->texture_from_local_x_scale = 1.0f;
            surface->texture_from_local_y_scale = 1.0f;
            surface->texture_from_local_x_offset = -left;
            surface->texture_from_local_y_offset = -top;
            surface->texture_local_left = left;
            surface->texture_local_top = top;
            surface->texture_local_right = left + texture_width;
            surface->texture_local_bottom = top + texture_height;
            break;
        }

        default:
            break;
    }

    surface->texture_mapping_identity =
        surface->width == surface->texture->width &&
        surface->height == surface->texture->height &&
        (surface->texture_mode == GRAPE_SURFACE_TEXTURE_STRETCH ||
         surface->texture_mode == GRAPE_SURFACE_TEXTURE_TILE ||
         surface->texture_mode == GRAPE_SURFACE_TEXTURE_FIT ||
         surface->texture_mode == GRAPE_SURFACE_TEXTURE_COVER ||
         surface->texture_mode == GRAPE_SURFACE_TEXTURE_CENTER);
}

bool grape_surface_map_texture_point(const grape_surface_t *surface,
                                     float local_x,
                                     float local_y,
                                     int32_t *out_x,
                                     int32_t *out_y)
{
    if (!surface || !surface->texture || !out_x || !out_y ||
        local_x < 0.0f || local_y < 0.0f ||
        local_x >= (float)surface->width || local_y >= (float)surface->height) {
        return false;
    }

    if (surface->texture_mapping_repeat) {
        int32_t x = (int32_t)local_x;
        int32_t y = (int32_t)local_y;
        *out_x = x % (int32_t)surface->texture->width;
        *out_y = y % (int32_t)surface->texture->height;
        return true;
    }

    if (local_x < surface->texture_local_left ||
        local_y < surface->texture_local_top ||
        local_x >= surface->texture_local_right ||
        local_y >= surface->texture_local_bottom) {
        return false;
    }

    float texture_x = local_x * surface->texture_from_local_x_scale +
                      surface->texture_from_local_x_offset;
    float texture_y = local_y * surface->texture_from_local_y_scale +
                      surface->texture_from_local_y_offset;
    if (texture_x < 0.0f || texture_y < 0.0f ||
        texture_x >= (float)surface->texture->width ||
        texture_y >= (float)surface->texture->height) {
        return false;
    }

    *out_x = (int32_t)texture_x;
    *out_y = (int32_t)texture_y;
    return true;
}

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
    if (!surface || surface->width == 0U || surface->height == 0U || !surface->visible) {
        return (grape_rect_t){0, 0, 0, 0};
    }

    float width = (float)surface->width;
    float height = (float)surface->height;
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

    /*
     * Coverage AA can shade a pixel whose centre is just outside the exact
     * transformed quad because one of its subpixel samples is still inside.
     * Pad the candidate bounds by half a screen pixel so those edge pixels
     * reach the coverage test. The rasterizer still rejects zero-coverage
     * pixels, so this is conservative rather than changing the primitive.
     */
    const float aa_pad = surface->aa == GRAPE_SURFACE_AA_COVERAGE_4X
        ? 0.5f
        : 0.0f;
    int32_t x0 = (int32_t)floorf(min_x - aa_pad);
    int32_t y0 = (int32_t)floorf(min_y - aa_pad);
    int32_t x1 = (int32_t)ceilf(max_x + aa_pad);
    int32_t y1 = (int32_t)ceilf(max_y + aa_pad);

    return (grape_rect_t){x0, y0, x1 - x0, y1 - y0};
}

/**
 * Recalculates derived/cached values that we need from a surface's
 * transform, so the renderer doesn't re-do expensive transform math every pixel
 *
 * @param surface GRAPE surface
 */
static void surface_recache_transform(grape_surface_t *surface)
{
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

    /*
     * D3D-style rotated-grid 4x sample pattern, expressed relative to the
     * pixel centre. Convert the fixed screen-space offsets to local-space once
     * here so boundary coverage never needs matrix math inside the hot loop.
     */
    static const float sample_dx[4] = {-0.125f, 0.375f, -0.375f, 0.125f};
    static const float sample_dy[4] = {-0.375f, -0.125f, 0.125f, 0.375f};
    for (size_t i = 0U; i < 4U; ++i) {
        surface->aa_local_dx[i] =
            surface->local_x_from_screen_x * sample_dx[i] +
            surface->local_x_from_screen_y * sample_dy[i];
        surface->aa_local_dy[i] =
            surface->local_y_from_screen_x * sample_dx[i] +
            surface->local_y_from_screen_y * sample_dy[i];
    }

    surface->aa_local_min_dx = surface->aa_local_dx[0];
    surface->aa_local_max_dx = surface->aa_local_dx[0];
    surface->aa_local_min_dy = surface->aa_local_dy[0];
    surface->aa_local_max_dy = surface->aa_local_dy[0];
    for (size_t i = 1U; i < 4U; ++i) {
        surface->aa_local_min_dx = fminf(surface->aa_local_min_dx, surface->aa_local_dx[i]);
        surface->aa_local_max_dx = fmaxf(surface->aa_local_max_dx, surface->aa_local_dx[i]);
        surface->aa_local_min_dy = fminf(surface->aa_local_min_dy, surface->aa_local_dy[i]);
        surface->aa_local_max_dy = fmaxf(surface->aa_local_max_dy, surface->aa_local_dy[i]);
    }

    surface->bounds = grape_surface_calculate_bounds(surface);
}

void grape_surface_recache(grape_surface_t *surface)
{
    GRAPE_TIME_SCOPE(SURFACE_RECACHE);
    surface_recache_transform(surface);
    surface_recache_texture_mapping(surface);
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
 * Creates a GRAPE surface from one descriptor.
 *
 * Width/height may be zero when a texture is supplied; zero dimensions then
 * resolve to the texture's native dimensions. The resolved surface size is
 * independent from the texture afterwards.
 */
static bool surface_transform_finite(const grape_transform_t *t)
{
    return isfinite(t->x) && isfinite(t->y) && isfinite(t->scale_x) &&
        isfinite(t->scale_y) && isfinite(t->rotation) &&
        isfinite(t->origin_x) && isfinite(t->origin_y);
}

esp_err_t grape_surface_create(grape_context_t *context,
                               const grape_surface_desc_t *desc,
                               grape_surface_t **out_surface)
{
    if (!context || !desc || !out_surface ||
        !surface_texture_mode_valid(desc->texture_mode) ||
        !surface_texture_filter_valid(desc->texture_filter) ||
        !surface_aa_valid(desc->aa) ||
        (desc->texture && desc->texture->context != context) ||
        (desc->shader_count > 0U && !desc->shaders) ||
        !surface_transform_finite(&desc->transform) ||
        fabsf(desc->transform.scale_x) < FLT_EPSILON ||
        fabsf(desc->transform.scale_y) < FLT_EPSILON) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t width = desc->width;
    uint32_t height = desc->height;
    if (width == 0U && desc->texture) {
        width = desc->texture->width;
    }
    if (height == 0U && desc->texture) {
        height = desc->texture->height;
    }
    if (width == 0U || height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_surface_shader_instance_t *shaders = NULL;
    esp_err_t ret = surface_copy_shaders(desc->shaders, desc->shader_count, &shaders);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!desc->texture && desc->shader_count == 0U) {
        surface_free_shaders(shaders, desc->shader_count);
        return ESP_ERR_INVALID_ARG;
    }
    if (!desc->texture &&
        !surface_shader_chain_can_start_without_texture(shaders, desc->shader_count)) {
        surface_free_shaders(shaders, desc->shader_count);
        return ESP_ERR_INVALID_ARG;
    }

    grape_surface_t *surface = calloc(1, sizeof(*surface));
    if (!surface) {
        surface_free_shaders(shaders, desc->shader_count);
        return ESP_ERR_NO_MEM;
    }

    surface->context = context;
    surface->texture = desc->texture;
    surface->shaders = shaders;
    surface->shader_count = desc->shader_count;
    surface->width = width;
    surface->height = height;
    surface->texture_mode = desc->texture_mode;
    surface->texture_filter = desc->texture_filter;
    surface->aa = desc->aa;
    surface->transform = desc->transform;
    surface->tint = desc->tint;
    surface->opacity = desc->opacity;
    surface->visible = desc->visible;
    surface->z = desc->z;
    if (surface->texture) {
        surface->texture->ref_count++;
    }

    grape_surface_recache(surface);
    grape_surface_insert_sorted(context, surface);

    ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        grape_surface_remove(context, surface);
        if (surface->texture && surface->texture->ref_count) {
            surface->texture->ref_count--;
        }
        surface_free_shaders(surface->shaders, surface->shader_count);
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
    surface_free_shaders(surface->shaders, surface->shader_count);
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
    if (!surface || (texture && texture->context != surface->context) ||
        (!texture && !surface_shader_chain_can_start_without_texture(surface->shaders, surface->shader_count))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->texture == texture) {
        return ESP_OK;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_texture_t *old_texture = surface->texture;
    if (texture) {
        texture->ref_count++;
    }
    surface->texture = texture;
    grape_surface_recache(surface);

    ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        surface->texture = old_texture;
        grape_surface_recache(surface);
        if (texture && texture->ref_count) {
            texture->ref_count--;
        }
        (void)mark_surface_coverage(surface);
        return ret;
    }

    if (old_texture && old_texture->ref_count) {
        old_texture->ref_count--;
    }
    return ESP_OK;
}

esp_err_t grape_surface_set_size(grape_surface_t *surface, uint32_t width, uint32_t height)
{
    if (!surface || width == 0U || height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->width == width && surface->height == height) {
        return ESP_OK;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t old_width = surface->width;
    uint32_t old_height = surface->height;
    surface->width = width;
    surface->height = height;
    grape_surface_recache(surface);

    ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        surface->width = old_width;
        surface->height = old_height;
        grape_surface_recache(surface);
        (void)mark_surface_coverage(surface);
    }
    return ret;
}

esp_err_t grape_surface_set_texture_mode(grape_surface_t *surface, grape_surface_texture_mode_t mode)
{
    if (!surface || !surface_texture_mode_valid(mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->texture_mode == mode) {
        return ESP_OK;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_surface_texture_mode_t old_mode = surface->texture_mode;
    surface->texture_mode = mode;
    grape_surface_recache(surface);
    ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        surface->texture_mode = old_mode;
        grape_surface_recache(surface);
        (void)mark_surface_coverage(surface);
    }
    return ret;
}

esp_err_t grape_surface_set_texture_filter(grape_surface_t *surface, grape_texture_filter_t filter)
{
    if (!surface || !surface_texture_filter_valid(filter)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->texture_filter == filter) {
        return ESP_OK;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }
    surface->texture_filter = filter;
    return mark_surface_coverage(surface);
}

esp_err_t grape_surface_set_aa(grape_surface_t *surface, grape_surface_aa_t aa)
{
    if (!surface || !surface_aa_valid(aa)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (surface->aa == aa) {
        return ESP_OK;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_surface_aa_t old_aa = surface->aa;
    surface->aa = aa;
    grape_surface_recache(surface);
    ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        surface->aa = old_aa;
        grape_surface_recache(surface);
        (void)mark_surface_coverage(surface);
    }
    return ret;
}

esp_err_t grape_surface_set_shaders(grape_surface_t *surface,
                                    const grape_surface_shader_desc_t *shaders,
                                    size_t shader_count)
{
    if (!surface || (shader_count > 0U && !shaders)) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_surface_shader_instance_t *new_shaders = NULL;
    esp_err_t ret = surface_copy_shaders(shaders, shader_count, &new_shaders);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!surface->texture && shader_count == 0U) {
        surface_free_shaders(new_shaders, shader_count);
        return ESP_ERR_INVALID_ARG;
    }
    if (!surface->texture &&
        !surface_shader_chain_can_start_without_texture(new_shaders, shader_count)) {
        surface_free_shaders(new_shaders, shader_count);
        return ESP_ERR_INVALID_ARG;
    }

    ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        surface_free_shaders(new_shaders, shader_count);
        return ret;
    }

    grape_surface_shader_instance_t *old_shaders = surface->shaders;
    size_t old_shader_count = surface->shader_count;
    surface->shaders = new_shaders;
    surface->shader_count = shader_count;

    ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        surface->shaders = old_shaders;
        surface->shader_count = old_shader_count;
        surface_free_shaders(new_shaders, shader_count);
        (void)mark_surface_coverage(surface);
        return ret;
    }

    surface_free_shaders(old_shaders, old_shader_count);
    return ESP_OK;
}

esp_err_t grape_surface_update_shader_uniforms(grape_surface_t *surface,
                                               size_t shader_index,
                                               const void *uniforms)
{
    if (!surface || shader_index >= surface->shader_count) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_surface_shader_instance_t *shader = &surface->shaders[shader_index];
    if (shader->program->uniform_size == 0U) {
        return ESP_OK;
    }
    if (!uniforms || !shader->uniforms) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret != ESP_OK) {
        return ret;
    }
    memcpy(shader->uniforms, uniforms, shader->program->uniform_size);
    return ESP_OK;
}

/* Translation preserves the linear transform, AA offsets and texture mapping.
 * Recompute offsets from absolute coordinates to avoid cumulative rounding drift.
 * Bounds use the original corner calculation, including its AA padding. */
static void surface_recache_translation(grape_surface_t *surface)
{
    GRAPE_TIME_SCOPE(SURFACE_RECACHE);
    surface->local_x_offset = surface->transform.origin_x
                       - surface->local_x_from_screen_x * surface->transform.x
                       - surface->local_x_from_screen_y * surface->transform.y;
    surface->local_y_offset = surface->transform.origin_y
                       - surface->local_y_from_screen_x * surface->transform.x
                       - surface->local_y_from_screen_y * surface->transform.y;
    surface->bounds = grape_surface_calculate_bounds(surface);
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
    if (!surface || !transform || !surface_transform_finite(transform) || fabsf(transform->scale_x) < FLT_EPSILON || fabsf(transform->scale_y) < FLT_EPSILON) {
        return ESP_ERR_INVALID_ARG;
    }

    const grape_transform_t *old = &surface->transform;
    if (old->x == transform->x && old->y == transform->y &&
        old->scale_x == transform->scale_x && old->scale_y == transform->scale_y &&
        old->rotation == transform->rotation &&
        old->origin_x == transform->origin_x && old->origin_y == transform->origin_y) {
        return ESP_OK;
    }
    /* Bit equality also preserves signed-zero behavior in cached coefficients. */
    const bool translation_only =
        memcmp(&old->rotation, &transform->rotation, sizeof(float)) == 0 &&
        memcmp(&old->scale_x, &transform->scale_x, sizeof(float)) == 0 &&
        memcmp(&old->scale_y, &transform->scale_y, sizeof(float)) == 0 &&
        memcmp(&old->origin_x, &transform->origin_x, sizeof(float)) == 0 &&
        memcmp(&old->origin_y, &transform->origin_y, sizeof(float)) == 0;
    GRAPE_TIME_SCOPE(SURFACE_TRANSFORM);

    esp_err_t ret = mark_surface_coverage(surface);
    if (ret == ESP_OK) {
        surface->transform = *transform;
        if (translation_only) surface_recache_translation(surface);
        else {
            GRAPE_TIME_SCOPE(SURFACE_RECACHE);
            surface_recache_transform(surface);
        }
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

uint32_t grape_surface_width(const grape_surface_t *surface)
{
    return surface ? surface->width : 0U;
}

uint32_t grape_surface_height(const grape_surface_t *surface)
{
    return surface ? surface->height : 0U;
}

grape_surface_texture_mode_t grape_surface_texture_mode(const grape_surface_t *surface)
{
    return surface ? surface->texture_mode : GRAPE_SURFACE_TEXTURE_STRETCH;
}

grape_texture_filter_t grape_surface_texture_filter(const grape_surface_t *surface)
{
    return surface ? surface->texture_filter : GRAPE_TEXTURE_FILTER_NEAREST;
}

grape_surface_aa_t grape_surface_aa(const grape_surface_t *surface)
{
    return surface ? surface->aa : GRAPE_SURFACE_AA_NONE;
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

size_t grape_surface_shader_count(const grape_surface_t *surface)
{
    return surface ? surface->shader_count : 0U;
}

const grape_shader_program_t *grape_surface_shader(const grape_surface_t *surface, size_t shader_index)
{
    return surface && shader_index < surface->shader_count
        ? surface->shaders[shader_index].program
        : NULL;
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
