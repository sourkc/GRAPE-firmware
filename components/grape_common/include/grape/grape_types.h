#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    GRAPE_PIXEL_FORMAT_RGB565 = 0,
    GRAPE_PIXEL_FORMAT_RGB888,
    GRAPE_PIXEL_FORMAT_A8,
    GRAPE_PIXEL_FORMAT_RGBA8888, ///< Byte order R, G, B, A; straight (non-premultiplied) alpha.
} grape_pixel_format_t;

typedef enum {
    GRAPE_MEMORY_DEFAULT = 0,
    GRAPE_MEMORY_INTERNAL,
    GRAPE_MEMORY_PSRAM,
} grape_memory_t;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} grape_rect_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} grape_color_t;

typedef struct {
    float x;
    float y;
    float scale_x;
    float scale_y;
    float rotation;
    float origin_x;
    float origin_y;
} grape_transform_t;

#define GRAPE_TRANSFORM_DEFAULT()  \
    {                              \
        .x = 0.0f,                 \
        .y = 0.0f,                 \
        .scale_x = 1.0f,           \
        .scale_y = 1.0f,           \
        .rotation = 0.0f,          \
        .origin_x = 0.0f,          \
        .origin_y = 0.0f,          \
    }
