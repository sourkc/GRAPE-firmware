#pragma once

#include <stddef.h>

#include "grape/grape_texture.h"
#include "grape/grape_shader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_surface grape_surface_t;

/** Controls how a texture is mapped into the independent surface rectangle. */
typedef enum {
    GRAPE_SURFACE_TEXTURE_STRETCH = 0, ///< Scale the full texture to the full surface; aspect ratio may change.
    GRAPE_SURFACE_TEXTURE_TILE,        ///< Repeat the texture at native texel size across the surface.
    GRAPE_SURFACE_TEXTURE_FIT,         ///< Preserve aspect ratio and contain the full texture, centered.
    GRAPE_SURFACE_TEXTURE_COVER,       ///< Preserve aspect ratio and fill the surface, centered and cropped.
    GRAPE_SURFACE_TEXTURE_CENTER,      ///< Keep native texture size and center it; crop/leave empty space as needed.
} grape_surface_texture_mode_t;

/** Texture reconstruction filter used when sampling a surface texture. */
typedef enum {
    GRAPE_TEXTURE_FILTER_NEAREST = 0, ///< Fast nearest-neighbour sampling.
    GRAPE_TEXTURE_FILTER_LINEAR,      ///< Bilinear interpolation of four neighbouring texels.
} grape_texture_filter_t;

/** Geometry anti-aliasing mode for the transformed surface rectangle. */
typedef enum {
    GRAPE_SURFACE_AA_NONE = 0,       ///< Binary inside/outside surface coverage.
    GRAPE_SURFACE_AA_COVERAGE_4X,   ///< Four sub-pixel coverage samples on boundary pixels.
} grape_surface_aa_t;

/** One stage in a surface shader chain. Uniform data is copied by GRAPE. */
typedef struct {
    const grape_shader_program_t *program;
    const void *uniforms;
} grape_surface_shader_desc_t;

/**
 * Complete surface creation descriptor.
 *
 * width/height are independent from the texture. A zero dimension adopts the
 * initial texture's corresponding native dimension; a textureless surface must
 * provide both dimensions. shaders are evaluated in array order. Each stage
 * receives the previous stage's output as source_color. If there is no texture,
 * the first shader must not require source_color. Shader uv/local_position are
 * surface-space coordinates; texture_mode only changes source texture sampling.
 */
typedef struct {
    grape_texture_t *texture;
    uint32_t width;
    uint32_t height;
    grape_surface_texture_mode_t texture_mode;
    grape_texture_filter_t texture_filter;
    grape_surface_aa_t aa;
    const grape_surface_shader_desc_t *shaders;
    size_t shader_count;
    grape_transform_t transform;
    grape_color_t tint;
    int32_t z;
    uint8_t opacity;
    bool visible;
} grape_surface_desc_t;

#define GRAPE_SURFACE_DESC_DEFAULT() ((grape_surface_desc_t) { \
    .texture = NULL, \
    .width = 0U, \
    .height = 0U, \
    .texture_mode = GRAPE_SURFACE_TEXTURE_STRETCH, \
    .texture_filter = GRAPE_TEXTURE_FILTER_NEAREST, \
    .aa = GRAPE_SURFACE_AA_NONE, \
    .shaders = NULL, \
    .shader_count = 0U, \
    .transform = GRAPE_TRANSFORM_DEFAULT(), \
    .tint = {255U, 255U, 255U, 255U}, \
    .z = 0, \
    .opacity = 255U, \
    .visible = true, \
})

#define GRAPE_SURFACE_DESC_TEXTURE(texture_) ((grape_surface_desc_t) { \
    .texture = (texture_), \
    .width = 0U, \
    .height = 0U, \
    .texture_mode = GRAPE_SURFACE_TEXTURE_STRETCH, \
    .texture_filter = GRAPE_TEXTURE_FILTER_NEAREST, \
    .aa = GRAPE_SURFACE_AA_NONE, \
    .shaders = NULL, \
    .shader_count = 0U, \
    .transform = GRAPE_TRANSFORM_DEFAULT(), \
    .tint = {255U, 255U, 255U, 255U}, \
    .z = 0, \
    .opacity = 255U, \
    .visible = true, \
})

esp_err_t grape_surface_create(grape_context_t *context,
                               const grape_surface_desc_t *desc,
                               grape_surface_t **out_surface);
esp_err_t grape_surface_destroy(grape_surface_t *surface);
esp_err_t grape_surface_set_texture(grape_surface_t *surface, grape_texture_t *texture);
esp_err_t grape_surface_set_size(grape_surface_t *surface, uint32_t width, uint32_t height);
esp_err_t grape_surface_set_texture_mode(grape_surface_t *surface, grape_surface_texture_mode_t mode);
esp_err_t grape_surface_set_texture_filter(grape_surface_t *surface, grape_texture_filter_t filter);
esp_err_t grape_surface_set_aa(grape_surface_t *surface, grape_surface_aa_t aa);
esp_err_t grape_surface_set_shaders(grape_surface_t *surface,
                                    const grape_surface_shader_desc_t *shaders,
                                    size_t shader_count);
esp_err_t grape_surface_update_shader_uniforms(grape_surface_t *surface,
                                               size_t shader_index,
                                               const void *uniforms);
esp_err_t grape_surface_set_transform(grape_surface_t *surface, const grape_transform_t *transform);
esp_err_t grape_surface_set_position(grape_surface_t *surface, float x, float y);
esp_err_t grape_surface_set_scale(grape_surface_t *surface, float scale_x, float scale_y);
esp_err_t grape_surface_set_rotation(grape_surface_t *surface, float radians);
esp_err_t grape_surface_set_origin(grape_surface_t *surface, float origin_x, float origin_y);
esp_err_t grape_surface_set_z(grape_surface_t *surface, int32_t z);
esp_err_t grape_surface_set_opacity(grape_surface_t *surface, uint8_t opacity);
esp_err_t grape_surface_set_tint(grape_surface_t *surface, grape_color_t tint);
esp_err_t grape_surface_set_visible(grape_surface_t *surface, bool visible);
const grape_transform_t *grape_surface_transform(const grape_surface_t *surface);
uint32_t grape_surface_width(const grape_surface_t *surface);
uint32_t grape_surface_height(const grape_surface_t *surface);
grape_surface_texture_mode_t grape_surface_texture_mode(const grape_surface_t *surface);
grape_texture_filter_t grape_surface_texture_filter(const grape_surface_t *surface);
grape_surface_aa_t grape_surface_aa(const grape_surface_t *surface);
int32_t grape_surface_z(const grape_surface_t *surface);
uint8_t grape_surface_opacity(const grape_surface_t *surface);
grape_color_t grape_surface_tint(const grape_surface_t *surface);
bool grape_surface_visible(const grape_surface_t *surface);
size_t grape_surface_shader_count(const grape_surface_t *surface);
const grape_shader_program_t *grape_surface_shader(const grape_surface_t *surface, size_t shader_index);
grape_texture_t *grape_surface_texture(grape_surface_t *surface);

#ifdef __cplusplus
}
#endif
