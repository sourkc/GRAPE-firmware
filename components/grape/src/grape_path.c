#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "grape/grape_path.h"
#include "grape/grape_telemetry.h"

typedef enum {
    GRAPE_PATH_COMMAND_MOVE_TO = 0,
    GRAPE_PATH_COMMAND_LINE_TO,
    GRAPE_PATH_COMMAND_CLOSE,
} grape_path_command_type_t;

typedef struct {
    grape_path_command_type_t type;
    float x;
    float y;
} grape_path_command_t;

typedef struct {
    float x0;
    float y0;
    float x1;
    float y1;
} grape_path_edge_t;

typedef struct {
    float x;
    int32_t winding_delta;
} grape_path_intersection_t;

struct grape_path {
    grape_path_command_t *commands;
    size_t command_count;
    size_t command_capacity;
    size_t line_count;
    float current_x;
    float current_y;
    float contour_start_x;
    float contour_start_y;
    grape_path_bounds_t bounds;
    bool has_current;
    bool contour_open;
    bool has_bounds;
};

static bool point_is_finite(float x, float y)
{
    return isfinite(x) && isfinite(y);
}

static esp_err_t path_reserve(grape_path_t *path, size_t required)
{
    if (required <= path->command_capacity) {
        return ESP_OK;
    }

    size_t capacity = path->command_capacity ? path->command_capacity : 16U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }

    if (capacity > SIZE_MAX / sizeof(*path->commands)) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_path_command_t *commands = realloc(
        path->commands,
        capacity * sizeof(*commands)
    );
    if (!commands) {
        return ESP_ERR_NO_MEM;
    }

    path->commands = commands;
    path->command_capacity = capacity;
    return ESP_OK;
}

static esp_err_t path_append(grape_path_t *path, grape_path_command_t command)
{
    esp_err_t ret = path_reserve(path, path->command_count + 1U);
    if (ret != ESP_OK) {
        return ret;
    }

    path->commands[path->command_count++] = command;
    return ESP_OK;
}

static void path_include_point(grape_path_t *path, float x, float y)
{
    if (!path->has_bounds) {
        path->bounds = (grape_path_bounds_t){
            .min_x = x,
            .min_y = y,
            .max_x = x,
            .max_y = y,
        };
        path->has_bounds = true;
        return;
    }

    if (x < path->bounds.min_x) path->bounds.min_x = x;
    if (x > path->bounds.max_x) path->bounds.max_x = x;
    if (y < path->bounds.min_y) path->bounds.min_y = y;
    if (y > path->bounds.max_y) path->bounds.max_y = y;
}

esp_err_t grape_path_create(grape_path_t **out_path)
{
    if (!out_path) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_path_t *path = calloc(1, sizeof(*path));
    if (!path) {
        return ESP_ERR_NO_MEM;
    }

    *out_path = path;
    return ESP_OK;
}

esp_err_t grape_path_destroy(grape_path_t *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    free(path->commands);
    free(path);
    return ESP_OK;
}

esp_err_t grape_path_clear(grape_path_t *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    path->command_count = 0;
    path->line_count = 0;
    path->current_x = 0.0f;
    path->current_y = 0.0f;
    path->contour_start_x = 0.0f;
    path->contour_start_y = 0.0f;
    path->bounds = (grape_path_bounds_t){0};
    path->has_current = false;
    path->contour_open = false;
    path->has_bounds = false;
    return ESP_OK;
}

esp_err_t grape_path_move_to(grape_path_t *path, float x, float y)
{
    if (!path || !point_is_finite(x, y)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = path_append(path, (grape_path_command_t){
        .type = GRAPE_PATH_COMMAND_MOVE_TO,
        .x = x,
        .y = y,
    });
    if (ret != ESP_OK) {
        return ret;
    }

    path->current_x = x;
    path->current_y = y;
    path->contour_start_x = x;
    path->contour_start_y = y;
    path->has_current = true;
    path->contour_open = true;
    path_include_point(path, x, y);
    return ESP_OK;
}

esp_err_t grape_path_line_to(grape_path_t *path, float x, float y)
{
    if (!path || !point_is_finite(x, y)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!path->has_current || !path->contour_open) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = path_append(path, (grape_path_command_t){
        .type = GRAPE_PATH_COMMAND_LINE_TO,
        .x = x,
        .y = y,
    });
    if (ret != ESP_OK) {
        return ret;
    }

    path->current_x = x;
    path->current_y = y;
    path->line_count++;
    path_include_point(path, x, y);
    return ESP_OK;
}

esp_err_t grape_path_close(grape_path_t *path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!path->has_current || !path->contour_open) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = path_append(path, (grape_path_command_t){
        .type = GRAPE_PATH_COMMAND_CLOSE,
    });
    if (ret != ESP_OK) {
        return ret;
    }

    path->current_x = path->contour_start_x;
    path->current_y = path->contour_start_y;
    path->has_current = false;
    path->contour_open = false;
    return ESP_OK;
}

