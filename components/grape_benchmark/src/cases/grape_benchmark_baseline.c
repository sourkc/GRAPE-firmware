#include "grape_benchmark_internal.h"
#include "generated_brightness.h"
#include "generated_procedural_gradient.h"

#include <stdlib.h>
#include <string.h>

typedef enum { DRAW, EDIT_ALPHA, ROTATE, CHAIN, PROCEDURAL, FILL_CPU, FILL_PPA } operation_t;
typedef struct {
    operation_t op;
    grape_pixel_format_t format;
    uint32_t width, height, layers;
    float scale;
    grape_texture_filter_t filter;
} config_t;
typedef struct {
    const config_t *config;
    grape_texture_t *texture;
    grape_surface_t *surfaces[4];
    grape_rect_t rect, previous_edit;
    bool have_previous;
    grape_feature_mode_t old_fill, old_blend;
    grape_rotation_backend_t old_rotation;
} state_t;

static void release(grape_benchmark_runtime_t *runtime, state_t *s)
{
    if (!s) return;
    for (unsigned i = 0; i < 4; ++i)
        if (s->surfaces[i]) grape_surface_destroy(s->surfaces[i]);
    if (s->texture) grape_texture_destroy(s->texture);
    grape_feature_set_mode(runtime->grape, GRAPE_FEATURE_PPA_FILL, s->old_fill);
    grape_feature_set_mode(runtime->grape, GRAPE_FEATURE_PPA_A8_BLEND, s->old_blend);
    grape_set_rotation_backend(runtime->grape, s->old_rotation);
    free(s);
}

static void paint(grape_texture_t *texture, grape_rect_t r, bool erase)
{
    uint8_t *pixels = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    grape_pixel_format_t format = grape_texture_format(texture);
    for (int32_t y = r.y; y < r.y + r.height; ++y) {
        uint8_t *row = pixels + (size_t)y * stride;
        for (int32_t x = r.x; x < r.x + r.width; ++x) {
            uint8_t a = erase ? 0 : (uint8_t)((((x / 8) ^ (y / 8)) & 1) ? 255 : 83);
            if (format == GRAPE_PIXEL_FORMAT_A8) row[x] = a;
            else if (format == GRAPE_PIXEL_FORMAT_RGB565)
                ((uint16_t *)row)[x] = (uint16_t)(((x & 31) << 11) | ((y & 63) << 5) | ((x + y) & 31));
            else {
                uint8_t *p = row + (size_t)x * 4;
                p[0] = (uint8_t)(x * 3); p[1] = (uint8_t)(y * 5);
                p[2] = (uint8_t)(x ^ y); p[3] = a;
            }
        }
    }
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bc, void **out)
{
    const config_t *cfg = bc->user_data;
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);
    if (!cfg || !display) return ESP_ERR_INVALID_ARG;
    if (cfg->width > display->width || cfg->height > display->height)
        return ESP_ERR_NOT_SUPPORTED;
    state_t *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->config = cfg;
    s->old_rotation = grape_get_rotation_backend(runtime->grape);
    grape_feature_get_mode(runtime->grape, GRAPE_FEATURE_PPA_FILL, &s->old_fill);
    grape_feature_get_mode(runtime->grape, GRAPE_FEATURE_PPA_A8_BLEND, &s->old_blend);
    s->rect = (grape_rect_t){16, 16, (int32_t)cfg->width, (int32_t)cfg->height};
    if (s->rect.x + s->rect.width > (int32_t)display->width) s->rect.x = 0;
    if (s->rect.y + s->rect.height > (int32_t)display->height) s->rect.y = 0;
    esp_err_t ret = ESP_OK;
    if (cfg->op == FILL_CPU || cfg->op == FILL_PPA) {
        if (cfg->op == FILL_PPA && !grape_feature_is_available(runtime->grape, GRAPE_FEATURE_PPA_FILL)) {
            ret = ESP_ERR_NOT_SUPPORTED; goto fail;
        }
        ret = grape_feature_set_mode(runtime->grape, GRAPE_FEATURE_PPA_FILL,
            cfg->op == FILL_PPA ? GRAPE_FEATURE_MODE_ENABLED : GRAPE_FEATURE_MODE_DISABLED);
        if (ret != ESP_OK) goto fail;
        /* A patterned sentinel makes a broken/no-op fill visible in references. */
        grape_texture_desc_t td = { .width=cfg->width, .height=cfg->height,
            .format=GRAPE_PIXEL_FORMAT_RGB565, .memory=GRAPE_MEMORY_PSRAM };
        ret = grape_texture_create(runtime->grape, &td, &s->texture);
        if (ret != ESP_OK) goto fail;
        paint(s->texture, (grape_rect_t){0,0,(int32_t)cfg->width,(int32_t)cfg->height}, false);
        ret = grape_texture_invalidate(s->texture);
        if (ret != ESP_OK) goto fail;
        grape_surface_desc_t sd = GRAPE_SURFACE_DESC_TEXTURE(s->texture);
        sd.transform.x = (float)s->rect.x; sd.transform.y = (float)s->rect.y;
        ret = grape_surface_create(runtime->grape, &sd, &s->surfaces[0]);
        if (ret != ESP_OK) goto fail;
    } else {
        grape_texture_desc_t td = { .width = cfg->width, .height = cfg->height,
            .format = cfg->format, .memory = GRAPE_MEMORY_PSRAM };
        ret = grape_texture_create(runtime->grape, &td, &s->texture);
        if (ret != ESP_OK) goto fail;
        paint(s->texture, (grape_rect_t){0,0,(int32_t)cfg->width,(int32_t)cfg->height}, false);
        ret = grape_texture_invalidate(s->texture);
        if (ret != ESP_OK) goto fail;
        generated_brightness_uniforms_t u[2] = {{ .brightness = 0.75f }, { .brightness = 1.15f }};
        grape_surface_shader_desc_t shaders[2] = {
            { &generated_brightness_program, &u[0] }, { &generated_brightness_program, &u[1] }
        };
        for (uint32_t i = 0; i < cfg->layers; ++i) {
            grape_surface_desc_t sd = GRAPE_SURFACE_DESC_TEXTURE(s->texture);
            sd.transform.x = (float)s->rect.x;
            sd.transform.y = (float)s->rect.y;
            sd.transform.scale_x = cfg->scale; sd.transform.scale_y = cfg->scale;
            sd.texture_filter = cfg->filter;
            sd.z = (int32_t)i;
            if (cfg->op == CHAIN) { sd.shaders = shaders; sd.shader_count = 2; }
            ret = grape_surface_create(runtime->grape, &sd, &s->surfaces[i]);
            if (ret != ESP_OK) goto fail;
        }
    }
    /* Repair both physical buffers before measuring partial updates. */
    for (unsigned i = 0; i < 2; ++i) {
        ret = grape_invalidate_all(runtime->grape);
        if (ret == ESP_OK) ret = grape_present(runtime->grape);
        if (ret != ESP_OK) goto fail;
    }
    if (cfg->op == FILL_CPU || cfg->op == FILL_PPA) {
        ret = grape_surface_destroy(s->surfaces[0]);
        if (ret != ESP_OK) goto fail;
        s->surfaces[0] = NULL;
        ret = grape_texture_destroy(s->texture);
        if (ret != ESP_OK) goto fail;
        s->texture = NULL;
        grape_benchmark_damage_clear(runtime->grape);
    }
    *out = s;
    return ESP_OK;
