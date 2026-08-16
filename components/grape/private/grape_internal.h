#pragma once

#include <math.h>

#include "sdkconfig.h"
#include "driver/ppa.h"
#include "grape/grape.h"
#include "grape/grape_telemetry.h"

// See /docs/PERFORMANCE.md#avoid-int64_t-floorf-in-hot-paths
static inline int32_t grape_floor_to_i32(float value)
{
    int32_t truncated = (int32_t)value;
    return truncated - ((float)truncated > value);
}

typedef enum {
#define GRAPE_FEATURE_ENTRY(symbol, id, name, default_mode, flags) \
    GRAPE_FEATURE_SLOT_##symbol,
#include "grape/grape_feature_registry.def"
#undef GRAPE_FEATURE_ENTRY
    GRAPE_FEATURE_SLOT_COUNT,
} grape_feature_slot_t;

/**
 * Feature state struct
 */
typedef struct {
    grape_feature_mode_t mode; ///< Feature mode (0-2)
    grape_feature_unavailable_reason_t unavailable_reason; ///< Reason for why the feature is unavailable (if it is)
    bool available; ///< If the feature is available
    bool active; ///< If the feature is turned on
} grape_feature_state_t;

/**
 * GRAPE texture
 */
struct grape_texture {
    grape_context_t *context;
    struct grape_texture *next;
    uint8_t *pixels;
    size_t stride;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t ref_count; ///< Amount of surfaces using that texture
    grape_pixel_format_t format;
    grape_memory_t memory; ///< Memory type for memory allocation
    uint8_t *occupancy; ///< Occupancy bitmap (See /docs/ARCHITECTURE.md#occupancy-map)
    size_t occupancy_bitmap_size;
    uint32_t occupancy_columns; ///< Amount of columns of in the occupancy bitmap
    uint32_t occupancy_rows; ///< Amount of rows in the occupancy bitmap
    size_t occupancy_occupied_count; ///< Amount of occupied cells in the occupancy bitmap
    bool occupancy_all_full; ///< If the occupancy is all full for a texture. Always true for opaque textures
    bool occupancy_all_empty; ///< If the occupancy is all empty for a texture (aka an empty transparent texture)
};

/**
 * GRAPE surface
 */
struct grape_surface {
    grape_context_t *context;
    struct grape_surface *prev; ///< Pointer to the previous surface in the Z-ordered surface list
    struct grape_surface *next; ///< Pointer to the next surface in the Z-ordered surface list
    grape_texture_t *texture;
    const grape_shader_program_t *shader;
    uint32_t width;
    uint32_t height;
    void *shader_uniforms;
    grape_transform_t transform;
    grape_rect_t bounds;
    grape_color_t tint;
    float cos_rotation; ///< Cached cos of rotation
    float sin_rotation; ///< Cached sin of rotation
    float local_x_from_screen_x; ///< How much the x changes in local texture coordinates if we move by one x
    float local_x_from_screen_y; ///< How much the x changes in local texture coordinates if we move by one y
    float local_x_offset; ///< Constant term of the cached screen-to-local X transform
    float local_y_from_screen_x; ///< How much the y changes in local texture coordinates if we move by one x
    float local_y_from_screen_y; ///< How much the y changes in local texture coordinates if we move by one y
    float local_y_offset; ///< Constant term of the cached screen-to-local Y transform
    float normalized_rotation; ///< Rotation normalized to [-pi, pi]
    float shear_x_coefficient; ///< Used for three-shear rotation. Equivalent to -tan(normalized_rotation / 2)
    int32_t z;
    uint8_t opacity;
    bool visible;
    bool shear_cache_valid;
};

/**
 * A non-owning description of an A8 image generated/used
 * by the three-shear rotation pipeline, including its
 * pixel buffer dimensions and its offset relative to
 * the surface position
 *
 * NOTE: I forgot why we need this
 */
typedef struct {
    const uint8_t *pixels;
    size_t stride;
    float left;
    float top;
    uint32_t width;
    uint32_t height;
} grape_shear_image_t;

/**
 * Simple rect measured in damage tiles instead of pixels
 */
typedef struct {
    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
} grape_damage_tile_region_t;

/**
 * Stores the bounds of a damaged tile region together with
 * the best split currently found for it.
 */
typedef struct {
    grape_damage_tile_region_t bounds; ///< Region currently being considered for splitting
    grape_damage_tile_region_t split_a; ///< First optimal split region
    grape_damage_tile_region_t split_b; ///< Second optimal split region
    int64_t split_saving; ///< Measures how much cheaper the split is
} grape_damage_split_region_t;

/**
 * Complicated thing T_T
 *
 * Maintains bitmaps describing damaged and visible tiles across frames and
 * combines them to determine which parts of the display needs to be redrawn.
 * Also owns the temporary workspace used by the damage rectangle planner to
 * group and split damaged tile regions into efficient render rectangles.
 *
 * Previous frame visibility is retained so damage can be propagated between
 * display backbuffers when necessary.
 */
