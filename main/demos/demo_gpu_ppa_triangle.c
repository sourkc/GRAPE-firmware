#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "demo_internal.h"
#include "esp_log.h"
#include "esp_timer.h"

#define PROBE_SIZE 64U
#define PROBE_RUNS 8U

static esp_err_t probe_draw(grape_gpu_context_t *gpu, grape_texture_t *target,
                            grape_gpu_pipeline_t *pipeline, grape_gpu_buffer_t *vertices,
                            grape_gpu_sample_count_t samples)
{
    const grape_gpu_render_pass_desc_t pass = {
        .color_attachment = target, .color_load_op = GRAPE_GPU_LOAD_OP_CLEAR,
        .clear_color = {.r = 13, .g = 47, .b = 91, .a = 73},
        .sample_count = samples,
    };
    esp_err_t ret = grape_gpu_begin_render_pass(gpu, &pass);
    if (ret != ESP_OK) return ret;
    ret = grape_gpu_bind_pipeline(gpu, pipeline);
    if (ret == ESP_OK) ret = grape_gpu_bind_vertex_buffer(gpu, vertices);
    if (ret == ESP_OK) ret = grape_gpu_draw(gpu, 0, 12);
    const esp_err_t end = grape_gpu_end_render_pass(gpu);
    return ret == ESP_OK ? end : ret;
}


