#include "grape/grape_benchmark_hooks.h"

#include "grape_internal.h"
#include "grape_gpu_internal.h"
#include <string.h>

esp_err_t grape_benchmark_damage_grid_info(
    const grape_context_t *context,
    grape_benchmark_damage_grid_info_t *out_info
)
{
    if (!context || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_info = (grape_benchmark_damage_grid_info_t) {
        .tile_columns = context->damage.tile_columns,
        .tile_rows = context->damage.tile_rows,
        .tile_size = CONFIG_GRAPE_DAMAGE_TILE_SIZE,
        .bitmap_size = context->damage.bitmap_size,
    };
    return ESP_OK;
}

void grape_benchmark_damage_clear(grape_context_t *context)
{
    grape_damage_clear(context);
}

esp_err_t grape_benchmark_texture_occupancy_info(
    const grape_texture_t *texture,
    grape_benchmark_occupancy_info_t *out_info
)
{
    if (!texture || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_info = (grape_benchmark_occupancy_info_t) {
        .columns = texture->occupancy_columns,
        .rows = texture->occupancy_rows,
        .bitmap_size = texture->occupancy_bitmap_size,
        .occupied_cells = texture->occupancy_occupied_count,
        .all_full = texture->occupancy_all_full,
        .all_empty = texture->occupancy_all_empty,
    };
    return ESP_OK;
}

esp_err_t grape_benchmark_render_rects(
    grape_context_t *context,
    const grape_rect_t *rects,
    size_t rect_count,
    bool present
)
{
    if (!context || (rect_count > 0 && !rects)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rect_count == 0) {
        return ESP_OK;
    }

    grape_display_render_target_t target = {0};
    esp_err_t ret = grape_display_begin_frame(
        context->display,
        rects,
        rect_count,
        &target
    );
    if (ret != ESP_OK) {
        return ret;
    }

    context->render_target = target;
    for (size_t i = 0; i < rect_count; ++i) {
        ret = grape_compositor_render(context, rects[i]);
        if (ret != ESP_OK) {
            context->render_target = (grape_display_render_target_t){0};
            return ret;
        }
    }

    if (present) {
        ret = grape_display_present(context->display);
    }

    context->render_target = (grape_display_render_target_t){0};
    return ret;
}

size_t grape_benchmark_shear_scratch_bytes(const grape_context_t *context)
{
    if (!context) {
        return 0;
    }
    return context->shear_buffer_a_size + context->shear_buffer_b_size;
}

esp_err_t grape_benchmark_copy_presented(const grape_context_t *context,
                                        void *dst, size_t size)
{
    if (!context) return ESP_ERR_INVALID_ARG;
    return grape_display_copy_presented_frame(context->display, dst, size);
}

esp_err_t grape_benchmark_copy_depth(const grape_gpu_depth_buffer_t *buffer,
                                    void *dst, size_t size)
{
    if (!buffer || !dst) return ESP_ERR_INVALID_ARG;
    if (buffer->context->render_pass_active) return ESP_ERR_INVALID_STATE;
    size_t row_bytes = (size_t)buffer->width * (size_t)buffer->sample_count * 2U;
    if (size < row_bytes * buffer->height) return ESP_ERR_INVALID_SIZE;
    for (uint32_t y = 0; y < buffer->height; ++y) {
        uint8_t *row = (uint8_t *)dst + y * row_bytes;
        for (uint32_t x = 0; x < buffer->width; ++x) {
            size_t tile = (size_t)(y / GRAPE_GPU_MSAA_TILE_SIZE) * buffer->tile_cols +
                          x / GRAPE_GPU_MSAA_TILE_SIZE;
            bool clear = buffer->lazy_clear_active && !buffer->lazy_tiles[tile];
            for (uint32_t s = 0; s < (uint32_t)buffer->sample_count; ++s) {
                size_t offset = ((size_t)x * buffer->sample_count + s) * 2U;
                uint16_t value;
                if (clear) value = buffer->lazy_clear_value;
                else memcpy(&value, (const uint8_t *)buffer->data + y * buffer->stride + offset, 2);
                row[offset] = (uint8_t)value;
                row[offset + 1U] = (uint8_t)(value >> 8);
            }
        }
    }
    return ESP_OK;
}
