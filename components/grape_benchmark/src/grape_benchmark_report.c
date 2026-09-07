#include "grape_benchmark_internal.h"
#include "sdkconfig.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "grape_storage_sd.h"

static const char *TAG = "grape_bench";

static const char *kind_name(grape_benchmark_kind_t kind)
{
    switch (kind) {
        case GRAPE_BENCHMARK_KIND_MICRO: return "micro";
        case GRAPE_BENCHMARK_KIND_PIPELINE: return "pipeline";
        case GRAPE_BENCHMARK_KIND_SCENE: return "scene";
        case GRAPE_BENCHMARK_KIND_LIFECYCLE: return "lifecycle";
        default: return "unknown";
    }
}

static int compare_function_profile_entries(const void *a, const void *b)
{
    const grape_benchmark_function_profile_entry_t *lhs = a;
    const grape_benchmark_function_profile_entry_t *rhs = b;
    if (lhs->calls < rhs->calls) {
        return 1;
    }
    if (lhs->calls > rhs->calls) {
        return -1;
    }
    return (lhs->function_address > rhs->function_address) -
           (lhs->function_address < rhs->function_address);
}

static bool make_path(char *out, size_t capacity,
                      const char *directory, const char *name)
{
    if (!out || capacity == 0 || !directory || !name) {
        return false;
    }
    int written = snprintf(out, capacity, "%s/%s", directory, name);
    return written > 0 && (size_t)written < capacity;
}

static void report_set_error(grape_benchmark_runtime_t *runtime, esp_err_t error)
{
    if (runtime && runtime->report_error == ESP_OK && error != ESP_OK) {
        runtime->report_error = error;
    }
}

static esp_err_t buffer_reserve(grape_benchmark_text_buffer_t *buffer,
                                size_t required)
{
    if (!buffer || !buffer->enabled) {
        return ESP_OK;
    }
    if (required <= buffer->capacity) {
        return ESP_OK;
    }

    size_t capacity = buffer->capacity ? buffer->capacity : 1024U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = required;
            break;
        }
        capacity *= 2U;
    }

    char *data = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        return ESP_ERR_NO_MEM;
    }
    if (buffer->data && buffer->size > 0) {
        memcpy(data, buffer->data, buffer->size);
    }
    heap_caps_free(buffer->data);
    buffer->data = data;
    buffer->capacity = capacity;
    if (buffer->size < buffer->capacity) {
        buffer->data[buffer->size] = '\0';
    }
    return ESP_OK;
}

static esp_err_t buffer_init(grape_benchmark_text_buffer_t *buffer,
                             bool enabled,
                             size_t initial_capacity)
{
    if (!buffer) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(buffer, 0, sizeof(*buffer));
    buffer->enabled = enabled;
    if (!enabled) {
        return ESP_OK;
    }
    return buffer_reserve(buffer, initial_capacity > 0 ? initial_capacity : 1U);
}

static void buffer_free(grape_benchmark_text_buffer_t *buffer)
{
    if (!buffer) {
        return;
    }
    heap_caps_free(buffer->data);
    memset(buffer, 0, sizeof(*buffer));
}

static esp_err_t buffer_vprintf(grape_benchmark_text_buffer_t *buffer,
                                const char *format,
                                va_list args)
{
    if (!buffer || !buffer->enabled) {
        return ESP_OK;
    }
    if (!format) {
        return ESP_ERR_INVALID_ARG;
    }

    va_list size_args;
    va_copy(size_args, args);
    int needed = vsnprintf(NULL, 0, format, size_args);
    va_end(size_args);
    if (needed < 0) {
        return ESP_FAIL;
    }

    size_t required = buffer->size + (size_t)needed + 1U;
    esp_err_t ret = buffer_reserve(buffer, required);
    if (ret != ESP_OK) {
        return ret;
    }

    int written = vsnprintf(
        buffer->data + buffer->size,
        buffer->capacity - buffer->size,
        format,
        args
    );
    if (written != needed) {
        return ESP_FAIL;
    }
    buffer->size += (size_t)written;
    return ESP_OK;
}

