#include <math.h>
#include <stdlib.h>

#include "grape_glyph_cache_internal.h"

#define GRAPE_GLYPH_CACHE_ENTRY_INITIAL_CAPACITY 32U

typedef struct {
    const grape_font_t *font;
    uint16_t glyph_id;
    uint8_t samples_per_axis;
    float pixels_per_unit;
    grape_texture_t *texture;
    grape_path_bounds_t bounds;
    float path_origin_x;
    float path_origin_y;
    size_t bytes;
    uint64_t last_used;
} grape_glyph_cache_entry_t;

struct grape_glyph_cache {
    grape_context_t *context;
    grape_glyph_cache_config_t config;
    grape_glyph_cache_entry_t *entries;
    size_t entry_count;
    size_t entry_capacity;
    size_t used_bytes;
    uint64_t use_counter;
    uint64_t requests;
    uint64_t exact_hits;
    uint64_t scaled_hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t uncached_rasters;
    uint64_t cpu_scales;
    grape_path_t *scratch_path;
};

static uint64_t cache_next_use(grape_glyph_cache_t *cache)
{
    return ++cache->use_counter;
}

static esp_err_t cache_reserve_entries(grape_glyph_cache_t *cache,
                                       size_t required)
{
    if (required <= cache->entry_capacity) {
        return ESP_OK;
    }

    size_t capacity = cache->entry_capacity
        ? cache->entry_capacity
        : GRAPE_GLYPH_CACHE_ENTRY_INITIAL_CAPACITY;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }

    if (capacity > SIZE_MAX / sizeof(*cache->entries)) {
        return ESP_ERR_INVALID_SIZE;
    }

    grape_glyph_cache_entry_t *entries = realloc(
        cache->entries,
        capacity * sizeof(*entries)
    );
    if (!entries) {
        return ESP_ERR_NO_MEM;
    }

    cache->entries = entries;
    cache->entry_capacity = capacity;
    return ESP_OK;
}

static esp_err_t cache_remove_entry(grape_glyph_cache_t *cache, size_t index)
{
    if (index >= cache->entry_count) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_glyph_cache_entry_t *entry = &cache->entries[index];
    esp_err_t ret = grape_texture_destroy(entry->texture);
    if (ret != ESP_OK) {
        return ret;
    }

    cache->used_bytes -= entry->bytes;
    cache->entry_count--;
    if (index != cache->entry_count) {
        cache->entries[index] = cache->entries[cache->entry_count];
    }
    return ESP_OK;
}

static esp_err_t cache_evict_lru(grape_glyph_cache_t *cache)
{
    if (cache->entry_count == 0U) {
        return ESP_ERR_NOT_FOUND;
    }

    size_t oldest_index = 0U;
    uint64_t oldest_use = cache->entries[0].last_used;
    for (size_t i = 1U; i < cache->entry_count; ++i) {
        if (cache->entries[i].last_used < oldest_use) {
            oldest_use = cache->entries[i].last_used;
            oldest_index = i;
        }
    }

    esp_err_t ret = cache_remove_entry(cache, oldest_index);
    if (ret == ESP_OK) {
        cache->evictions++;
    }
    return ret;
}

