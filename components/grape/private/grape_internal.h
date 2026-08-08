#pragma once

#include <math.h>

#include "sdkconfig.h"
#include "driver/ppa.h"
#include "grape/grape.h"
#include "grape/grape_profile.h"

struct grape_texture {
    grape_context_t *context;
    struct grape_texture *next;
    uint8_t *pixels;
    size_t stride;
    size_t size;
    uint32_t width;
    uint32_t height;
    uint32_t ref_count;
    grape_pixel_format_t format;
    grape_memory_t memory;
};

struct grape_surface {
    grape_context_t *context;
    struct grape_surface *prev;
    struct grape_surface *next;
    grape_texture_t *texture;
    grape_transform_t transform;
    grape_rect_t bounds;
    grape_color_t tint;
    float cos_rotation;
    float sin_rotation;
    float local_x_from_screen_x;
    float local_x_from_screen_y;
    float local_x_offset;
    float local_y_from_screen_x;
    float local_y_from_screen_y;
    float local_y_offset;
    float normalized_rotation;
    float shear_x_coefficient;
    int32_t z;
    uint8_t opacity;
    bool visible;
    bool shear_cache_valid;
};

typedef struct {
    const uint8_t *pixels;
    size_t stride;
    float left;
    float top;
    uint32_t width;
    uint32_t height;
} grape_shear_image_t;

typedef struct {
    uint8_t *tiles;
    uint8_t *render_tiles;
    size_t bitmap_size;
    uint32_t tile_columns;
    uint32_t tile_rows;
    bool has_damage;
    grape_rect_t final_rects[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t final_rect_count;
    grape_rect_t *work_rects;
    size_t work_rect_capacity;
    size_t *active_runs;
    size_t *next_active_runs;
    size_t active_run_capacity;
} grape_damage_state_t;

typedef struct {
    uint32_t enabled_mask;
    grape_rect_t render_damage[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t render_damage_count;
    grape_rect_t damage_rects_current[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t damage_rects_current_count;
    grape_rect_t damage_rects_previous[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t damage_rects_previous_count;
} grape_debug_state_t;

struct grape_context {
    grape_display_t *display;
    grape_display_info_t display_info;
    grape_texture_t *textures;
    grape_surface_t *surfaces;
    grape_color_t background;
    grape_damage_state_t damage;
    uint8_t *scratch;
    size_t scratch_size;
    ppa_client_handle_t ppa_srm;
    ppa_client_handle_t ppa_blend;
    ppa_client_handle_t ppa_fill;
    uint8_t *shear_buffer_a;
    uint8_t *shear_buffer_b;
    size_t shear_buffer_a_size;
    size_t shear_buffer_b_size;
    grape_rotation_backend_t rotation_backend;
    grape_shear_y_backend_t shear_y_backend;
    grape_debug_state_t debug;
};

size_t grape_bytes_per_pixel(grape_pixel_format_t format);
bool grape_rect_empty(grape_rect_t rect);
grape_rect_t grape_rect_intersection(grape_rect_t a, grape_rect_t b);
grape_rect_t grape_rect_union(grape_rect_t a, grape_rect_t b);
bool grape_rect_touches(grape_rect_t a, grape_rect_t b);
esp_err_t grape_damage_init(grape_context_t *context);
void grape_damage_deinit(grape_context_t *context);
esp_err_t grape_damage_add(grape_context_t *context, grape_rect_t rect);
void grape_damage_all(grape_context_t *context);
void grape_damage_clear(grape_context_t *context);
esp_err_t grape_damage_build_logical_rects(grape_context_t *context);
esp_err_t grape_damage_build_render_rects(grape_context_t *context,
                                          const grape_rect_t *extra_rects,
                                          size_t extra_count,
                                          grape_rect_t *out_rects,
                                          size_t out_capacity,
                                          size_t *out_count);
grape_rect_t grape_surface_calculate_bounds(const grape_surface_t *surface);
void grape_surface_recache(grape_surface_t *surface);
void grape_surface_insert_sorted(grape_context_t *context, grape_surface_t *surface);
void grape_surface_remove(grape_context_t *context, grape_surface_t *surface);

esp_err_t grape_ppa_init(grape_context_t *context);
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