static void report_printf(grape_benchmark_runtime_t *runtime,
                          grape_benchmark_text_buffer_t *buffer,
                          const char *format,
                          ...)
{
    if (!runtime || runtime->report_error != ESP_OK ||
        !buffer || !buffer->enabled) {
        return;
    }

    va_list args;
    va_start(args, format);
    esp_err_t ret = buffer_vprintf(buffer, format, args);
    va_end(args);
    report_set_error(runtime, ret);
}

static void write_distribution_header(grape_benchmark_runtime_t *runtime,
                                      grape_benchmark_text_buffer_t *buffer,
                                      const char *prefix)
{
    report_printf(runtime, buffer,
                  ",%s_mean_us,%s_stddev_us,%s_min_us,%s_p50_us,%s_p95_us,%s_p99_us,%s_max_us",
                  prefix, prefix, prefix, prefix, prefix, prefix, prefix);
}

static void write_distribution(grape_benchmark_runtime_t *runtime,
                               grape_benchmark_text_buffer_t *buffer,
                               const grape_benchmark_distribution_t *dist)
{
    report_printf(runtime, buffer,
                  ",%.3f,%.3f,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32,
                  dist->mean_us,
                  dist->stddev_us,
                  dist->min_us,
                  dist->p50_us,
                  dist->p95_us,
                  dist->p99_us,
                  dist->max_us);
}

static void write_summary_header(grape_benchmark_runtime_t *runtime)
{
    grape_benchmark_text_buffer_t *buffer = &runtime->summary_buffer;
    report_printf(runtime, buffer, "status,kind,group,name");
    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_PARAMS; ++i) {
        report_printf(runtime, buffer, ",param%u_name,param%u_value",
                      (unsigned)i, (unsigned)i);
    }
    report_printf(runtime, buffer, ",iterations,elapsed_us,iterations_per_second");
    write_distribution_header(runtime, buffer, "work");
    write_distribution_header(runtime, buffer, "present");
    write_distribution_header(runtime, buffer, "total");
    write_distribution_header(runtime, buffer, "refresh_wait");

    for (int i = 0; i < GRAPE_TELEMETRY_TIMER_COUNT; ++i) {
        const char *name = grape_telemetry_timer_csv_name((grape_telemetry_timer_t)i);
        report_printf(runtime, buffer, ",%s_total_us,%s_avg_us,%s_max_us,%s_calls",
                      name, name, name, name);
    }

    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_METRICS; ++i) {
        report_printf(runtime, buffer, ",metric%u_name,metric%u_unit,metric%u_value",
                      (unsigned)i, (unsigned)i, (unsigned)i);
    }
    report_printf(runtime, buffer, "\n");
}

static void write_samples_header(grape_benchmark_runtime_t *runtime)
{
    grape_benchmark_text_buffer_t *buffer = &runtime->samples_buffer;
    report_printf(runtime, buffer, "kind,group,name");
    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_PARAMS; ++i) {
        report_printf(runtime, buffer, ",param%u_name,param%u_value",
                      (unsigned)i, (unsigned)i);
    }
    report_printf(runtime, buffer,
                  ",iteration,work_us,present_us,total_us,refresh_wait_us\n");
}

esp_err_t grape_benchmark_report_open(grape_benchmark_runtime_t *runtime)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }

    runtime->report_error = ESP_OK;
    esp_err_t ret = buffer_init(
        &runtime->summary_buffer,
        runtime->config.write_summary_csv,
        64U * 1024U
    );
    if (ret != ESP_OK) {
        return ret;
    }
    ret = buffer_init(
        &runtime->samples_buffer,
        runtime->config.write_samples_csv,
        256U * 1024U
    );
    if (ret != ESP_OK) {
        grape_benchmark_report_close(runtime);
        return ret;
    }
    ret = buffer_init(&runtime->metadata_buffer, true, 4096U);
    if (ret != ESP_OK) {
        grape_benchmark_report_close(runtime);
        return ret;
    }
    ret = buffer_init(
        &runtime->function_profile_buffer,
        grape_benchmark_function_profile_enabled(),
        128U * 1024U
    );
    if (ret != ESP_OK) {
        grape_benchmark_report_close(runtime);
        return ret;
    }

    ret = buffer_init(&runtime->references_buffer, true, 8192U);
    if (ret != ESP_OK) { grape_benchmark_report_close(runtime); return ret; }
    report_printf(runtime, &runtime->references_buffer,
        "group,name,frame,plane,format,width,height,samples,bytes,crc32,file\n");
    write_summary_header(runtime);
    write_samples_header(runtime);
    report_printf(
        runtime,
        &runtime->function_profile_buffer,
        "kind,group,name,iterations,address,calls,calls_per_iteration,table_overflow\n"
    );
    grape_benchmark_report_metadata(runtime);
    if (runtime->report_error != ESP_OK) {
        ret = runtime->report_error;
        grape_benchmark_report_close(runtime);
        return ret;
    }

    ESP_LOGI(TAG, "Benchmark reports buffered in PSRAM until the run completes");
    return ESP_OK;
}