esp_err_t grape_path_get_bounds(const grape_path_t *path, grape_path_bounds_t *out_bounds)
{
    if (!path || !out_bounds) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!path->has_bounds || path->line_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *out_bounds = path->bounds;
    return ESP_OK;
}

static void edge_append(grape_path_edge_t *edges,
                        size_t capacity,
                        size_t *count,
                        float x0,
                        float y0,
                        float x1,
                        float y1)
{
    if (*count >= capacity || (x0 == x1 && y0 == y1)) {
        return;
    }

    edges[(*count)++] = (grape_path_edge_t){
        .x0 = x0,
        .y0 = y0,
        .x1 = x1,
        .y1 = y1,
    };
}

static esp_err_t path_build_edges(const grape_path_t *path,
                                  grape_path_edge_t **out_edges,
                                  size_t *out_edge_count)
{
    if (!path || !out_edges || !out_edge_count || path->command_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (path->command_count > SIZE_MAX / sizeof(grape_path_edge_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_path_edge_t *edges = malloc(path->command_count * sizeof(*edges));
    if (!edges) {
        return ESP_ERR_NO_MEM;
    }

    size_t edge_count = 0;
    bool contour_active = false;
    bool contour_has_segment = false;
    float current_x = 0.0f;
    float current_y = 0.0f;
    float start_x = 0.0f;
    float start_y = 0.0f;

    for (size_t i = 0; i < path->command_count; ++i) {
        const grape_path_command_t *command = &path->commands[i];

        switch (command->type) {
        case GRAPE_PATH_COMMAND_MOVE_TO:
            if (contour_active && contour_has_segment) {
                edge_append(edges, path->command_count, &edge_count,
                            current_x, current_y, start_x, start_y);
            }
            current_x = command->x;
            current_y = command->y;
            start_x = command->x;
            start_y = command->y;
            contour_active = true;
            contour_has_segment = false;
            break;

        case GRAPE_PATH_COMMAND_LINE_TO:
            if (!contour_active) {
                free(edges);
                return ESP_ERR_INVALID_STATE;
            }
            edge_append(edges, path->command_count, &edge_count,
                        current_x, current_y, command->x, command->y);
            current_x = command->x;
            current_y = command->y;
            contour_has_segment = true;
            break;

        case GRAPE_PATH_COMMAND_CLOSE:
            if (contour_active && contour_has_segment) {
                edge_append(edges, path->command_count, &edge_count,
                            current_x, current_y, start_x, start_y);
            }
            contour_active = false;
            contour_has_segment = false;
            break;

        default:
            free(edges);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (contour_active && contour_has_segment) {
        edge_append(edges, path->command_count, &edge_count,
                    current_x, current_y, start_x, start_y);
    }

    if (edge_count == 0) {
        free(edges);
        return ESP_ERR_INVALID_STATE;
    }

    *out_edges = edges;
    *out_edge_count = edge_count;
    return ESP_OK;
}

static int intersection_compare(const void *lhs, const void *rhs)
{
    const grape_path_intersection_t *a = lhs;
    const grape_path_intersection_t *b = rhs;

    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    return 0;
}

static size_t build_scanline_intersections(const grape_path_edge_t *edges,
                                           size_t edge_count,
                                           float sample_y,
                                           grape_path_intersection_t *intersections)
{
    size_t count = 0;

    for (size_t i = 0; i < edge_count; ++i) {
        const grape_path_edge_t *edge = &edges[i];
        if (edge->y0 == edge->y1) {
            continue;
        }

        float min_y = fminf(edge->y0, edge->y1);
        float max_y = fmaxf(edge->y0, edge->y1);
        if (sample_y < min_y || sample_y >= max_y) {
            continue;
        }

        float t = (sample_y - edge->y0) / (edge->y1 - edge->y0);
        intersections[count++] = (grape_path_intersection_t){
            .x = edge->x0 + t * (edge->x1 - edge->x0),
            .winding_delta = edge->y1 > edge->y0 ? 1 : -1,
        };
    }

    qsort(intersections, count, sizeof(*intersections), intersection_compare);
    return count;
}

static esp_err_t raster_dimensions(const grape_path_bounds_t *bounds,
                                   const grape_path_rasterize_config_t *config,
                                   uint32_t *out_width,
                                   uint32_t *out_height,
                                   float *out_origin_x,
                                   float *out_origin_y)
{
    double scale = (double)config->pixels_per_unit;
    double min_x_px = floor((double)bounds->min_x * scale) - config->padding_pixels;
    double min_y_px = floor((double)bounds->min_y * scale) - config->padding_pixels;
    double max_x_px = ceil((double)bounds->max_x * scale) + config->padding_pixels;
    double max_y_px = ceil((double)bounds->max_y * scale) + config->padding_pixels;
    double width = max_x_px - min_x_px;
    double height = max_y_px - min_y_px;

    if (!isfinite(min_x_px) || !isfinite(min_y_px) ||
        !isfinite(max_x_px) || !isfinite(max_y_px) ||
        width < 1.0 || height < 1.0 ||
        width > UINT32_MAX || height > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    *out_origin_x = (float)(min_x_px / scale);
    *out_origin_y = (float)(min_y_px / scale);
    return ESP_OK;
}

esp_err_t grape_path_rasterize_a8(grape_context_t *context,
                                   const grape_path_t *path,
                                   const grape_path_rasterize_config_t *config,
                                   grape_path_raster_t *out_raster)
{
    if (!context || !path || !config || !out_raster ||
        !isfinite(config->pixels_per_unit) || config->pixels_per_unit <= 0.0f ||
        config->samples_per_axis == 0 || config->samples_per_axis > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_raster = (grape_path_raster_t){0};
    GRAPE_TIME_SCOPE(VECTOR_RASTERIZE);

    grape_path_bounds_t bounds = {0};
    esp_err_t ret = grape_path_get_bounds(path, &bounds);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_path_edge_t *edges = NULL;
    size_t edge_count = 0;
    ret = path_build_edges(path, &edges, &edge_count);
    if (ret != ESP_OK) {
        return ret;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    ret = raster_dimensions(
        &bounds,
        config,
        &width,
        &height,
        &origin_x,
        &origin_y
    );
    if (ret != ESP_OK) {
        free(edges);
        return ret;
    }

    grape_texture_desc_t texture_desc = {
        .width = width,
        .height = height,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = config->memory,
    };

    grape_texture_t *texture = NULL;
    ret = grape_texture_create(context, &texture_desc, &texture);
    if (ret != ESP_OK) {
        free(edges);
        return ret;
    }

    if (edge_count > SIZE_MAX / sizeof(grape_path_intersection_t)) {
        grape_texture_destroy(texture);
        free(edges);
        return ESP_ERR_INVALID_SIZE;
    }

    grape_path_intersection_t *intersections = malloc(
        edge_count * sizeof(*intersections)
    );
    if (!intersections) {
        grape_texture_destroy(texture);
        free(edges);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *pixels = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    const uint32_t samples = config->samples_per_axis;
    const uint32_t coverage_samples = samples * samples;
    const float sample_step = 1.0f / (config->pixels_per_unit * (float)samples);

    if (width > SIZE_MAX / samples || height > SIZE_MAX / samples) {
        free(intersections);
        grape_texture_destroy(texture);
        free(edges);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t subpixel_width = (size_t)width * samples;
    size_t subpixel_height = (size_t)height * samples;

    for (size_t sub_y = 0; sub_y < subpixel_height; ++sub_y) {
        float sample_y = origin_y + ((float)sub_y + 0.5f) * sample_step;
        size_t intersection_count = build_scanline_intersections(
            edges,
            edge_count,
            sample_y,
            intersections
        );

        if (intersection_count == 0) {
            continue;
        }

        uint8_t *row = pixels + (sub_y / samples) * stride;
        size_t intersection_index = 0;
        int32_t winding = 0;
        float sample_x = origin_x + 0.5f * sample_step;

        for (size_t sub_x = 0; sub_x < subpixel_width; ++sub_x) {
            while (intersection_index < intersection_count &&
                   intersections[intersection_index].x <= sample_x) {
                winding += intersections[intersection_index].winding_delta;
                intersection_index++;
            }

            if (winding != 0) {
                row[sub_x / samples]++;
            }

            sample_x += sample_step;
        }
    }

    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            row[x] = (uint8_t)(((uint32_t)row[x] * 255U + coverage_samples / 2U) /
                               coverage_samples);
        }
    }

    free(intersections);
    free(edges);

    ret = grape_texture_invalidate(texture);
    if (ret != ESP_OK) {
        grape_texture_destroy(texture);
        return ret;
    }

    *out_raster = (grape_path_raster_t){
        .texture = texture,
        .path_origin_x = origin_x,
        .path_origin_y = origin_y,
        .pixels_per_unit = config->pixels_per_unit,
    };
    return ESP_OK;
}
