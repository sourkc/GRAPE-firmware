#include <math.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "grape_internal.h"

#define GRAPE_SHEAR_PI 3.14159265358979323846f
#define GRAPE_SHEAR_EPSILON 0.0001f

typedef struct {
    const uint8_t *pixels;
    size_t stride;
    float left;
    float top;
    uint32_t width;
    uint32_t height;
} shear_source_t;

typedef struct {
    int32_t left;
    int32_t top;
    uint32_t width;
    uint32_t height;
} shear_bounds_t;

static esp_err_t ensure_buffer(uint8_t **buffer, size_t *capacity, size_t required)
{
    if (required <= *capacity) {
        return ESP_OK;
    }

    uint8_t *replacement = heap_caps_malloc(
        required,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_8BIT
    );

    if (!replacement) {
        replacement = heap_caps_malloc(
            required,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED | MALLOC_CAP_8BIT
        );
    }

    if (!replacement) {
        return ESP_ERR_NO_MEM;
    }

    heap_caps_free(*buffer);
    *buffer = replacement;
    *capacity = required;
    return ESP_OK;
}

static esp_err_t bounds_size(const shear_bounds_t *bounds, size_t *out_size)
{
    if (!bounds || !out_size || bounds->width == 0 || bounds->height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((size_t)bounds->width > SIZE_MAX / (size_t)bounds->height) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_size = (size_t)bounds->width * (size_t)bounds->height;
    return ESP_OK;
}

static esp_err_t transformed_bounds(
    const grape_surface_t *surface,
    float m00,
    float m01,
    float m10,
    float m11,
    shear_bounds_t *out_bounds
)
{
    float left = -surface->transform.origin_x;
    float top = -surface->transform.origin_y;
    float right = (float)surface->texture->width - surface->transform.origin_x;
    float bottom = (float)surface->texture->height - surface->transform.origin_y;

    float xs[4] = {
        m00 * left + m01 * top,
        m00 * right + m01 * top,
        m00 * left + m01 * bottom,
        m00 * right + m01 * bottom,
    };

    float ys[4] = {
        m10 * left + m11 * top,
        m10 * right + m11 * top,
        m10 * left + m11 * bottom,
        m10 * right + m11 * bottom,
    };

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

    if (!isfinite(min_x) || !isfinite(max_x) ||
        !isfinite(min_y) || !isfinite(max_y)) {
        return ESP_ERR_INVALID_ARG;
    }

    float left_f = floorf(min_x);
    float top_f = floorf(min_y);
    float right_f = ceilf(max_x);
    float bottom_f = ceilf(max_y);

    if (left_f < (float)INT32_MIN || left_f > (float)INT32_MAX ||
        top_f < (float)INT32_MIN || top_f > (float)INT32_MAX ||
        right_f < (float)INT32_MIN || right_f > (float)INT32_MAX ||
        bottom_f < (float)INT32_MIN || bottom_f > (float)INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    int32_t out_left = (int32_t)left_f;
    int32_t out_top = (int32_t)top_f;
    int32_t out_right = (int32_t)right_f;
    int32_t out_bottom = (int32_t)bottom_f;

    int64_t width = (int64_t)out_right - out_left;
    int64_t height = (int64_t)out_bottom - out_top;

    if (width <= 0 || height <= 0 ||
        width > UINT32_MAX || height > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_bounds = (shear_bounds_t) {
        .left = out_left,
        .top = out_top,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
    };

    return ESP_OK;
}

static esp_err_t transformed_bounds_from_rect(
    float left,
    float top,
    float right,
    float bottom,
    float m00,
    float m01,
    float m10,
    float m11,
    shear_bounds_t *out_bounds
)
{
    float xs[4] = {
        m00 * left + m01 * top,
        m00 * right + m01 * top,
        m00 * left + m01 * bottom,
        m00 * right + m01 * bottom,
    };

    float ys[4] = {
        m10 * left + m11 * top,
        m10 * right + m11 * top,
        m10 * left + m11 * bottom,
        m10 * right + m11 * bottom,
    };

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

    if (!isfinite(min_x) || !isfinite(max_x) ||
        !isfinite(min_y) || !isfinite(max_y)) {
        return ESP_ERR_INVALID_ARG;
    }

    float left_f = floorf(min_x);
    float top_f = floorf(min_y);
    float right_f = ceilf(max_x);
    float bottom_f = ceilf(max_y);

    if (left_f < (float)INT32_MIN || left_f > (float)INT32_MAX ||
        top_f < (float)INT32_MIN || top_f > (float)INT32_MAX ||
        right_f < (float)INT32_MIN || right_f > (float)INT32_MAX ||
        bottom_f < (float)INT32_MIN || bottom_f > (float)INT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    int32_t out_left = (int32_t)left_f;
    int32_t out_top = (int32_t)top_f;
    int32_t out_right = (int32_t)right_f;
    int32_t out_bottom = (int32_t)bottom_f;

    int64_t width = (int64_t)out_right - out_left;
    int64_t height = (int64_t)out_bottom - out_top;

    if (width <= 0 || height <= 0 ||
        width > UINT32_MAX || height > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_bounds = (shear_bounds_t) {
        .left = out_left,
        .top = out_top,
        .width = (uint32_t)width,
        .height = (uint32_t)height,
    };

    return ESP_OK;
}

static esp_err_t transformed_bounds_from_source(
    const shear_source_t *source,
    float m00,
    float m01,
    float m10,
    float m11,
    shear_bounds_t *out_bounds
)
{
    if (!source || !out_bounds) {
        return ESP_ERR_INVALID_ARG;
    }

    float left = source->left;
    float top = source->top;
    float right = source->left + (float)source->width;
    float bottom = source->top + (float)source->height;

    return transformed_bounds_from_rect(
        left,
        top,
        right,
        bottom,
        m00,
        m01,
        m10,
        m11,
        out_bounds
    );
}

static grape_shear_image_t shear_source_as_image(const shear_source_t *source)
{
    return (grape_shear_image_t) {
        .pixels = source->pixels,
        .stride = source->stride,
        .left = source->left,
        .top = source->top,
        .width = source->width,
        .height = source->height,
    };
}

static void shear_x(
    const shear_source_t *input,
    float coefficient,
    const shear_bounds_t *output_bounds,
    uint8_t *output
)
{
    size_t output_stride = output_bounds->width;
    GRAPE_TIME_BLOCK(SHEAR_CLEAR) {
        memset(output, 0, output_stride * output_bounds->height);
    }

    for (uint32_t out_y = 0; out_y < output_bounds->height; ++out_y) {
        float y =
            (float)output_bounds->top +
            (float)out_y +
            0.5f;

        int64_t source_y = (int64_t)floorf(y - input->top);
        if (source_y < 0 || source_y >= input->height) {
            continue;
        }

        float first_output_x = (float)output_bounds->left + 0.5f;
        float first_source_x = first_output_x - coefficient * y;

        int64_t source_x =
            (int64_t)floorf(first_source_x - input->left);
        int64_t output_x = 0;

        if (source_x < 0) {
            output_x = -source_x;
            source_x = 0;
        }

        if (output_x >= output_bounds->width ||
            source_x >= input->width) {
            continue;
        }

        size_t available_output =
            (size_t)output_bounds->width - (size_t)output_x;
        size_t available_input =
            (size_t)input->width - (size_t)source_x;
        size_t copy_count =
            available_output < available_input
                ? available_output
                : available_input;

        const uint8_t *source =
            input->pixels +
            (size_t)source_y * input->stride +
            (size_t)source_x;

        uint8_t *destination =
            output +
            (size_t)out_y * output_stride +
            (size_t)output_x;

        memcpy(destination, source, copy_count);
    }
}

static void shear_y(
    const shear_source_t *input,
    float coefficient,
    const shear_bounds_t *output_bounds,
    uint8_t *output
)
{
    size_t output_stride = output_bounds->width;
    GRAPE_TIME_BLOCK(SHEAR_CLEAR) {
        memset(output, 0, output_stride * output_bounds->height);
    }

    for (uint32_t out_x = 0; out_x < output_bounds->width; ++out_x) {
        float x =
            (float)output_bounds->left +
            (float)out_x +
            0.5f;

        int64_t source_x = (int64_t)floorf(x - input->left);
        if (source_x < 0 || source_x >= input->width) {
            continue;
        }

        float first_output_y = (float)output_bounds->top + 0.5f;
        float first_source_y = first_output_y - coefficient * x;

        int64_t source_y =
            (int64_t)floorf(first_source_y - input->top);
        int64_t output_y = 0;

        if (source_y < 0) {
            output_y = -source_y;
            source_y = 0;
        }

        if (output_y >= output_bounds->height ||
            source_y >= input->height) {
            continue;
        }

        size_t available_output =
            (size_t)output_bounds->height - (size_t)output_y;
        size_t available_input =
            (size_t)input->height - (size_t)source_y;
        size_t copy_count =
            available_output < available_input
                ? available_output
                : available_input;

        const uint8_t *source =
            input->pixels +
            (size_t)source_y * input->stride +
            (size_t)source_x;

        uint8_t *destination =
            output +
            (size_t)output_y * output_stride +
            out_x;

        for (size_t i = 0; i < copy_count; ++i) {
            *destination = *source;
            destination += output_stride;
            source += input->stride;
        }
    }
}


static esp_err_t shear_y_ppa_layout(
    const shear_source_t *input,
    float coefficient,
    shear_bounds_t *rotated_input_bounds,
    shear_bounds_t *rotated_sheared_bounds
)
{
    if (!input || !rotated_input_bounds || !rotated_sheared_bounds) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = transformed_bounds_from_source(
        input,
        0.0f,
        1.0f,
        -1.0f,
        0.0f,
        rotated_input_bounds
    );
    if (ret != ESP_OK) {
        return ret;
    }

    shear_source_t rotated_source = {
        .pixels = NULL,
        .stride = rotated_input_bounds->width,
        .left = (float)rotated_input_bounds->left,
        .top = (float)rotated_input_bounds->top,
        .width = rotated_input_bounds->width,
        .height = rotated_input_bounds->height,
    };

    return transformed_bounds_from_source(
        &rotated_source,
        1.0f,
        -coefficient,
        0.0f,
        1.0f,
        rotated_sheared_bounds
    );
}

static esp_err_t shear_y_ppa_buffer_requirements(
    const shear_source_t *input,
    float coefficient,
    size_t *buffer_a_required,
    size_t *buffer_b_required
)
{
    if (!input || !buffer_a_required || !buffer_b_required) {
        return ESP_ERR_INVALID_ARG;
    }

    shear_bounds_t rotated_input_bounds;
    shear_bounds_t rotated_sheared_bounds;
    esp_err_t ret = shear_y_ppa_layout(
        input,
        coefficient,
        &rotated_input_bounds,
        &rotated_sheared_bounds
    );
    if (ret != ESP_OK) {
        return ret;
    }

    size_t rotated_input_size;
    ret = bounds_size(&rotated_input_bounds, &rotated_input_size);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t rotated_sheared_size;
    ret = bounds_size(&rotated_sheared_bounds, &rotated_sheared_size);
    if (ret != ESP_OK) {
        return ret;
    }

    if (rotated_sheared_size > *buffer_a_required) {
        *buffer_a_required = rotated_sheared_size;
    }
    if (rotated_input_size > *buffer_b_required) {
        *buffer_b_required = rotated_input_size;
    }

    return ESP_OK;
}

static esp_err_t shear_y_via_ppa_rotate(
    grape_context_t *context,
    const shear_source_t *input,
    float coefficient,
    grape_shear_image_t *out_image
)
{
    if (!context || !input || !input->pixels || !out_image) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!context->ppa_srm) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    shear_bounds_t rotated_input_bounds;
    shear_bounds_t rotated_sheared_bounds;
    esp_err_t ret = shear_y_ppa_layout(
        input,
        coefficient,
        &rotated_input_bounds,
        &rotated_sheared_bounds
    );
    if (ret != ESP_OK) {
        return ret;
    }

    size_t rotated_input_size;
    ret = bounds_size(&rotated_input_bounds, &rotated_input_size);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t rotated_sheared_size;
    ret = bounds_size(&rotated_sheared_bounds, &rotated_sheared_size);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!context->shear_buffer_a || context->shear_buffer_a_size < rotated_sheared_size ||
        !context->shear_buffer_b || context->shear_buffer_b_size < rotated_input_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_shear_image_t input_image = shear_source_as_image(input);
    grape_shear_image_t rotated_input_image;
    ret = grape_ppa_rotate_a8(
        context,
        &input_image,
        true,
        context->shear_buffer_b,
        context->shear_buffer_b_size,
        &rotated_input_image
    );
    if (ret != ESP_OK) {
        return ret;
    }

    GRAPE_TIME_BLOCK(SHEAR_Y) {
        shear_x(
            &(const shear_source_t) {
                .pixels = rotated_input_image.pixels,
                .stride = rotated_input_image.stride,
                .left = rotated_input_image.left,
                .top = rotated_input_image.top,
                .width = rotated_input_image.width,
                .height = rotated_input_image.height,
            },
            -coefficient,
            &rotated_sheared_bounds,
            context->shear_buffer_a
        );
    }

    grape_shear_image_t rotated_sheared_image = {
        .pixels = context->shear_buffer_a,
        .stride = rotated_sheared_bounds.width,
        .left = (float)rotated_sheared_bounds.left,
        .top = (float)rotated_sheared_bounds.top,
        .width = rotated_sheared_bounds.width,
        .height = rotated_sheared_bounds.height,
    };

    return grape_ppa_rotate_a8(
        context,
        &rotated_sheared_image,
        false,
        context->shear_buffer_b,
        context->shear_buffer_b_size,
        out_image
    );
}

static esp_err_t rotate_quarter_turn_a8_impl(
    grape_context_t *context,
    const grape_surface_t *surface,
    float angle,
    grape_shear_image_t *out_image
)
{
    float sign = angle >= 0.0f ? 1.0f : -1.0f;

    shear_bounds_t bounds;
    esp_err_t ret = transformed_bounds(
        surface,
        0.0f,
        -sign,
        sign,
        0.0f,
        &bounds
    );
    if (ret != ESP_OK) {
        return ret;
    }

    size_t required;
    ret = bounds_size(&bounds, &required);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = ensure_buffer(
        &context->shear_buffer_a,
        &context->shear_buffer_a_size,
        required
    );
    if (ret != ESP_OK) {
        return ret;
    }

    memset(context->shear_buffer_a, 0, required);

    for (uint32_t y = 0; y < bounds.height; ++y) {
        float rotated_y = (float)bounds.top + (float)y + 0.5f;
        uint8_t *destination =
            context->shear_buffer_a + (size_t)y * bounds.width;

        for (uint32_t x = 0; x < bounds.width; ++x) {
            float rotated_x = (float)bounds.left + (float)x + 0.5f;

            float local_x;
            float local_y;

            if (sign > 0.0f) {
                local_x = rotated_y + surface->transform.origin_x;
                local_y = -rotated_x + surface->transform.origin_y;
            } else {
                local_x = -rotated_y + surface->transform.origin_x;
                local_y = rotated_x + surface->transform.origin_y;
            }

            int32_t source_x = (int32_t)floorf(local_x);
            int32_t source_y = (int32_t)floorf(local_y);

            if (source_x < 0 || source_y < 0 ||
                source_x >= (int32_t)surface->texture->width ||
                source_y >= (int32_t)surface->texture->height) {
                continue;
            }

            destination[x] =
                surface->texture->pixels[
                    (size_t)source_y * surface->texture->stride +
                    (size_t)source_x
                ];
        }
    }

    *out_image = (grape_shear_image_t) {
        .pixels = context->shear_buffer_a,
        .stride = bounds.width,
        .left = (float)bounds.left,
        .top = (float)bounds.top,
        .width = bounds.width,
        .height = bounds.height,
    };

    return ESP_OK;
}

static esp_err_t rotate_quarter_turn_a8(
    grape_context_t *context,
    const grape_surface_t *surface,
    float angle,
    grape_shear_image_t *out_image
)
{
    GRAPE_TIME_SCOPE(SHEAR_QUARTER_TURN);
    return rotate_quarter_turn_a8_impl(context, surface, angle, out_image);
}

esp_err_t grape_shear_rotate_a8(
    grape_context_t *context,
    const grape_surface_t *surface,
    grape_shear_image_t *out_image
)
{
    if (!context || !surface || !surface->texture || !out_image) {
        return ESP_ERR_INVALID_ARG;
    }

    if (surface->texture->format != GRAPE_PIXEL_FORMAT_A8) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (fabsf(surface->transform.scale_x - 1.0f) > GRAPE_SHEAR_EPSILON ||
        fabsf(surface->transform.scale_y - 1.0f) > GRAPE_SHEAR_EPSILON) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    float angle = surface->shear_cache_valid
        ? surface->normalized_rotation
        : atan2f(surface->sin_rotation, surface->cos_rotation);

    if (fabsf(angle) > (GRAPE_SHEAR_PI * 0.5f + GRAPE_SHEAR_EPSILON)) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (fabsf(angle) <= GRAPE_SHEAR_EPSILON) {
        *out_image = (grape_shear_image_t) {
            .pixels = surface->texture->pixels,
            .stride = surface->texture->stride,
            .left = -surface->transform.origin_x,
            .top = -surface->transform.origin_y,
            .width = surface->texture->width,
            .height = surface->texture->height,
        };
        return ESP_OK;
    }

    if (fabsf(fabsf(angle) - GRAPE_SHEAR_PI * 0.5f) <= GRAPE_SHEAR_EPSILON) {
        return rotate_quarter_turn_a8(context, surface, angle, out_image);
    }

    float shear_x_coefficient;
    float shear_y_coefficient;
    shear_bounds_t stage1;
    shear_bounds_t stage2;
    shear_bounds_t stage3;
    esp_err_t ret;

    GRAPE_TIME_BLOCK(SHEAR_PREP) {
        shear_x_coefficient = surface->shear_cache_valid
            ? surface->shear_x_coefficient
            : -tanf(angle * 0.5f);
        shear_y_coefficient = surface->sin_rotation;

        ret = transformed_bounds(
            surface,
            1.0f,
            shear_x_coefficient,
            0.0f,
            1.0f,
            &stage1
        );
        if (ret != ESP_OK) {
            return ret;
        }

        ret = transformed_bounds(
            surface,
            1.0f,
            shear_x_coefficient,
            shear_y_coefficient,
            1.0f + shear_x_coefficient * shear_y_coefficient,
            &stage2
        );
        if (ret != ESP_OK) {
            return ret;
        }

        ret = transformed_bounds(
            surface,
            surface->cos_rotation,
            -surface->sin_rotation,
            surface->sin_rotation,
            surface->cos_rotation,
            &stage3
        );
        if (ret != ESP_OK) {
            return ret;
        }

        size_t stage1_size;
        size_t stage2_size;
        size_t stage3_size;

        ret = bounds_size(&stage1, &stage1_size);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = bounds_size(&stage2, &stage2_size);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = bounds_size(&stage3, &stage3_size);
        if (ret != ESP_OK) {
            return ret;
        }

        size_t buffer_a_required =
            stage1_size > stage3_size ? stage1_size : stage3_size;
        size_t buffer_b_required = stage2_size;

        if (grape_feature_is_active(context, GRAPE_FEATURE_PPA_A8_ROTATE)) {
            shear_source_t stage1_template = {
                .pixels = NULL,
                .stride = stage1.width,
                .left = (float)stage1.left,
                .top = (float)stage1.top,
                .width = stage1.width,
                .height = stage1.height,
            };

            ret = shear_y_ppa_buffer_requirements(
                &stage1_template,
                shear_y_coefficient,
                &buffer_a_required,
                &buffer_b_required
            );
            if (ret != ESP_OK) {
                return ret;
            }
        }

        ret = ensure_buffer(
            &context->shear_buffer_a,
            &context->shear_buffer_a_size,
            buffer_a_required
        );
        if (ret != ESP_OK) {
            return ret;
        }

        ret = ensure_buffer(
            &context->shear_buffer_b,
            &context->shear_buffer_b_size,
            buffer_b_required
        );
        if (ret != ESP_OK) {
            return ret;
        }
    }

    shear_source_t source = {
        .pixels = surface->texture->pixels,
        .stride = surface->texture->stride,
        .left = -surface->transform.origin_x,
        .top = -surface->transform.origin_y,
        .width = surface->texture->width,
        .height = surface->texture->height,
    };

    GRAPE_TIME_BLOCK(SHEAR_X1) {
        shear_x(
            &source,
            shear_x_coefficient,
            &stage1,
            context->shear_buffer_a
        );
    }

    source = (shear_source_t) {
        .pixels = context->shear_buffer_a,
        .stride = stage1.width,
        .left = (float)stage1.left,
        .top = (float)stage1.top,
        .width = stage1.width,
        .height = stage1.height,
    };

    grape_shear_image_t stage2_image;
    bool used_ppa_rotate_y = false;

    if (grape_feature_is_active(context, GRAPE_FEATURE_PPA_A8_ROTATE)) {
        ret = shear_y_via_ppa_rotate(
            context,
            &source,
            shear_y_coefficient,
            &stage2_image
        );
        if (ret == ESP_OK) {
            used_ppa_rotate_y = true;
        } else if (ret != ESP_ERR_NOT_SUPPORTED) {
            return ret;
        }
    }

    if (!used_ppa_rotate_y) {
        GRAPE_TIME_BLOCK(SHEAR_Y) {
            shear_y(
                &source,
                shear_y_coefficient,
                &stage2,
                context->shear_buffer_b
            );
        }

        stage2_image = (grape_shear_image_t) {
            .pixels = context->shear_buffer_b,
            .stride = stage2.width,
            .left = (float)stage2.left,
            .top = (float)stage2.top,
            .width = stage2.width,
            .height = stage2.height,
        };
    }

    source = (shear_source_t) {
        .pixels = stage2_image.pixels,
        .stride = stage2_image.stride,
        .left = stage2_image.left,
        .top = stage2_image.top,
        .width = stage2_image.width,
        .height = stage2_image.height,
    };

    GRAPE_TIME_BLOCK(SHEAR_X2) {
        shear_x(
            &source,
            shear_x_coefficient,
            &stage3,
            context->shear_buffer_a
        );
    }

    *out_image = (grape_shear_image_t) {
        .pixels = context->shear_buffer_a,
        .stride = stage3.width,
        .left = (float)stage3.left,
        .top = (float)stage3.top,
        .width = stage3.width,
        .height = stage3.height,
    };

    return ESP_OK;
}