void grape_benchmark_report_close(grape_benchmark_runtime_t *runtime)
{
    if (!runtime) {
        return;
    }
    buffer_free(&runtime->summary_buffer);
    buffer_free(&runtime->samples_buffer);
    buffer_free(&runtime->metadata_buffer);
    buffer_free(&runtime->function_profile_buffer);
    buffer_free(&runtime->references_buffer);
}

void grape_benchmark_report_metadata(grape_benchmark_runtime_t *runtime)
{
    if (!runtime) {
        return;
    }

    grape_benchmark_text_buffer_t *buffer = &runtime->metadata_buffer;
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);

    report_printf(runtime, buffer, "benchmark_build=%s\n", GRAPE_BENCHMARK_BUILD_LABEL);
    report_printf(runtime, buffer, "case_selection=%s\n",
                  GRAPE_BENCHMARK_REGRESSION_FOCUS ? "regressions-v1" : "all");
    report_printf(runtime, buffer, "schema_version=5\nrun_label=%s\ncompiler=%s\n",
                  GRAPE_BENCHMARK_RUN_LABEL, __VERSION__);
    const esp_app_desc_t *app = esp_app_get_description();
    report_printf(runtime, buffer, "app_version=%s\napp_build_date=%s\napp_build_time=%s\nelf_sha256=",
                  app->version, app->date, app->time);
    for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i)
        report_printf(runtime, buffer, "%02x", app->app_elf_sha256[i]);
    report_printf(runtime, buffer, "\n");
#if CONFIG_COMPILER_OPTIMIZATION_PERF
    report_printf(runtime, buffer, "optimization_config=performance_O2\n");
#elif CONFIG_COMPILER_OPTIMIZATION_DEBUG
    report_printf(runtime, buffer, "optimization_config=debug_Og\n");
#elif CONFIG_COMPILER_OPTIMIZATION_NONE
    report_printf(runtime, buffer, "optimization_config=none_O0\n");
#else
    report_printf(runtime, buffer, "optimization_config=size_or_other\n");
#endif
#ifdef CONFIG_SPIRAM_SPEED
    report_printf(runtime, buffer, "psram_frequency_mhz=%u\n", (unsigned)CONFIG_SPIRAM_SPEED);
#endif
#ifdef CONFIG_CACHE_L2_CACHE_SIZE
    report_printf(runtime, buffer, "l2_cache_bytes=%u\n", (unsigned)CONFIG_CACHE_L2_CACHE_SIZE);
#endif
#if CONFIG_GRAPE_GPU_MULTICORE && !CONFIG_FREERTOS_UNICORE && !CONFIG_GRAPE_FUNCTION_PROFILING
    report_printf(runtime, buffer, "gpu_multicore_build=1\n");
#else
    report_printf(runtime, buffer, "gpu_multicore_build=0\n");