fail:
    release(runtime, s);
    return ret;
}

static esp_err_t iteration(grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bc, void *opaque, uint32_t sequence)
{
    (void)bc;
    state_t *s = opaque;
    const config_t *c = s->config;
    if (c->op == FILL_CPU || c->op == FILL_PPA)
        return grape_benchmark_render_rects(runtime->grape, &s->rect, 1, false);
    if (c->op == EDIT_ALPHA) {
        if (s->have_previous) {
            paint(s->texture, s->previous_edit, true);
            grape_rect_t old = s->previous_edit;
            esp_err_t ret = grape_texture_invalidate_rect(s->texture,
                (uint32_t)old.x, (uint32_t)old.y, (uint32_t)old.width, (uint32_t)old.height);
            if (ret != ESP_OK) return ret;
        }
        grape_rect_t r = { (int32_t)((sequence * 17U) % (c->width - 8U)),
            (int32_t)((sequence * 11U) % (c->height - 8U)), 8, 8 };
        paint(s->texture, r, false);
        s->previous_edit = r; s->have_previous = true;
        return grape_texture_invalidate_rect(s->texture,
            (uint32_t)r.x, (uint32_t)r.y, (uint32_t)r.width, (uint32_t)r.height);
    }
    if (c->op == ROTATE)
        return grape_surface_set_rotation(s->surfaces[0], 0.23f + (sequence % 32U) * 0.017f);
    if (c->op == PROCEDURAL) {
        generated_procedural_gradient_uniforms_t uniforms = {0};
        return grape_shader_render_procedural_to_texture(s->texture,
            &generated_procedural_gradient_program, &uniforms);
    }
    return grape_invalidate_all(runtime->grape);
}

