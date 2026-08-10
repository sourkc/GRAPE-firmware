#include "grape_benchmark_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SVG_OP_PATH_PARSE = 0,
    SVG_OP_DOCUMENT_CREATE,
} svg_operation_t;

typedef enum {
    SVG_PATH_LINE = 0,
    SVG_PATH_QUAD,
    SVG_PATH_CUBIC,
    SVG_PATH_ARC,
    SVG_PATH_MIXED,
} svg_path_shape_t;

typedef struct {
    svg_operation_t operation;
    svg_path_shape_t shape;
    uint32_t command_count;
    uint32_t layer_count;
    uint32_t raster_size;
    uint8_t samples_per_axis;
} svg_case_config_t;

typedef struct {
    char *text;
    size_t text_size;
    grape_path_t *path;
    grape_svg_document_t *document;
    size_t layer_count;
} svg_case_state_t;

static bool append_text(char *buffer,
                        size_t capacity,
                        size_t *offset,
                        const char *format,
                        ...)
{
    if (!buffer || !offset || *offset >= capacity) {
        return false;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(
        buffer + *offset,
        capacity - *offset,
        format,
        args
    );
    va_end(args);
    if (written < 0 || (size_t)written >= capacity - *offset) {
        return false;
    }

    *offset += (size_t)written;
    return true;
}

static char *make_path_data(svg_path_shape_t shape,
                            uint32_t command_count,
                            uint32_t layer,
                            size_t *out_size)
{
    size_t capacity = 128U + (size_t)command_count * 96U;
    char *buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }

    size_t offset = 0U;
    float layer_y = (float)(layer % 5U) * 1.5f;
    if (!append_text(buffer, capacity, &offset, "M0 %.2f ", 50.0f + layer_y)) {
        free(buffer);
        return NULL;
    }

    for (uint32_t i = 0; i < command_count; ++i) {
        float x0 = (float)i * 100.0f / (float)command_count;
        float x1 = (float)(i + 1U) * 100.0f / (float)command_count;
        float y0 = 16.0f + (float)((i * 37U + layer * 11U) % 68U);
        float y1 = 16.0f + (float)(((i + 1U) * 37U + layer * 11U) % 68U);
        float mid_x = (x0 + x1) * 0.5f;
        float sign = (i & 1U) ? -1.0f : 1.0f;
        svg_path_shape_t command_shape = shape == SVG_PATH_MIXED
            ? (svg_path_shape_t)(i % 4U)
            : shape;
        bool ok = false;

        switch (command_shape) {
            case SVG_PATH_LINE:
                ok = append_text(
                    buffer, capacity, &offset,
                    "L%.3f %.3f ", x1, y1
                );
                break;

            case SVG_PATH_QUAD:
                ok = append_text(
                    buffer, capacity, &offset,
                    "Q%.3f %.3f %.3f %.3f ",
                    mid_x, (y0 + y1) * 0.5f + sign * 18.0f,
                    x1, y1
                );
                break;

            case SVG_PATH_CUBIC: {
                float dx = (x1 - x0) / 3.0f;
                ok = append_text(
                    buffer, capacity, &offset,
                    "C%.3f %.3f %.3f %.3f %.3f %.3f ",
                    x0 + dx, y0 + sign * 22.0f,
                    x1 - dx, y1 - sign * 22.0f,
                    x1, y1
                );
                break;
            }

            case SVG_PATH_ARC:
                ok = append_text(
                    buffer, capacity, &offset,
                    "A%.3f %.3f %u 0 %u %.3f %.3f ",
                    fmaxf((x1 - x0) * 1.5f, 2.0f),
                    16.0f + (float)(i % 4U) * 3.0f,
                    (unsigned)((i * 13U) % 45U),
                    (unsigned)((i & 1U) == 0U),
                    x1, y1
                );
                break;

            case SVG_PATH_MIXED:
            default:
                break;
        }

        if (!ok) {
            free(buffer);
            return NULL;
        }
    }

    if (!append_text(buffer, capacity, &offset, "L100 96 L0 96 Z")) {
        free(buffer);
        return NULL;
    }

    if (out_size) {
        *out_size = offset;
    }
    return buffer;
}