#endif
    report_printf(runtime, buffer,
        "profile=%u\ngpu_stats=%u\nrefresh_wait_available=%u\n"
        "reference_frames=%u\nservice_interval_us=%u\nservice_delay_ticks=1\n"
        "references_complete=0\n",
        (unsigned)GRAPE_BENCHMARK_PROFILE, (unsigned)GRAPE_BENCHMARK_GPU_STATS,
        GRAPE_TELEMETRY_LEVEL >= 2 ? 1U : 0U,
        (unsigned)GRAPE_BENCHMARK_REFERENCE_FRAMES,
        (unsigned)GRAPE_BENCHMARK_SERVICE_INTERVAL_US);
    report_printf(runtime, buffer, "idf_version=%s\n", esp_get_idf_version());
    report_printf(runtime, buffer, "idf_target=%s\n", CONFIG_IDF_TARGET);
    report_printf(runtime, buffer, "chip_model=%d\n", (int)chip.model);
    report_printf(runtime, buffer, "chip_revision=%u\n", chip.revision);
    report_printf(runtime, buffer, "chip_cores=%u\n", chip.cores);
    report_printf(runtime, buffer, "ppa_fill_available=%u\nppa_blend_available=%u\n",
        grape_feature_is_available(runtime->grape, GRAPE_FEATURE_PPA_FILL) ? 1U : 0U,
        grape_feature_is_available(runtime->grape, GRAPE_FEATURE_PPA_A8_BLEND) ? 1U : 0U);
    report_printf(runtime, buffer, "cpu_frequency_mhz=%d\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    report_printf(runtime, buffer, "psram_total_bytes=%u\n",
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    report_printf(runtime, buffer, "psram_free_bytes=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (display) {
        report_printf(runtime, buffer, "display_name=%s\n",
                      display->name ? display->name : "unknown");
        report_printf(runtime, buffer, "display_width=%" PRIu32 "\n", display->width);
        report_printf(runtime, buffer, "display_height=%" PRIu32 "\n", display->height);
        report_printf(runtime, buffer, "display_format=%d\n", (int)display->format);
    }

    report_printf(runtime, buffer, "telemetry_level=%d\n", GRAPE_TELEMETRY_LEVEL);
    report_printf(runtime, buffer, "function_profiling=%d\n",
                  grape_benchmark_function_profile_enabled() ? 1 : 0);
    report_printf(runtime, buffer, "function_profile_capacity=%u\n",
                  (unsigned)grape_benchmark_function_profile_capacity());
    report_printf(runtime, buffer, "warmup_iterations=%" PRIu32 "\n",
                  runtime->config.warmup_iterations);
    report_printf(runtime, buffer, "measured_iterations=%" PRIu32 "\n",
                  runtime->config.measured_iterations);
    report_printf(runtime, buffer, "fixed_dt_us=%" PRIu32 "\n", runtime->config.fixed_dt_us);
    report_printf(runtime, buffer, "seed=0x%08" PRIx32 "\n", runtime->config.seed);
    report_printf(runtime, buffer, "suite_mask=0x%08" PRIx32 "\n", runtime->config.suite_mask);
}

void grape_benchmark_report_function_profile(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    uint32_t measured_iterations
)
{
    if (!runtime || !bench_case ||
        !runtime->function_profile_buffer.enabled) {
        return;
    }

    size_t capacity = grape_benchmark_function_profile_capacity();
    if (capacity == 0U) {
        return;
    }

    grape_benchmark_function_profile_entry_t *entries = heap_caps_malloc(
        capacity * sizeof(*entries),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );
    if (!entries) {
        report_set_error(runtime, ESP_ERR_NO_MEM);
        return;
    }

    bool overflow = false;
    size_t count = grape_benchmark_function_profile_snapshot(
        entries,
        capacity,
        &overflow
    );
    if (count > capacity) {
        count = capacity;
        overflow = true;
    }

    qsort(entries, count, sizeof(*entries), compare_function_profile_entries);

    for (size_t i = 0; i < count; ++i) {
        double calls_per_iteration = measured_iterations > 0U
            ? (double)entries[i].calls / (double)measured_iterations
            : 0.0;
        report_printf(
            runtime,
            &runtime->function_profile_buffer,
            "%s,%s,%s,%" PRIu32 ",0x%" PRIxPTR ",%" PRIu64 ",%.6f,%u\n",
            kind_name(bench_case->kind),
            bench_case->group,
            bench_case->name,
            measured_iterations,
            entries[i].function_address,
            entries[i].calls,
            calls_per_iteration,
            overflow ? 1U : 0U
        );
    }

    if (overflow) {
        ESP_LOGW(
            TAG,
            "Function profile table overflowed for %s/%s; increase CONFIG_GRAPE_FUNCTION_PROFILE_MAX_FUNCTIONS",
            bench_case->group,
            bench_case->name
        );
    }

    heap_caps_free(entries);
}