static esp_err_t probe_measure(grape_context_t *grape, grape_gpu_context_t *gpu,
    grape_texture_t *target, grape_gpu_pipeline_t *pipeline, grape_gpu_buffer_t *vertices,
    uint8_t *reference, unsigned size, const char *name, bool cpu, bool validate,
    bool batch, bool overlap, int64_t *cpu_us)
{
    const char *tag = "PPA_TRIANGLE_AB";
    esp_err_t ret = grape_feature_set_mode(grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES,
        cpu ? GRAPE_FEATURE_MODE_DISABLED : GRAPE_FEATURE_MODE_ENABLED);
    if (ret != ESP_OK) return ret;
    ret = grape_feature_set_mode(grape, GRAPE_FEATURE_PPA_TRIANGLE_VALIDATE,
        validate ? GRAPE_FEATURE_MODE_ENABLED : GRAPE_FEATURE_MODE_DISABLED);
    if (ret != ESP_OK) return ret;
    ret = grape_feature_set_mode(grape, GRAPE_FEATURE_PPA_TRIANGLE_BATCH,
        batch ? GRAPE_FEATURE_MODE_ENABLED : GRAPE_FEATURE_MODE_DISABLED);
    if (ret != ESP_OK) return ret;
    ret = grape_feature_set_mode(grape, GRAPE_FEATURE_PPA_TRIANGLE_OVERLAP,
        overlap ? GRAPE_FEATURE_MODE_ENABLED : GRAPE_FEATURE_MODE_DISABLED);
    if (ret != ESP_OK) return ret;
    /* Warm both paths, initialize hardware, and allocate render storage outside
     * the timing. Snapshot after warmup so all reported counters cover 8 passes. */
    ret = probe_draw(gpu, target, pipeline, vertices, GRAPE_GPU_SAMPLE_COUNT_1);
    if (ret != ESP_OK) return ret;
    grape_gpu_ppa_triangle_stats_t before, after;
    grape_gpu_get_ppa_triangle_stats(gpu, &before);
    int64_t started = esp_timer_get_time();
    for (unsigned run = 0; run < PROBE_RUNS; ++run) {
        ret = probe_draw(gpu, target, pipeline, vertices, GRAPE_GPU_SAMPLE_COUNT_1);
        if (ret != ESP_OK) return ret;
    }
    int64_t elapsed = esp_timer_get_time() - started;
    grape_gpu_get_ppa_triangle_stats(gpu, &after);
    const size_t stride = grape_texture_stride(target);
    for (unsigned y = 0; y < size; ++y) {
        const uint8_t *row = (const uint8_t *)grape_texture_pixels_const(target) + y * stride;
        if (cpu) memcpy(reference + y * size * 4, row, size * 4);
        else if (memcmp(reference + y * size * 4, row, size * 4)) {
            ESP_LOGE(tag, "%s %ux%u: RGBA mismatch on row %u", name, size, size, y);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (cpu) *cpu_us = elapsed;
    ESP_LOGI(tag, "%ux%u %s: 8 passes=%" PRId64 " us, CPU/feature=%.3fx, RGBA=PASS",
             size, size, name, elapsed, (double)*cpu_us / (double)(elapsed ? elapsed : 1));
    ESP_LOGI(tag, "%s: hardware=%" PRIu64 " blocks=%" PRIu64 " large=%" PRIu64
        " pixels=%" PRIu64 " validated=%" PRIu64 " coefficients=%" PRIu64,
        name, after.triangles_completed - before.triangles_completed,
        after.tiles_computed - before.tiles_computed,
        after.large_tiles_computed - before.large_tiles_computed,
        after.pixels_computed - before.pixels_computed,
        after.pixels_validated - before.pixels_validated,
        after.fallback_coefficients - before.fallback_coefficients);
    ESP_LOGI(tag, "%s: cpu_blocks=%" PRIu64 " cpu_pixels=%" PRIu64 " mixed_triangles=%" PRIu64,
        name, after.cpu_tiles_computed - before.cpu_tiles_computed,
        after.cpu_pixels_computed - before.cpu_pixels_computed,
        after.overlap_triangles - before.overlap_triangles);
    ESP_LOGI(tag, "%s us: prepare=%" PRIu64 " driver=%" PRIu64 " wait=%" PRIu64
        " cpu_render=%" PRIu64 " validate=%" PRIu64 " commit=%" PRIu64,
        name, after.prepare_us - before.prepare_us, after.driver_us - before.driver_us,
        after.wait_us - before.wait_us, after.cpu_render_us - before.cpu_render_us,
        after.validation_us - before.validation_us, after.commit_us - before.commit_us);
    if (overlap && size >= 256 && after.cpu_tiles_computed == before.cpu_tiles_computed)
        ESP_LOGW(tag, "%s: no CPU blocks claimed during waits; overlap not exercised", name);
    if (!cpu && (after.triangles_completed == before.triangles_completed || after.errors ||
                 after.validation_mismatches || (!validate && after.pixels_validated != before.pixels_validated)))
        return ESP_FAIL;
    if (batch && size >= 256 && after.large_tiles_computed == before.large_tiles_computed)
        return ESP_FAIL; /* Do not report a batching result that never batched. */
    return ESP_OK;
}

static esp_err_t probe_scene(grape_context_t *grape, unsigned size)
{
    const char *tag = "PPA_TRIANGLE_AB";
    grape_gpu_context_t *gpu = NULL;
    grape_texture_t *target = NULL;
    grape_surface_t *surface = NULL;
    grape_gpu_buffer_t *vertices = NULL;
    grape_gpu_pipeline_t *pipeline = NULL;
    grape_gpu_pipeline_t *msaa_pipeline = NULL;
    uint8_t *reference = NULL;
    grape_feature_mode_t previous;
    esp_err_t ret = grape_feature_get_mode(grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES, &previous);
    if (ret != ESP_OK) return ret;
    const grape_texture_desc_t texture = {
        .width = size, .height = size,
        .format = GRAPE_PIXEL_FORMAT_RGBA8888, .memory = GRAPE_MEMORY_DEFAULT,
    };
    if ((ret = grape_texture_create(grape, &texture, &target)) != ESP_OK) goto done;
    if ((ret = grape_gpu_context_create(grape, &gpu)) != ESP_OK) goto done;
    /* Shared diagonal, fractional vertices, and a viewport-clipped triangle. */
    const float screen[12][2] = {
        {4,4}, {28,4}, {4,28}, {28,4}, {28,28}, {4,28},
        {31.125f,8.375f}, {61.75f,10.125f}, {45.875f,59.625f},
        {-4,48}, {24,40}, {16,68},
    };
    float clip[12][2];
    for (unsigned i = 0; i < 12; ++i) {
        clip[i][0] = screen[i][0] / 32.0f - 1.0f;
        clip[i][1] = 1.0f - screen[i][1] / 32.0f;
    }
    const grape_gpu_buffer_desc_t buffer = {
        .size = sizeof(clip), .usage = GRAPE_GPU_BUFFER_VERTEX, .memory = GRAPE_MEMORY_DEFAULT,
    };
    if ((ret = grape_gpu_buffer_create(gpu, &buffer, &vertices)) != ESP_OK) goto done;
    if ((ret = grape_gpu_buffer_write(vertices, 0, clip, sizeof(clip))) != ESP_OK) goto done;
    const grape_gpu_pipeline_desc_t desc = {
        .vertex_layout = {.stride = sizeof(clip[0]), .attribute_count = 1,
            .attributes = {{.location = 0, .format = GRAPE_GPU_VERTEX_FORMAT_F32X2, .offset = 0}}},
        .topology = GRAPE_GPU_TOPOLOGY_TRIANGLE_LIST,
        .vertex_program = GRAPE_GPU_VERTEX_PROGRAM_CLIP_SPACE,
        .fragment_program = GRAPE_GPU_FRAGMENT_PROGRAM_SOLID_COLOR,
        .sample_count = GRAPE_GPU_SAMPLE_COUNT_1,
        .solid_color = {.r = 231, .g = 107, .b = 53, .a = 255},
    };
    if ((ret = grape_gpu_pipeline_create(gpu, &desc, &pipeline)) != ESP_OK) goto done;
    /* Sample count belongs to both the pipeline and the render pass. The API
     * rejects binding the 1x pipeline to either of the 4x fallback passes. */
    grape_gpu_pipeline_desc_t msaa_desc = desc;
    msaa_desc.sample_count = GRAPE_GPU_SAMPLE_COUNT_4;
    if ((ret = grape_gpu_pipeline_create(gpu, &msaa_desc, &msaa_pipeline)) != ESP_OK) goto done;
    reference = malloc(size * size * 4);
    if (!reference) { ret = ESP_ERR_NO_MEM; goto done; }
    const size_t stride = grape_texture_stride(target);

    int64_t cpu_us = 0;
    if ((ret = probe_measure(grape, gpu, target, pipeline, vertices, reference, size,
                            "CPU", true, false, false, false, &cpu_us)) != ESP_OK) goto done;
#if CONFIG_GRAPE_PPA_TRIANGLE_VALIDATE
    if ((ret = probe_measure(grape, gpu, target, pipeline, vertices, reference, size,
                            "checked16", false, true, false, false, &cpu_us)) != ESP_OK) goto done;
#endif
    if ((ret = probe_measure(grape, gpu, target, pipeline, vertices, reference, size,
                            "fast16", false, false, false, false, &cpu_us)) != ESP_OK) goto done;
#if CONFIG_GRAPE_PPA_TRIANGLE_VALIDATE
    if ((ret = probe_measure(grape, gpu, target, pipeline, vertices, reference, size,
                            "checked64", false, true, true, false, &cpu_us)) != ESP_OK) goto done;
#endif
    if ((ret = probe_measure(grape, gpu, target, pipeline, vertices, reference, size,
                            "fast64", false, false, true, false, &cpu_us)) != ESP_OK) goto done;
#if CONFIG_GRAPE_PPA_TRIANGLE_VALIDATE
    if ((ret = probe_measure(grape, gpu, target, pipeline, vertices, reference, size,
                            "overlap_checked64", false, true, true, true, &cpu_us)) != ESP_OK) goto done;
#endif
    if ((ret = probe_measure(grape, gpu, target, pipeline, vertices, reference, size,
                            "overlap_fast16", false, false, false, true, &cpu_us)) != ESP_OK) goto done;
    if ((ret = probe_measure(grape, gpu, target, pipeline, vertices, reference, size,
                            "overlap_fast64", false, false, true, true, &cpu_us)) != ESP_OK) goto done;
    grape_gpu_ppa_triangle_stats_t stats;
    grape_gpu_get_ppa_triangle_stats(gpu, &stats);
    /* Exercise runtime disabling again and the retained multicore/MSAA path. */
    ESP_LOGI(tag, "Checking 4x CPU reference and feature fallback");
    const uint64_t completed = stats.triangles_completed, cpu_blocks = stats.cpu_tiles_computed;
    if ((ret = grape_feature_disable(grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES)) != ESP_OK) goto done;
    if ((ret = probe_draw(gpu, target, msaa_pipeline, vertices, GRAPE_GPU_SAMPLE_COUNT_4)) != ESP_OK) goto done;
    for (unsigned y = 0; y < size; ++y)
        memcpy(reference + y * size * 4,
               (const uint8_t *)grape_texture_pixels_const(target) + y * stride, size * 4);
    if ((ret = grape_feature_enable(grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES)) != ESP_OK) goto done;
    if ((ret = probe_draw(gpu, target, msaa_pipeline, vertices, GRAPE_GPU_SAMPLE_COUNT_4)) != ESP_OK) goto done;
    for (unsigned y = 0; y < size; ++y)
        if (memcmp(reference + y * size * 4,
                   (const uint8_t *)grape_texture_pixels_const(target) + y * stride, size * 4)) {
            ret = ESP_ERR_INVALID_RESPONSE;
            goto done;
        }
    grape_gpu_get_ppa_triangle_stats(gpu, &stats);
    if (stats.triangles_completed != completed || stats.cpu_tiles_computed != cpu_blocks || !stats.fallback_unsupported) {
        ret = ESP_FAIL;
        goto done;
    }
    ESP_LOGI(tag, "PASS: hardware 1x matches CPU RGBA; feature toggle and 4x fallback match");
    if (size != PROBE_SIZE) goto done;
    if ((ret = grape_surface_create(grape, &GRAPE_SURFACE_DESC_TEXTURE(target), &surface)) != ESP_OK) goto done;
    grape_transform_t transform = GRAPE_TRANSFORM_DEFAULT();
    transform.scale_x = transform.scale_y = 4;
    transform.x = transform.y = 24;
    if ((ret = grape_surface_set_transform(surface, &transform)) == ESP_OK) ret = grape_present(grape);
done:
    /* Restore the caller's mode; hardware failure still leaves availability false. */
    grape_feature_set_mode(grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES, previous);
    free(reference);
    if (msaa_pipeline) grape_gpu_pipeline_destroy(msaa_pipeline);
    if (pipeline) grape_gpu_pipeline_destroy(pipeline);
    if (vertices) grape_gpu_buffer_destroy(vertices);
    if (gpu) grape_gpu_context_destroy(gpu);
    if (surface) grape_surface_destroy(surface);
    if (target) grape_texture_destroy(target);
    if (ret != ESP_OK) ESP_LOGE(tag, "FAIL: %s", esp_err_to_name(ret));
    return ret;
}

esp_err_t grape_demo_gpu_ppa_triangle_run(grape_context_t *grape)
{
    const grape_feature_id_t ids[] = {GRAPE_FEATURE_PPA_TRIANGLE_EDGES,
        GRAPE_FEATURE_PPA_TRIANGLE_VALIDATE, GRAPE_FEATURE_PPA_TRIANGLE_BATCH, GRAPE_FEATURE_PPA_TRIANGLE_OVERLAP};
    grape_feature_mode_t modes[4];
    for (unsigned i = 0; i < 4; ++i) {
        esp_err_t ret = grape_feature_get_mode(grape, ids[i], &modes[i]);
        if (ret != ESP_OK) return ret;
    }
    esp_err_t ret = probe_scene(grape, PROBE_SIZE);
    if (ret == ESP_OK) ret = probe_scene(grape, 256);
    for (unsigned i = 0; i < 4; ++i) grape_feature_set_mode(grape, ids[i], modes[i]);
    if (ret == ESP_OK) ESP_LOGI("PPA_TRIANGLE_AB", "PASS: both scene sizes, all timing modes, and 4x fallback match");
    return ret;
}
