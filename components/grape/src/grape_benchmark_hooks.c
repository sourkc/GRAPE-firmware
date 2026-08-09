#include "grape/grape_benchmark_hooks.h"

#include "grape_internal.h"

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