void grape_benchmark_report_skip(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    esp_err_t reason
)
{
    if (!runtime || !bench_case) {
        return;
    }
    (void)reason;

    ESP_LOGW(TAG, "SKIP %-14s %-26s (%s)",
             bench_case->group, bench_case->name, esp_err_to_name(reason));

    grape_benchmark_text_buffer_t *buffer = &runtime->summary_buffer;
    report_printf(runtime, buffer, "skip,%s,%s,%s",
                  kind_name(bench_case->kind), bench_case->group, bench_case->name);
    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_PARAMS; ++i) {
        const grape_benchmark_param_t *param = &bench_case->params[i];
        report_printf(runtime, buffer, ",%s,%.9g",
                      param->name ? param->name : "",
                      param->name ? param->value : 0.0);
    }

    grape_benchmark_distribution_t zero_dist = {0};
    report_printf(runtime, buffer, ",0,0,0");
    write_distribution(runtime, buffer, &zero_dist);
    write_distribution(runtime, buffer, &zero_dist);
    write_distribution(runtime, buffer, &zero_dist);
    write_distribution(runtime, buffer, &zero_dist);

    for (int i = 0; i < GRAPE_TELEMETRY_TIMER_COUNT; ++i) {
        report_printf(runtime, buffer, ",0,0,0,0");
    }
    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_METRICS; ++i) {
        report_printf(runtime, buffer, ",,,0");
    }
    report_printf(runtime, buffer, "\n");
}