static esp_err_t capture(grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bc, void *opaque, uint32_t frame)
{
    state_t *s = opaque;
    if (s->config->op == FILL_CPU || s->config->op == FILL_PPA) {
        esp_err_t ret = grape_benchmark_render_rects(runtime->grape, &s->rect, 1, true);
        if (ret != ESP_OK) return ret;
    }
    return grape_benchmark_capture_display(runtime, bc, opaque, frame);
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bc, void *opaque, grape_benchmark_metric_t *out, size_t cap)
{
    (void)runtime; (void)bc;
    state_t *s = opaque;
    if (cap < 2 || s->config->op != EDIT_ALPHA) return 0;
    grape_benchmark_occupancy_info_t info;
    if (grape_benchmark_texture_occupancy_info(s->texture, &info) != ESP_OK) return 0;
    out[0] = (grape_benchmark_metric_t){"occupied_cells_final", "cells", (double)info.occupied_cells};
    out[1] = (grape_benchmark_metric_t){"edited_pixels_steady", "px", 128};
    return 2;
}

static void teardown(grape_benchmark_runtime_t *runtime, const grape_benchmark_case_t *bc, void *s)
{ (void)bc; release(runtime, s); }

#define CASE(n, op_, fmt, w, h, layers_, scale_, filter_, flags_) \
    { .group="baseline2d", .name=n, .kind=GRAPE_BENCHMARK_KIND_SCENE, .flags=flags_, \
      .user_data=&(const config_t){op_, fmt, w, h, layers_, scale_, filter_}, \
      .setup=setup, .iteration=iteration, .teardown=teardown, .collect_metrics=metrics, \
      .capture_reference=capture, .params={{"width",w},{"height",h},{"layers",layers_},{"scale",scale_}} }
#define N GRAPE_TEXTURE_FILTER_NEAREST
#define P GRAPE_BENCHMARK_CASE_PRESENT
static const grape_benchmark_case_t cases[] = {
    CASE("rgb565_copy", DRAW, GRAPE_PIXEL_FORMAT_RGB565,320,240,1,1,N,P),
    CASE("rgb565_2x", DRAW, GRAPE_PIXEL_FORMAT_RGB565,320,240,1,2,N,P),
    CASE("rgba_2x", DRAW, GRAPE_PIXEL_FORMAT_RGBA8888,320,240,1,2,N,P),
    CASE("a8_blend", DRAW, GRAPE_PIXEL_FORMAT_A8,320,240,1,1,N,P),
    CASE("rgba_blend", DRAW, GRAPE_PIXEL_FORMAT_RGBA8888,320,240,1,1,N,P),
    CASE("opaque_overlap4", DRAW, GRAPE_PIXEL_FORMAT_RGB565,320,240,4,1,N,P),
    CASE("a8_tiny_edit", EDIT_ALPHA, GRAPE_PIXEL_FORMAT_A8,512,512,1,1,N,P),
    CASE("rgba_tiny_edit", EDIT_ALPHA, GRAPE_PIXEL_FORMAT_RGBA8888,512,512,1,1,N,P),
    CASE("a8_rotation", ROTATE, GRAPE_PIXEL_FORMAT_A8,240,160,1,1,N,P),
    CASE("rgba_linear_rotate", ROTATE, GRAPE_PIXEL_FORMAT_RGBA8888,240,160,1,1,GRAPE_TEXTURE_FILTER_LINEAR,P),
    CASE("brightness_chain2", CHAIN, GRAPE_PIXEL_FORMAT_RGBA8888,320,240,1,1,N,P),
    CASE("procedural_135_rows", PROCEDURAL, GRAPE_PIXEL_FORMAT_RGB565,240,135,1,1,N,P),
    CASE("cpu_fill_16x16", FILL_CPU, GRAPE_PIXEL_FORMAT_RGB565,16,16,0,1,N,0),
    CASE("ppa_fill_16x16", FILL_PPA, GRAPE_PIXEL_FORMAT_RGB565,16,16,0,1,N,0),
    CASE("cpu_fill_16x256", FILL_CPU, GRAPE_PIXEL_FORMAT_RGB565,16,256,0,1,N,0),
    CASE("ppa_fill_16x256", FILL_PPA, GRAPE_PIXEL_FORMAT_RGB565,16,256,0,1,N,0),
};
#undef N
#undef P
#undef CASE

const grape_benchmark_case_t *grape_benchmark_baseline_cases(size_t *count)
{
    if (count) *count = sizeof(cases)/sizeof(cases[0]);
    return cases;
}
