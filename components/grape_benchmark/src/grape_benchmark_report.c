#include "grape_benchmark_internal.h"
#include "sdkconfig.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

#include "esp_chip_info.h"
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

    write_summary_header(runtime);
    write_samples_header(runtime);
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
    report_printf(runtime, buffer, "idf_version=%s\n", esp_get_idf_version());
    report_printf(runtime, buffer, "idf_target=%s\n", CONFIG_IDF_TARGET);
    report_printf(runtime, buffer, "chip_model=%d\n", (int)chip.model);
    report_printf(runtime, buffer, "chip_revision=%u\n", chip.revision);
    report_printf(runtime, buffer, "chip_cores=%u\n", chip.cores);
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
    report_printf(runtime, buffer, "warmup_iterations=%" PRIu32 "\n",
                  runtime->config.warmup_iterations);
    report_printf(runtime, buffer, "measured_iterations=%" PRIu32 "\n",
                  runtime->config.measured_iterations);
    report_printf(runtime, buffer, "fixed_dt_us=%" PRIu32 "\n", runtime->config.fixed_dt_us);
    report_printf(runtime, buffer, "seed=0x%08" PRIx32 "\n", runtime->config.seed);
    report_printf(runtime, buffer, "suite_mask=0x%08" PRIx32 "\n", runtime->config.suite_mask);
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
                        runtime->samples_buffer.size));

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