void grape_benchmark_report_case(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    const grape_benchmark_result_t *result,
    const grape_benchmark_sample_t *samples
)
{
    if (!runtime || !bench_case || !result || result->iterations == 0) {
        return;
    }

    double ips = result->elapsed_us
        ? ((double)result->iterations * 1000000.0) / (double)result->elapsed_us
        : 0.0;

    if (runtime->config.log_each_case) {
        ESP_LOGI(TAG,
                 "%-9s %-14s %-26s work=%7.3f ms p50=%7.3f p95=%7.3f p99=%7.3f max=%7.3f ms",
                 kind_name(bench_case->kind),
                 bench_case->group,
                 bench_case->name,
                 result->work.mean_us / 1000.0,
                 result->work.p50_us / 1000.0,
                 result->work.p95_us / 1000.0,
                 result->work.p99_us / 1000.0,
                 result->work.max_us / 1000.0);

        if ((bench_case->flags & GRAPE_BENCHMARK_CASE_PRESENT) != 0U ||
            result->telemetry.timers[GRAPE_TELEMETRY_TIMER_DISPLAY_REFRESH_WAIT].calls) {
            ESP_LOGI(TAG,
                     "  total=%7.3f ms present=%7.3f ms refresh_wait=%7.3f ms p50=%7.3f p95=%7.3f p99=%7.3f",
                     result->total.mean_us / 1000.0,
                     result->present.mean_us / 1000.0,
                     result->refresh_wait.mean_us / 1000.0,
                     result->refresh_wait.p50_us / 1000.0,
                     result->refresh_wait.p95_us / 1000.0,
                     result->refresh_wait.p99_us / 1000.0);
        }

        if (result->metric_count > 0) {
            char line[384];
            size_t used = (size_t)snprintf(line, sizeof(line), "  metrics:");
            for (size_t i = 0; i < result->metric_count && used < sizeof(line); ++i) {
                int written = snprintf(line + used, sizeof(line) - used,
                                       " %s=%.4g%s",
                                       result->metrics[i].name,
                                       result->metrics[i].value,
                                       result->metrics[i].unit ? result->metrics[i].unit : "");
                if (written < 0) break;
                used += (size_t)written;
            }
            ESP_LOGI(TAG, "%s", line);
        }
    }

    grape_benchmark_text_buffer_t *summary = &runtime->summary_buffer;
    report_printf(runtime, summary, "ok,%s,%s,%s",
                  kind_name(bench_case->kind), bench_case->group, bench_case->name);
    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_PARAMS; ++i) {
        const grape_benchmark_param_t *param = &bench_case->params[i];
        report_printf(runtime, summary, ",%s,%.9g",
                      param->name ? param->name : "",
                      param->name ? param->value : 0.0);
    }

    report_printf(runtime, summary, ",%" PRIu32 ",%" PRIu64 ",%.6f",
                  result->iterations, result->elapsed_us, ips);
    write_distribution(runtime, summary, &result->work);
    write_distribution(runtime, summary, &result->present);
    write_distribution(runtime, summary, &result->total);
    write_distribution(runtime, summary, &result->refresh_wait);

    for (int i = 0; i < GRAPE_TELEMETRY_TIMER_COUNT; ++i) {
        const grape_telemetry_stat_t *stat = &result->telemetry.timers[i];
        double avg = stat->calls
            ? (double)stat->total_us / (double)stat->calls
            : 0.0;
        report_printf(runtime, summary,
                      ",%" PRIu64 ",%.3f,%" PRIu64 ",%" PRIu32,
                      stat->total_us, avg, stat->max_us, stat->calls);
    }

    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_METRICS; ++i) {
        if (i < result->metric_count) {
            report_printf(runtime, summary, ",%s,%s,%.9g",
                          result->metrics[i].name ? result->metrics[i].name : "",
                          result->metrics[i].unit ? result->metrics[i].unit : "",
                          result->metrics[i].value);
        } else {
            report_printf(runtime, summary, ",,,0");
        }
    }
    report_printf(runtime, summary, "\n");

    grape_benchmark_text_buffer_t *sample_buffer = &runtime->samples_buffer;
    if (sample_buffer->enabled && samples) {
        for (uint32_t i = 0; i < result->iterations; ++i) {
            report_printf(runtime, sample_buffer, "%s,%s,%s",
                          kind_name(bench_case->kind),
                          bench_case->group,
                          bench_case->name);
            for (size_t p = 0; p < GRAPE_BENCHMARK_MAX_PARAMS; ++p) {
                const grape_benchmark_param_t *param = &bench_case->params[p];
                report_printf(runtime, sample_buffer, ",%s,%.9g",
                              param->name ? param->name : "",
                              param->name ? param->value : 0.0);
            }
            report_printf(runtime, sample_buffer,
                          ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
                          samples[i].iteration_index,
                          samples[i].work_us,
                          samples[i].present_us,
                          samples[i].total_us,
                          samples[i].refresh_wait_us);
        }
    }
}