static char *make_document(uint32_t layer_count, size_t *out_size)
{
    size_t capacity = 256U + (size_t)layer_count * 4096U;
    char *buffer = malloc(capacity);
    if (!buffer) {
        return NULL;
    }

    size_t offset = 0U;
    if (!append_text(
            buffer, capacity, &offset,
            "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 100 100\">")) {
        free(buffer);
        return NULL;
    }

    static const char *colors[] = {
        "#E2CDFF", "#89D66D", "#CE87FF", "#FFFFFF",
    };
    for (uint32_t layer = 0; layer < layer_count; ++layer) {
        size_t path_size = 0U;
        char *path = make_path_data(
            (svg_path_shape_t)(layer % 5U),
            12U + (layer % 4U) * 4U,
            layer,
            &path_size
        );
        if (!path) {
            free(buffer);
            return NULL;
        }

        bool ok = append_text(
            buffer, capacity, &offset,
            "<path fill=\"%s\" d=\"%s\"/>",
            colors[layer % (sizeof(colors) / sizeof(colors[0]))],
            path
        );
        free(path);
        if (!ok) {
            free(buffer);
            return NULL;
        }
    }

    if (!append_text(buffer, capacity, &offset, "</svg>")) {
        free(buffer);
        return NULL;
    }

    if (out_size) {
        *out_size = offset;
    }
    return buffer;
}

static void destroy_document(svg_case_state_t *state)
{
    if (!state || !state->document) {
        return;
    }
    grape_svg_document_destroy(state->document);
    state->document = NULL;
}

static void destroy_state(svg_case_state_t *state)
{
    if (!state) {
        return;
    }
    destroy_document(state);
    if (state->path) {
        grape_path_destroy(state->path);
    }
    free(state->text);
    free(state);
}

static esp_err_t setup(grape_benchmark_runtime_t *runtime,
                       const grape_benchmark_case_t *bench_case,
                       void **out_state)
{
    (void)runtime;
    const svg_case_config_t *config = bench_case->user_data;
    if (!config || !out_state) {
        return ESP_ERR_INVALID_ARG;
    }

    svg_case_state_t *state = calloc(1, sizeof(*state));
    if (!state) {
        return ESP_ERR_NO_MEM;
    }

    if (config->operation == SVG_OP_PATH_PARSE) {
        state->text = make_path_data(
            config->shape,
            config->command_count,
            0U,
            &state->text_size
        );
        if (!state->text) {
            destroy_state(state);
            return ESP_ERR_NO_MEM;
        }
        esp_err_t ret = grape_path_create(&state->path);
        if (ret != ESP_OK) {
            destroy_state(state);
            return ret;
        }
    } else {
        state->text = make_document(config->layer_count, &state->text_size);
        if (!state->text) {
            destroy_state(state);
            return ESP_ERR_NO_MEM;
        }
    }

    *out_state = state;
    return ESP_OK;
}

static esp_err_t iteration(grape_benchmark_runtime_t *runtime,
                           const grape_benchmark_case_t *bench_case,
                           void *opaque_state,
                           uint32_t sequence_iteration)
{
    (void)sequence_iteration;
    const svg_case_config_t *config = bench_case->user_data;
    svg_case_state_t *state = opaque_state;

    if (config->operation == SVG_OP_PATH_PARSE) {
        esp_err_t ret = grape_path_clear(state->path);
        return ret == ESP_OK
            ? grape_svg_parse_path_data(state->path, state->text)
            : ret;
    }

    grape_svg_document_config_t document_config =
        GRAPE_SVG_DOCUMENT_CONFIG_DEFAULT();
    document_config.width = (float)config->raster_size;
    document_config.height = (float)config->raster_size;
    document_config.samples_per_axis = config->samples_per_axis;
    document_config.padding_pixels = 2U;
    document_config.memory = GRAPE_MEMORY_PSRAM;

    esp_err_t ret = grape_svg_document_create(
        runtime->grape,
        state->text,
        &document_config,
        &state->document
    );
    if (ret == ESP_OK) {
        state->layer_count = grape_svg_document_layer_count(state->document);
    }
    return ret;
}

static void after_iteration(grape_benchmark_runtime_t *runtime,
                            const grape_benchmark_case_t *bench_case,
                            void *opaque_state,
                            uint32_t sequence_iteration)
{
    (void)runtime;
    (void)sequence_iteration;
    const svg_case_config_t *config = bench_case->user_data;
    if (config->operation == SVG_OP_DOCUMENT_CREATE) {
        destroy_document(opaque_state);
    }
}

static size_t metrics(grape_benchmark_runtime_t *runtime,
                      const grape_benchmark_case_t *bench_case,
                      void *opaque_state,
                      grape_benchmark_metric_t *out,
                      size_t capacity)
{
    (void)runtime;
    const svg_case_config_t *config = bench_case->user_data;
    svg_case_state_t *state = opaque_state;
    if (capacity < 3U) {
        return 0U;
    }

    out[0] = (grape_benchmark_metric_t){
        "input_bytes", "B", state->text_size,
    };
    out[1] = (grape_benchmark_metric_t){
        "commands", "", config->command_count,
    };
    out[2] = (grape_benchmark_metric_t){
        "layers", "", config->operation == SVG_OP_DOCUMENT_CREATE
            ? state->layer_count
            : 0U,
    };
    return 3U;
}

