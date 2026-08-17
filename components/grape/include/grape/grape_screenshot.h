#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_context grape_context_t;

typedef enum {
    GRAPE_SCREENSHOT_FORMAT_PNG = 0,
    GRAPE_SCREENSHOT_FORMAT_JPEG,
} grape_screenshot_format_t;

typedef enum {
    GRAPE_SCREENSHOT_JPEG_YUV444 = 0,
    GRAPE_SCREENSHOT_JPEG_YUV422,
    GRAPE_SCREENSHOT_JPEG_YUV420,
} grape_screenshot_jpeg_subsampling_t;

typedef struct {
    grape_screenshot_format_t format;

    /* PNG compression level, 0 (fastest/largest) through 9 (smallest/slowest). */
    int png_compression_level;

    /* JPEG quality, 1 through 100. JPEG remains lossy at 100. */
    uint8_t jpeg_quality;
    grape_screenshot_jpeg_subsampling_t jpeg_subsampling;
} grape_screenshot_desc_t;

#define GRAPE_SCREENSHOT_DESC_DEFAULT()                 \
    {                                                   \
        .format = GRAPE_SCREENSHOT_FORMAT_PNG,          \
        .png_compression_level = 3,                     \
        .jpeg_quality = 95U,                            \
        .jpeg_subsampling = GRAPE_SCREENSHOT_JPEG_YUV444, \
    }

typedef struct {
    uint8_t *data;
    size_t size;
    uint32_t width;
    uint32_t height;
    grape_screenshot_format_t format;
} grape_screenshot_t;

/**
 * Captures the framebuffer that is currently presented by GRAPE and encodes
 * it into PNG or JPEG. The call is synchronous and the returned bytes are
 * owned by the caller until grape_screenshot_release() is called.
 *
 * PNG is lossless relative to the presented framebuffer. RGB565 displays are
 * expanded to RGB888 in a reversible way before PNG encoding.
 */
esp_err_t grape_screenshot_capture(grape_context_t *context,
                                   const grape_screenshot_desc_t *desc,
                                   grape_screenshot_t *out_screenshot);

/** Saves a screenshot directly to a file path using the same encoder path. */
esp_err_t grape_screenshot_save(grape_context_t *context,
                                const grape_screenshot_desc_t *desc,
                                const char *path);

/** Releases bytes returned by grape_screenshot_capture(). */
void grape_screenshot_release(grape_screenshot_t *screenshot);

#ifdef __cplusplus
}
#endif