static esp_err_t write_buffer_file(const char *directory,
                                   const char *name,
                                   const grape_benchmark_text_buffer_t *buffer)
{
    if (!buffer || !buffer->enabled) {
        return ESP_OK;
    }

    char path[256];
    if (!make_path(path, sizeof(path), directory, name)) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGW(TAG, "Could not open %s (%s)", path, strerror(errno));
        return ESP_FAIL;
    }

    size_t written = fwrite(buffer->data, 1, buffer->size, file);
    bool ok = written == buffer->size;
    if (fflush(file) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        ESP_LOGW(TAG, "Failed while writing %s", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved %s (%u bytes)", path, (unsigned)buffer->size);
    return ESP_OK;
}

static esp_err_t write_all_reports(grape_benchmark_runtime_t *runtime)
{
    if (!runtime->report_directory[0]) {
        char path[256];
        bool created = false;
        for (unsigned run = 1; run <= 9999; ++run) {
            int n = snprintf(path, sizeof(path), "%s/grape_run_%04u",
                             runtime->config.output_directory, run);
            if (n < 0 || (size_t)n >= sizeof(path)) return ESP_ERR_INVALID_SIZE;
            if (mkdir(path, 0777) == 0) { created = true; break; }
            if (errno != EEXIST) return ESP_FAIL;
        }
        if (!created) return ESP_ERR_NO_MEM;
        memcpy(runtime->report_directory, path, strlen(path) + 1);
        runtime->config.output_directory = runtime->report_directory;
        report_printf(runtime, &runtime->metadata_buffer,
                      "result_directory=%s\n", runtime->report_directory);
        ESP_LOGI(TAG, "New run directory: %s", runtime->report_directory);
    }
    esp_err_t ret = write_buffer_file(
        runtime->config.output_directory,
        "grape_benchmark_metadata.txt",
        &runtime->metadata_buffer
    );
    if (ret == ESP_OK) {
        ret = write_buffer_file(
            runtime->config.output_directory,
            "grape_benchmark_summary.csv",
            &runtime->summary_buffer
        );
    }
    if (ret == ESP_OK) {
        ret = write_buffer_file(
            runtime->config.output_directory,
            "grape_benchmark_samples.csv",
            &runtime->samples_buffer
        );
    }
    if (ret == ESP_OK) {
        ret = write_buffer_file(
            runtime->config.output_directory,
            "grape_function_profile.csv",
            &runtime->function_profile_buffer
        );
    }
    if (ret == ESP_OK) ret = write_buffer_file(runtime->config.output_directory,
        "grape_benchmark_references.csv", &runtime->references_buffer);
    return ret;
}

esp_err_t grape_benchmark_report_save_wait(grape_benchmark_runtime_t *runtime)
{
    if (!runtime || !runtime->config.output_directory) {
        return ESP_ERR_INVALID_ARG;
    }
    if (runtime->report_error != ESP_OK) {
        return runtime->report_error;
    }

    const bool uses_sd = strncmp(
        runtime->config.output_directory,
        GRAPE_STORAGE_SD_MOUNT_POINT,
        strlen(GRAPE_STORAGE_SD_MOUNT_POINT)
    ) == 0;

    ESP_LOGI(TAG,
             "Benchmark complete; %u bytes of report data retained in PSRAM",
             (unsigned)(runtime->metadata_buffer.size +
                        runtime->summary_buffer.size +
                        runtime->samples_buffer.size +
                        runtime->function_profile_buffer.size));

    if (!uses_sd) {
        return write_all_reports(runtime);
    }

    while (true) {
        if (!grape_storage_sd_is_mounted()) {
            esp_err_t mount_ret = grape_storage_sd_mount();
            if (mount_ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "Insert SD card to save benchmark results; retrying in %u ms",
                         (unsigned)GRAPE_BENCHMARK_SD_RETRY_MS);
                vTaskDelay(pdMS_TO_TICKS(GRAPE_BENCHMARK_SD_RETRY_MS));
                continue;
            }
        }

        esp_err_t ret = write_all_reports(runtime);
        if (ret == ESP_OK) {
            esp_err_t unmount_ret = grape_storage_sd_unmount();
            if (unmount_ret != ESP_OK) {
                ESP_LOGW(TAG, "Reports saved, but SD unmount failed: %s",
                         esp_err_to_name(unmount_ret));
            } else {
                if (runtime->references_pending)
                    ESP_LOGI(TAG, "Timing results saved; keep SD inserted for reference replay");
                else
                    ESP_LOGI(TAG, "Benchmark results saved; SD card is safe to remove");
            }
            return ESP_OK;
        }

        ESP_LOGW(TAG,
                 "Could not save benchmark results; data remains in PSRAM and will be retried");
        grape_storage_sd_unmount();
        vTaskDelay(pdMS_TO_TICKS(GRAPE_BENCHMARK_SD_RETRY_MS));
    }
}


void grape_benchmark_report_references_complete(grape_benchmark_runtime_t *runtime)
{
    runtime->references_pending = false;
    report_printf(runtime, &runtime->metadata_buffer, "references_complete=%u\n",
                  GRAPE_BENCHMARK_CAPTURE_REFERENCES ? 1U : 0U);
}

/* IEEE CRC32 over tightly packed rows, independent of padding or allocation. */
static uint32_t reference_crc32(const uint8_t *pixels, size_t stride,
                                size_t row_bytes, uint32_t height)
{
    uint32_t crc = UINT32_MAX;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = pixels + y * stride;
        for (size_t x = 0; x < row_bytes; ++x) {
            crc ^= row[x];
            for (unsigned b = 0; b < 8; ++b)
                crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
        if ((y & 63U) == 63U) vTaskDelay(1);
    }
    return ~crc;
}