static void teardown(grape_benchmark_runtime_t *runtime,
                     const grape_benchmark_case_t *bench_case,
                     void *opaque_state)
{
    (void)runtime;
    (void)bench_case;
    destroy_state(opaque_state);
}

#define SVG_CASE_CAPACITY 32

static grape_benchmark_case_t s_cases[SVG_CASE_CAPACITY];
static svg_case_config_t s_configs[SVG_CASE_CAPACITY];
static char s_names[SVG_CASE_CAPACITY][64];
static size_t s_case_count;
static bool s_initialized;

static const char *shape_name(svg_path_shape_t shape)
{
    switch (shape) {
        case SVG_PATH_LINE: return "line";
        case SVG_PATH_QUAD: return "quad";
        case SVG_PATH_CUBIC: return "cubic";
        case SVG_PATH_ARC: return "arc";
        case SVG_PATH_MIXED: return "mixed";
        default: return "unknown";
    }
}

static void add_path_case(svg_path_shape_t shape, uint32_t command_count)
{
    if (s_case_count >= SVG_CASE_CAPACITY) {
        return;
    }

    size_t index = s_case_count++;
    s_configs[index] = (svg_case_config_t){
        .operation = SVG_OP_PATH_PARSE,
        .shape = shape,
        .command_count = command_count,
    };
    snprintf(
        s_names[index], sizeof(s_names[index]),
        "path_%s_n%u", shape_name(shape), (unsigned)command_count
    );
    s_cases[index] = (grape_benchmark_case_t){
        .group = "svg.path_parse",
        .name = s_names[index],
        .kind = GRAPE_BENCHMARK_KIND_MICRO,
        .user_data = &s_configs[index],
        .setup = setup,
        .iteration = iteration,
        .collect_metrics = metrics,
        .teardown = teardown,
        .params = {
            { "shape", shape },
            { "commands", command_count },
        },
    };
}

static void add_document_case(uint32_t layers,
                              uint32_t raster_size,
                              uint8_t samples_per_axis)
{
    if (s_case_count >= SVG_CASE_CAPACITY) {
        return;
    }

    size_t index = s_case_count++;
    s_configs[index] = (svg_case_config_t){
        .operation = SVG_OP_DOCUMENT_CREATE,
        .layer_count = layers,
        .raster_size = raster_size,
        .samples_per_axis = samples_per_axis,
    };
    snprintf(
        s_names[index], sizeof(s_names[index]),
        "document_l%u_s%u_aa%u",
        (unsigned)layers,
        (unsigned)raster_size,
        (unsigned)samples_per_axis
    );

    uint32_t measured = (layers >= 12U || raster_size >= 256U) ? 8U : 16U;
    s_cases[index] = (grape_benchmark_case_t){
        .group = "svg.document",
        .name = s_names[index],
        .kind = GRAPE_BENCHMARK_KIND_PIPELINE,
        .warmup_iterations = 3U,
        .measured_iterations = measured,
        .user_data = &s_configs[index],
        .setup = setup,
        .iteration = iteration,
        .after_iteration = after_iteration,
        .collect_metrics = metrics,
        .teardown = teardown,
        .params = {
            { "layers", layers },
            { "size", raster_size },
            { "aa", samples_per_axis },
        },
    };
}

const grape_benchmark_case_t *grape_benchmark_svg_cases(size_t *out_count)
{
    if (!s_initialized) {
        static const uint32_t command_counts[] = { 8U, 32U, 128U };
        for (svg_path_shape_t shape = SVG_PATH_LINE;
             shape <= SVG_PATH_MIXED;
             shape = (svg_path_shape_t)(shape + 1)) {
            for (size_t i = 0;
                 i < sizeof(command_counts) / sizeof(command_counts[0]);
                 ++i) {
                add_path_case(shape, command_counts[i]);
            }
        }

        static const uint32_t layers[] = { 1U, 4U, 12U };
        static const uint32_t sizes[] = { 128U, 256U };
        static const uint8_t aa[] = { 1U, 2U };
        for (size_t li = 0; li < sizeof(layers) / sizeof(layers[0]); ++li) {
            for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si) {
                for (size_t ai = 0; ai < sizeof(aa) / sizeof(aa[0]); ++ai) {
                    add_document_case(layers[li], sizes[si], aa[ai]);
                }
            }
        }

        s_initialized = true;
    }

    if (out_count) {
        *out_count = s_case_count;
    }
    return s_cases;
}