typedef struct {
    uint8_t *tiles; ///< Tiles directly marked as damaged
    uint8_t *current_visible_tiles; ///< Damage/visibility required for the current frame
    uint8_t *previous_visible_tiles; ///< Damage/visibility required for that was required for the previous frame
    uint8_t *render_tiles;
    size_t bitmap_size;
    uint32_t tile_columns;
    uint32_t tile_rows;
    bool has_damage;
    grape_rect_t final_rects[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t final_rect_count;
    grape_damage_split_region_t *split_regions;
    grape_damage_tile_region_t *column_bounds;
    grape_damage_tile_region_t *row_bounds;
    grape_damage_tile_region_t *suffix_bounds;
    size_t split_region_capacity;
    size_t split_axis_capacity;
    uint64_t mark_timer_start_us;
    grape_debug_damage_stats_t latest_stats;
} grape_damage_state_t;

#define GRAPE_DEBUG_RENDER_DAMAGE_CAPACITY (CONFIG_GRAPE_MAX_DAMAGE_RECTS * 2U)

/**
 * State struct for debug layers
 */
typedef struct {
    uint32_t enabled_mask;
    grape_rect_t render_damage[GRAPE_DEBUG_RENDER_DAMAGE_CAPACITY];
    size_t render_damage_count;
    grape_rect_t damage_rects_current[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t damage_rects_current_count;
    grape_rect_t damage_rects_previous[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t damage_rects_previous_count;
} grape_debug_state_t;

/**
 * Internal state associated with a GRAPE context.
 *
 * Contains the display and render target, lists of textures and surfaces,
 * damage tracking state, hardware accelerator handles, reusable rotation
 * buffers, feature configuration and debug state used by the renderer.
 */
struct grape_context {
    grape_display_t *display;
    grape_display_info_t display_info;
    grape_texture_t *textures;
    grape_surface_t *surfaces;
    grape_color_t background;
    grape_damage_state_t damage;
    grape_display_render_target_t render_target;
    ppa_client_handle_t ppa_srm;
    ppa_client_handle_t ppa_blend;
    ppa_client_handle_t ppa_fill;
    uint8_t *shear_buffer_a;
    uint8_t *shear_buffer_b;
    size_t shear_buffer_a_size;
    size_t shear_buffer_b_size;
    grape_rotation_backend_t rotation_backend;
    grape_feature_state_t features[GRAPE_FEATURE_SLOT_COUNT];
    grape_debug_state_t debug;
    bool display_backbuffer_needs_full_redraw;
};

size_t grape_bytes_per_pixel(grape_pixel_format_t format);
bool grape_rect_empty(grape_rect_t rect);
grape_rect_t grape_rect_intersection(grape_rect_t a, grape_rect_t b);
grape_rect_t grape_rect_union(grape_rect_t a, grape_rect_t b);
bool grape_rect_touches(grape_rect_t a, grape_rect_t b);
esp_err_t grape_damage_init(grape_context_t *context);
void grape_damage_deinit(grape_context_t *context);
esp_err_t grape_damage_add(grape_context_t *context, grape_rect_t rect);
esp_err_t grape_damage_add_surface_coverage(grape_surface_t *surface);
void grape_damage_all(grape_context_t *context);
void grape_damage_clear(grape_context_t *context);
esp_err_t grape_damage_build_logical_rects(grape_context_t *context);
esp_err_t grape_damage_prepare_visible(grape_context_t *context,
                                       const grape_rect_t *extra_rects,
                                       size_t extra_count);
esp_err_t grape_damage_build_render_rects(grape_context_t *context,
                                          bool force_full_redraw,
                                          grape_rect_t *out_rects,
                                          size_t out_capacity,
                                          size_t *out_count);
void grape_damage_commit_visible(grape_context_t *context);
grape_rect_t grape_surface_calculate_bounds(const grape_surface_t *surface);
void grape_surface_recache(grape_surface_t *surface);
void grape_surface_insert_sorted(grape_context_t *context, grape_surface_t *surface);
void grape_surface_remove(grape_context_t *context, grape_surface_t *surface);
esp_err_t grape_texture_rebuild_occupancy(grape_texture_t *texture);

void grape_feature_init(grape_context_t *context);
esp_err_t grape_feature_set_availability(grape_context_t *context,
                                         grape_feature_id_t id,
                                         bool available,
                                         grape_feature_unavailable_reason_t reason);

void grape_ppa_init(grape_context_t *context);
void grape_ppa_deinit(grape_context_t *context);
esp_err_t grape_ppa_fill(grape_context_t *context, grape_rect_t rect, grape_color_t color);
esp_err_t grape_ppa_blend_surface(grape_context_t *context, const grape_surface_t *surface,
                                  grape_rect_t damage_rect, bool *handled);
esp_err_t grape_ppa_blend_a8_image(grape_context_t *context,
                                   const uint8_t *pixels,
                                   uint32_t width,
                                   uint32_t height,
                                   float screen_left,
                                   float screen_top,
                                   grape_rect_t damage_rect,
                                   grape_color_t tint,
                                   uint8_t opacity,
                                   bool *handled);
esp_err_t grape_ppa_rotate_a8(grape_context_t *context,
                              const grape_shear_image_t *input_image,
                              bool clockwise,
                              uint8_t *output_buffer,
                              size_t output_buffer_size,
                              grape_shear_image_t *out_image);

esp_err_t grape_shear_rotate_a8(grape_context_t *context, const grape_surface_t *surface,
                                grape_shear_image_t *out_image);

esp_err_t grape_compositor_render(grape_context_t *context, grape_rect_t rect);

esp_err_t grape_debug_prepare_frame(grape_context_t *context);
void grape_debug_render(grape_context_t *context, grape_rect_t rect);
void grape_debug_finish_frame(grape_context_t *context);
void grape_debug_reset_frame(grape_context_t *context);
