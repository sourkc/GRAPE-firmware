#pragma once

#include <math.h>

#include "sdkconfig.h"
#include "grape/grape.h"

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
    int32_t z;
    uint8_t opacity;
    bool visible;
};

struct grape_context {
    grape_display_t *display;
    grape_display_info_t display_info;
    grape_texture_t *textures;
    grape_surface_t *surfaces;
    grape_color_t background;
    grape_rect_t damage[CONFIG_GRAPE_MAX_DAMAGE_RECTS];
    size_t damage_count;
    uint8_t *scratch;
    size_t scratch_size;
};

size_t grape_bytes_per_pixel(grape_pixel_format_t format);
bool grape_rect_empty(grape_rect_t rect);
grape_rect_t grape_rect_intersection(grape_rect_t a, grape_rect_t b);
grape_rect_t grape_rect_union(grape_rect_t a, grape_rect_t b);
bool grape_rect_touches(grape_rect_t a, grape_rect_t b);
esp_err_t grape_damage_add(grape_context_t *context, grape_rect_t rect);
void grape_damage_all(grape_context_t *context);
grape_rect_t grape_surface_calculate_bounds(const grape_surface_t *surface);
void grape_surface_recache(grape_surface_t *surface);
void grape_surface_insert_sorted(grape_context_t *context, grape_surface_t *surface);
void grape_surface_remove(grape_context_t *context, grape_surface_t *surface);
esp_err_t grape_compositor_render(grape_context_t *context, grape_rect_t rect);