static esp_err_t cache_make_room(grape_glyph_cache_t *cache, size_t bytes)
{
    if (bytes > cache->config.capacity_bytes) {
        return ESP_ERR_NO_MEM;
    }

    while (cache->used_bytes > cache->config.capacity_bytes - bytes) {
        esp_err_t ret = cache_evict_lru(cache);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t cache_store(grape_glyph_cache_t *cache,
                             const grape_font_t *font,
                             uint16_t glyph_id,
                             uint8_t samples_per_axis,
                             float pixels_per_unit,
                             grape_texture_t *texture,
                             grape_path_bounds_t bounds,
                             float path_origin_x,
                             float path_origin_y,
                             bool *out_stored)
{
    if (!out_stored) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_stored = false;

    uint32_t width = grape_texture_width(texture);
    uint32_t height = grape_texture_height(texture);
    if (width != 0U && height > SIZE_MAX / width) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t bytes = (size_t)width * height;

    if (bytes > cache->config.capacity_bytes ||
        cache->config.capacity_bytes == 0U) {
        return ESP_OK;
    }

    esp_err_t ret = cache_make_room(cache, bytes);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = cache_reserve_entries(cache, cache->entry_count + 1U);
    if (ret != ESP_OK) {
        return ret;
    }

    cache->entries[cache->entry_count++] = (grape_glyph_cache_entry_t){
        .font = font,
        .glyph_id = glyph_id,
        .samples_per_axis = samples_per_axis,
        .pixels_per_unit = pixels_per_unit,
        .texture = texture,
        .bounds = bounds,
        .path_origin_x = path_origin_x,
        .path_origin_y = path_origin_y,
        .bytes = bytes,
        .last_used = cache_next_use(cache),
    };
    cache->used_bytes += bytes;
    *out_stored = true;
    return ESP_OK;
}

static grape_glyph_cache_entry_t *cache_find_exact(
    grape_glyph_cache_t *cache,
    const grape_font_t *font,
    uint16_t glyph_id,
    uint8_t samples_per_axis,
    float pixels_per_unit)
{
    for (size_t i = 0; i < cache->entry_count; ++i) {
        grape_glyph_cache_entry_t *entry = &cache->entries[i];
        if (entry->font == font &&
            entry->glyph_id == glyph_id &&
            entry->samples_per_axis == samples_per_axis &&
            entry->pixels_per_unit == pixels_per_unit) {
            return entry;
        }
    }
    return NULL;
}

static grape_glyph_cache_entry_t *cache_find_scale_source(
    grape_glyph_cache_t *cache,
    const grape_font_t *font,
    uint16_t glyph_id,
    uint8_t samples_per_axis,
    float pixels_per_unit)
{
    grape_glyph_cache_entry_t *best = NULL;
    for (size_t i = 0; i < cache->entry_count; ++i) {
        grape_glyph_cache_entry_t *entry = &cache->entries[i];
        if (entry->font != font ||
            entry->glyph_id != glyph_id ||
            entry->samples_per_axis != samples_per_axis ||
            entry->pixels_per_unit <= pixels_per_unit) {
            continue;
        }

        float ratio = entry->pixels_per_unit / pixels_per_unit;
        if (!isfinite(ratio) || ratio > cache->config.max_downscale_ratio) {
            continue;
        }

        if (!best || entry->pixels_per_unit < best->pixels_per_unit) {
            best = entry;
        }
    }
    return best;
}

static esp_err_t raster_dimensions(const grape_path_bounds_t *bounds,
                                   float pixels_per_unit,
                                   uint32_t padding_pixels,
                                   uint32_t *out_width,
                                   uint32_t *out_height,
                                   float *out_origin_x,
                                   float *out_origin_y)
{
    double scale = (double)pixels_per_unit;
    double min_x_px = floor((double)bounds->min_x * scale) - padding_pixels;
    double min_y_px = floor((double)bounds->min_y * scale) - padding_pixels;
    double max_x_px = ceil((double)bounds->max_x * scale) + padding_pixels;
    double max_y_px = ceil((double)bounds->max_y * scale) + padding_pixels;
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

static uint8_t sample_a8_bilinear(const grape_texture_t *texture,
                                  float x,
                                  float y)
{
    if (!isfinite(x) || !isfinite(y)) {
        return 0U;
    }

    int32_t x0 = (int32_t)floorf(x);
    int32_t y0 = (int32_t)floorf(y);
    float fx = x - (float)x0;
    float fy = y - (float)y0;

    const uint8_t *pixels = grape_texture_pixels_const(texture);
    size_t stride = grape_texture_stride(texture);
    int32_t width = (int32_t)grape_texture_width(texture);
    int32_t height = (int32_t)grape_texture_height(texture);

    float samples[4] = {0};
    const int32_t xs[2] = {x0, x0 + 1};
    const int32_t ys[2] = {y0, y0 + 1};
    for (uint32_t iy = 0; iy < 2U; ++iy) {
        if (ys[iy] < 0 || ys[iy] >= height) {
            continue;
        }
        const uint8_t *row = pixels + (size_t)ys[iy] * stride;
        for (uint32_t ix = 0; ix < 2U; ++ix) {
            if (xs[ix] >= 0 && xs[ix] < width) {
                samples[iy * 2U + ix] = row[xs[ix]];
            }
        }
    }

    float top = samples[0] + (samples[1] - samples[0]) * fx;
    float bottom = samples[2] + (samples[3] - samples[2]) * fx;
    float value = top + (bottom - top) * fy;
    if (value <= 0.0f) {
        return 0U;
    }
    if (value >= 255.0f) {
        return 255U;
    }
    return (uint8_t)(value + 0.5f);
}

static esp_err_t scale_cpu(grape_glyph_cache_t *cache,
                           const grape_glyph_cache_entry_t *source,
                           float pixels_per_unit,
                           grape_texture_t **out_texture,
                           float *out_origin_x,
                           float *out_origin_y)
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    esp_err_t ret = raster_dimensions(
        &source->bounds,
        pixels_per_unit,
        cache->config.raster_padding_pixels,
        &width,
        &height,
        &origin_x,
        &origin_y
    );
    if (ret != ESP_OK) {
        return ret;
    }

    grape_texture_desc_t desc = {
        .width = width,
        .height = height,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = cache->config.memory,
    };
    grape_texture_t *texture = NULL;
    ret = grape_texture_create(cache->context, &desc, &texture);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *dst = grape_texture_pixels(texture);
    size_t dst_stride = grape_texture_stride(texture);
    float source_step = source->pixels_per_unit / pixels_per_unit;
    float source_start_x =
        (origin_x - source->path_origin_x) * source->pixels_per_unit +
        0.5f * source_step - 0.5f;
    float source_start_y =
        (origin_y - source->path_origin_y) * source->pixels_per_unit +
        0.5f * source_step - 0.5f;

    float source_y = source_start_y;
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t *row = dst + (size_t)y * dst_stride;
        float source_x = source_start_x;
        for (uint32_t x = 0; x < width; ++x) {
            row[x] = sample_a8_bilinear(source->texture, source_x, source_y);
            source_x += source_step;
        }
        source_y += source_step;
    }

    ret = grape_texture_invalidate(texture);
    if (ret != ESP_OK) {
        grape_texture_destroy(texture);
        return ret;
    }

    *out_texture = texture;
    *out_origin_x = origin_x;
    *out_origin_y = origin_y;
    return ESP_OK;
}

static esp_err_t scale_from_entry(grape_glyph_cache_t *cache,
                                  const grape_glyph_cache_entry_t *source,
                                  float pixels_per_unit,
                                  grape_texture_t **out_texture,
                                  float *out_origin_x,
                                  float *out_origin_y)
{
    switch (cache->config.scale_backend) {
        case GRAPE_GLYPH_CACHE_SCALE_CPU:
            cache->cpu_scales++;
            return scale_cpu(
                cache,
                source,
                pixels_per_unit,
                out_texture,
                out_origin_x,
                out_origin_y
            );
        case GRAPE_GLYPH_CACHE_SCALE_PPA:
            return ESP_ERR_NOT_SUPPORTED;
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t rasterize_glyph(grape_glyph_cache_t *cache,
                                 const grape_font_t *font,
                                 uint16_t glyph_id,
                                 float pixels_per_unit,
                                 uint8_t samples_per_axis,
                                 grape_texture_t **out_texture,
                                 grape_path_bounds_t *out_bounds,
                                 float *out_origin_x,
                                 float *out_origin_y)
{
    esp_err_t ret = grape_font_get_glyph_path(font, glyph_id, cache->scratch_path);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_path_bounds_t bounds = {0};
    ret = grape_path_get_bounds(cache->scratch_path, &bounds);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_path_rasterize_config_t config = GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT();
    config.pixels_per_unit = pixels_per_unit;
    config.samples_per_axis = samples_per_axis;
    config.padding_pixels = cache->config.raster_padding_pixels;
    config.memory = cache->config.memory;

    grape_path_raster_t raster = {0};
    ret = grape_path_rasterize_a8(
        cache->context,
        cache->scratch_path,
        &config,
        &raster
    );
    if (ret != ESP_OK) {
        return ret;
    }

    *out_texture = raster.texture;
    *out_bounds = bounds;
    *out_origin_x = raster.path_origin_x;
    *out_origin_y = raster.path_origin_y;
    return ESP_OK;
}

esp_err_t grape_glyph_cache_create(grape_context_t *context,
                                   const grape_glyph_cache_config_t *config,
                                   grape_glyph_cache_t **out_cache)
{
    if (!out_cache) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_cache = NULL;

    if (!context || !config || !isfinite(config->max_downscale_ratio) ||
        config->max_downscale_ratio < 1.0f ||
        (config->memory != GRAPE_MEMORY_DEFAULT &&
         config->memory != GRAPE_MEMORY_INTERNAL &&
         config->memory != GRAPE_MEMORY_PSRAM) ||
        (config->scale_backend != GRAPE_GLYPH_CACHE_SCALE_CPU &&
         config->scale_backend != GRAPE_GLYPH_CACHE_SCALE_PPA)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->scale_backend == GRAPE_GLYPH_CACHE_SCALE_PPA) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    grape_glyph_cache_t *cache = calloc(1, sizeof(*cache));
    if (!cache) {
        return ESP_ERR_NO_MEM;
    }

    cache->context = context;
    cache->config = *config;

    esp_err_t ret = grape_path_create(&cache->scratch_path);
    if (ret != ESP_OK) {
        free(cache);
        return ret;
    }

    *out_cache = cache;
    return ESP_OK;
}

esp_err_t grape_glyph_cache_clear(grape_glyph_cache_t *cache)
{
    if (!cache) {
        return ESP_ERR_INVALID_ARG;
    }

    while (cache->entry_count > 0U) {
        esp_err_t ret = cache_remove_entry(cache, cache->entry_count - 1U);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t grape_glyph_cache_destroy(grape_glyph_cache_t *cache)
{
    if (!cache) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = grape_glyph_cache_clear(cache);
    if (ret != ESP_OK) {
        return ret;
    }

    grape_path_destroy(cache->scratch_path);
    free(cache->entries);
    free(cache);
    return ESP_OK;
}

grape_context_t *grape_glyph_cache_context(grape_glyph_cache_t *cache)
{
    return cache ? cache->context : NULL;
}

void grape_glyph_cache_get_stats(const grape_glyph_cache_t *cache,
                                 grape_glyph_cache_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }

    if (!cache) {
        *out_stats = (grape_glyph_cache_stats_t){0};
        return;
    }

    *out_stats = (grape_glyph_cache_stats_t){
        .requests = cache->requests,
        .exact_hits = cache->exact_hits,
        .scaled_hits = cache->scaled_hits,
        .misses = cache->misses,
        .evictions = cache->evictions,
        .uncached_rasters = cache->uncached_rasters,
        .cpu_scales = cache->cpu_scales,
        .entry_count = cache->entry_count,
        .used_bytes = cache->used_bytes,
        .capacity_bytes = cache->config.capacity_bytes,
    };
}

esp_err_t grape_glyph_cache_acquire(grape_glyph_cache_t *cache,
                                    const grape_font_t *font,
                                    uint16_t glyph_id,
                                    float pixels_per_unit,
                                    uint8_t samples_per_axis,
                                    grape_glyph_cache_raster_t *out_raster)
{
    if (!cache || !font || !out_raster ||
        !isfinite(pixels_per_unit) || pixels_per_unit <= 0.0f ||
        samples_per_axis == 0U || samples_per_axis > 8U) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_raster = (grape_glyph_cache_raster_t){0};
    cache->requests++;

    grape_glyph_cache_entry_t *entry = cache_find_exact(
        cache,
        font,
        glyph_id,
        samples_per_axis,
        pixels_per_unit
    );
    if (entry) {
        entry->last_used = cache_next_use(cache);
        cache->exact_hits++;
        *out_raster = (grape_glyph_cache_raster_t){
            .texture = entry->texture,
            .path_origin_x = entry->path_origin_x,
            .path_origin_y = entry->path_origin_y,
            .pixels_per_unit = entry->pixels_per_unit,
            .transient = false,
        };
        return ESP_OK;
    }

    grape_glyph_cache_entry_t *source = cache_find_scale_source(
        cache,
        font,
        glyph_id,
        samples_per_axis,
        pixels_per_unit
    );
    if (source) {
        source->last_used = cache_next_use(cache);

        grape_texture_t *texture = NULL;
        float origin_x = 0.0f;
        float origin_y = 0.0f;
        esp_err_t ret = scale_from_entry(
            cache,
            source,
            pixels_per_unit,
            &texture,
            &origin_x,
            &origin_y
        );
        if (ret != ESP_OK) {
            return ret;
        }

        cache->scaled_hits++;
        bool stored = false;
        ret = cache_store(
            cache,
            font,
            glyph_id,
            samples_per_axis,
            pixels_per_unit,
            texture,
            source->bounds,
            origin_x,
            origin_y,
            &stored
        );
        if (ret != ESP_OK) {
            grape_texture_destroy(texture);
            return ret;
        }

        if (!stored) {
            cache->uncached_rasters++;
        }
        *out_raster = (grape_glyph_cache_raster_t){
            .texture = texture,
            .path_origin_x = origin_x,
            .path_origin_y = origin_y,
            .pixels_per_unit = pixels_per_unit,
            .transient = !stored,
        };
        return ESP_OK;
    }

    cache->misses++;
    grape_texture_t *texture = NULL;
    grape_path_bounds_t bounds = {0};
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    esp_err_t ret = rasterize_glyph(
        cache,
        font,
        glyph_id,
        pixels_per_unit,
        samples_per_axis,
        &texture,
        &bounds,
        &origin_x,
        &origin_y
    );
    if (ret != ESP_OK) {
        return ret;
    }

    bool stored = false;
    ret = cache_store(
        cache,
        font,
        glyph_id,
        samples_per_axis,
        pixels_per_unit,
        texture,
        bounds,
        origin_x,
        origin_y,
        &stored
    );
    if (ret != ESP_OK) {
        grape_texture_destroy(texture);
        return ret;
    }

    if (!stored) {
        cache->uncached_rasters++;
    }
    *out_raster = (grape_glyph_cache_raster_t){
        .texture = texture,
        .path_origin_x = origin_x,
        .path_origin_y = origin_y,
        .pixels_per_unit = pixels_per_unit,
        .transient = !stored,
    };
    return ESP_OK;
}

void grape_glyph_cache_release(grape_glyph_cache_t *cache,
                               grape_glyph_cache_raster_t *raster)
{
    (void)cache;
    if (!raster) {
        return;
    }

    if (raster->transient && raster->texture) {
        grape_texture_destroy(raster->texture);
    }
    *raster = (grape_glyph_cache_raster_t){0};
}