esp_err_t grape_benchmark_report_reference(grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bc, uint32_t frame, const char *plane,
    const char *format, uint32_t width, uint32_t height, uint32_t samples,
    const void *pixels, size_t stride, size_t row_bytes)
{
    if (!runtime || !bc || !pixels || !height || !row_bytes || stride < row_bytes)
        return ESP_ERR_INVALID_ARG;
    if (row_bytes > SIZE_MAX / height) return ESP_ERR_INVALID_SIZE;
    char name[160], path[256];
    int n = snprintf(name, sizeof(name), "%s_%s_f%u_%s.raw",
                     bc->group, bc->name, (unsigned)frame, plane);
    if (n < 0 || (size_t)n >= sizeof(name) ||
        !make_path(path, sizeof(path), runtime->config.output_directory, name))
        return ESP_ERR_INVALID_SIZE;
    bool uses_sd = strncmp(runtime->config.output_directory,
        GRAPE_STORAGE_SD_MOUNT_POINT, strlen(GRAPE_STORAGE_SD_MOUNT_POINT)) == 0;
    uint32_t crc = reference_crc32(pixels, stride, row_bytes, height);
    for (;;) {
        if (uses_sd && !grape_storage_sd_is_mounted()) {
            esp_err_t ret = grape_storage_sd_mount();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Insert SD card to save references");
                vTaskDelay(pdMS_TO_TICKS(GRAPE_BENCHMARK_SD_RETRY_MS));
                continue;
            }
        }
        FILE *file = fopen(path, "wb");
        bool ok = file != NULL;
        if (file) {
            for (uint32_t y = 0; y < height && ok; ++y) {
                ok = fwrite((const uint8_t *)pixels + y * stride, 1, row_bytes, file) == row_bytes;
                if ((y & 63U) == 63U) vTaskDelay(1);
            }
            if (fflush(file) != 0) ok = false;
            if (fclose(file) != 0) ok = false;
        }
        if (ok) break;
        if (!uses_sd) return ESP_FAIL;
        ESP_LOGW(TAG, "Reference write failed; free space/check SD; retrying %s", name);
        grape_storage_sd_unmount();
        vTaskDelay(pdMS_TO_TICKS(GRAPE_BENCHMARK_SD_RETRY_MS));
    }
    report_printf(runtime, &runtime->references_buffer,
        "%s,%s,%u,%s,%s,%u,%u,%u,%u,%08" PRIx32 ",%s\n",
        bc->group, bc->name, (unsigned)frame, plane, format,
        (unsigned)width, (unsigned)height, (unsigned)samples,
        (unsigned)(row_bytes * height), crc, name);
    return runtime->report_error;
}

esp_err_t grape_benchmark_capture_display(grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bc, void *state, uint32_t frame)
{
    (void)state;
    const grape_display_info_t *d = grape_get_display_info(runtime->grape);
    if (!d) return ESP_ERR_INVALID_STATE;
    const char *format;
    size_t bpp;
    switch (d->format) {
        case GRAPE_PIXEL_FORMAT_RGB565: format = "rgb565le"; bpp = 2; break;
        case GRAPE_PIXEL_FORMAT_RGB888: format = "rgb888"; bpp = 3; break;
        case GRAPE_PIXEL_FORMAT_RGBA8888: format = "rgba8888"; bpp = 4; break;
        default: return ESP_ERR_NOT_SUPPORTED;
    }
    size_t stride = (size_t)d->width * bpp;
    if (d->height == 0 || stride > SIZE_MAX / d->height) return ESP_ERR_INVALID_SIZE;
    void *pixels = heap_caps_malloc(stride * d->height, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) return ESP_ERR_NO_MEM;
    esp_err_t ret = grape_benchmark_copy_presented(runtime->grape, pixels, stride * d->height);
    if (ret == ESP_OK) ret = grape_benchmark_report_reference(runtime, bc, frame,
        "display", format, d->width, d->height, 1, pixels, stride, stride);
    heap_caps_free(pixels);
    return ret;
}
