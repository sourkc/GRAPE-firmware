#include "grape_benchmark_internal.h"
#include "sdkconfig.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"

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

static void write_distribution_header(FILE *file, const char *prefix)
{
    fprintf(file,
            ",%s_mean_us,%s_stddev_us,%s_min_us,%s_p50_us,%s_p95_us,%s_p99_us,%s_max_us",
            prefix, prefix, prefix, prefix, prefix, prefix, prefix);
}

static void write_distribution(FILE *file, const grape_benchmark_distribution_t *dist)
{
    fprintf(file,
            ",%.3f,%.3f,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32,
            dist->mean_us,
            dist->stddev_us,
            dist->min_us,
            dist->p50_us,
            dist->p95_us,
            dist->p99_us,
            dist->max_us);
}

static void write_summary_header(FILE *file)
{
    fprintf(file, "status,kind,group,name");
    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_PARAMS; ++i) {
        fprintf(file, ",param%u_name,param%u_value", (unsigned)i, (unsigned)i);
    }
    fprintf(file, ",iterations,elapsed_us,iterations_per_second");
    write_distribution_header(file, "work");
    write_distribution_header(file, "present");
    write_distribution_header(file, "total");
    write_distribution_header(file, "refresh_wait");

    for (int i = 0; i < GRAPE_TELEMETRY_TIMER_COUNT; ++i) {
        const char *name = grape_telemetry_timer_csv_name((grape_telemetry_timer_t)i);
        fprintf(file, ",%s_total_us,%s_avg_us,%s_max_us,%s_calls",
                name, name, name, name);
    }

    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_METRICS; ++i) {
        fprintf(file, ",metric%u_name,metric%u_unit,metric%u_value",
                (unsigned)i, (unsigned)i, (unsigned)i);
    }
    fputc('\n', file);
}

static void write_samples_header(FILE *file)
{
    fprintf(file, "kind,group,name");
    for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_PARAMS; ++i) {
        fprintf(file, ",param%u_name,param%u_value", (unsigned)i, (unsigned)i);
    }
    fprintf(file, ",iteration,work_us,present_us,total_us,refresh_wait_us\n");
}

esp_err_t grape_benchmark_report_open(grape_benchmark_runtime_t *runtime)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }

    char path[256];
    if (runtime->config.write_summary_csv &&
        make_path(path, sizeof(path), runtime->config.output_directory,
                  "grape_benchmark_summary.csv")) {
        runtime->summary_csv = fopen(path, "w");
        if (!runtime->summary_csv) {
            ESP_LOGW(TAG, "Could not open %s (%s); summary will only be logged",
                     path, strerror(errno));
        } else {
            write_summary_header(runtime->summary_csv);
            ESP_LOGI(TAG, "Summary CSV: %s", path);
        }
    }

    if (runtime->config.write_samples_csv &&
        make_path(path, sizeof(path), runtime->config.output_directory,
                  "grape_benchmark_samples.csv")) {
        runtime->samples_csv = fopen(path, "w");
        if (!runtime->samples_csv) {
            ESP_LOGW(TAG, "Could not open %s (%s); samples disabled",
                     path, strerror(errno));
        } else {
            write_samples_header(runtime->samples_csv);
            ESP_LOGI(TAG, "Samples CSV: %s", path);
        }
    }

    grape_benchmark_report_metadata(runtime);
    return ESP_OK;
}

void grape_benchmark_report_close(grape_benchmark_runtime_t *runtime)
{
    if (!runtime) {
        return;
    }
    if (runtime->summary_csv) {
        fclose(runtime->summary_csv);
        runtime->summary_csv = NULL;
    }
    if (runtime->samples_csv) {
        fclose(runtime->samples_csv);
        runtime->samples_csv = NULL;
    }
}

void grape_benchmark_report_metadata(grape_benchmark_runtime_t *runtime)
{
    if (!runtime || !runtime->config.output_directory) {
        return;
    }

    char path[256];
    if (!make_path(path, sizeof(path), runtime->config.output_directory,
                   "grape_benchmark_metadata.txt")) {
        return;
    }

    FILE *file = fopen(path, "w");
    if (!file) {
        ESP_LOGW(TAG, "Could not open %s (%s)", path, strerror(errno));
        return;
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const grape_display_info_t *display = grape_get_display_info(runtime->grape);

    fprintf(file, "benchmark_build=%s\n", GRAPE_BENCHMARK_BUILD_LABEL);
    fprintf(file, "idf_version=%s\n", esp_get_idf_version());
    fprintf(file, "idf_target=%s\n", CONFIG_IDF_TARGET);
    fprintf(file, "chip_model=%d\n", (int)chip.model);
    fprintf(file, "chip_revision=%u\n", chip.revision);
    fprintf(file, "chip_cores=%u\n", chip.cores);
    fprintf(file, "cpu_frequency_mhz=%d\n", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    fprintf(file, "psram_total_bytes=%u\n",
            (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    fprintf(file, "psram_free_bytes=%u\n",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (display) {
        fprintf(file, "display_name=%s\n", display->name ? display->name : "unknown");
        fprintf(file, "display_width=%" PRIu32 "\n", display->width);
        fprintf(file, "display_height=%" PRIu32 "\n", display->height);
        fprintf(file, "display_format=%d\n", (int)display->format);
    }

    fprintf(file, "telemetry_level=%d\n", GRAPE_TELEMETRY_LEVEL);
    fprintf(file, "warmup_iterations=%" PRIu32 "\n", runtime->config.warmup_iterations);
    fprintf(file, "measured_iterations=%" PRIu32 "\n", runtime->config.measured_iterations);
    fprintf(file, "fixed_dt_us=%" PRIu32 "\n", runtime->config.fixed_dt_us);
    fprintf(file, "seed=0x%08" PRIx32 "\n", runtime->config.seed);
    fprintf(file, "suite_mask=0x%08" PRIx32 "\n", runtime->config.suite_mask);

    fclose(file);
    ESP_LOGI(TAG, "Metadata: %s", path);
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

    if (runtime->summary_csv) {
        fprintf(runtime->summary_csv, "skip,%s,%s,%s",
                kind_name(bench_case->kind), bench_case->group, bench_case->name);
        for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_PARAMS; ++i) {
            const grape_benchmark_param_t *param = &bench_case->params[i];
            fprintf(runtime->summary_csv, ",%s,%.9g",
                    param->name ? param->name : "",
                    param->name ? param->value : 0.0);
        }

        grape_benchmark_distribution_t zero_dist = {0};
        fprintf(runtime->summary_csv, ",0,0,0");
        write_distribution(runtime->summary_csv, &zero_dist);
        write_distribution(runtime->summary_csv, &zero_dist);
        write_distribution(runtime->summary_csv, &zero_dist);
        write_distribution(runtime->summary_csv, &zero_dist);

        for (int i = 0; i < GRAPE_TELEMETRY_TIMER_COUNT; ++i) {
            fprintf(runtime->summary_csv, ",0,0,0,0");
        }
        for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_METRICS; ++i) {
            fprintf(runtime->summary_csv, ",,,0");
        }

        fputc('\n', runtime->summary_csv);
        fflush(runtime->summary_csv);
    }
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

    if (runtime->summary_csv) {
        fprintf(runtime->summary_csv, "ok,%s,%s,%s",
                kind_name(bench_case->kind), bench_case->group, bench_case->name);
        for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_PARAMS; ++i) {
            const grape_benchmark_param_t *param = &bench_case->params[i];
            fprintf(runtime->summary_csv, ",%s,%.9g",
                    param->name ? param->name : "",
                    param->name ? param->value : 0.0);
        }

        fprintf(runtime->summary_csv, ",%" PRIu32 ",%" PRIu64 ",%.6f",
                result->iterations, result->elapsed_us, ips);
        write_distribution(runtime->summary_csv, &result->work);
        write_distribution(runtime->summary_csv, &result->present);
        write_distribution(runtime->summary_csv, &result->total);
        write_distribution(runtime->summary_csv, &result->refresh_wait);

        for (int i = 0; i < GRAPE_TELEMETRY_TIMER_COUNT; ++i) {
            const grape_telemetry_stat_t *stat = &result->telemetry.timers[i];
            double avg = stat->calls
                ? (double)stat->total_us / (double)stat->calls
                : 0.0;
            fprintf(runtime->summary_csv,
                    ",%" PRIu64 ",%.3f,%" PRIu64 ",%" PRIu32,
                    stat->total_us, avg, stat->max_us, stat->calls);
        }

        for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_METRICS; ++i) {
            if (i < result->metric_count) {
                fprintf(runtime->summary_csv, ",%s,%s,%.9g",
                        result->metrics[i].name ? result->metrics[i].name : "",
                        result->metrics[i].unit ? result->metrics[i].unit : "",
                        result->metrics[i].value);
            } else {
                fprintf(runtime->summary_csv, ",,,0");
            }
        }
        fputc('\n', runtime->summary_csv);
        fflush(runtime->summary_csv);
    }

    if (runtime->samples_csv && samples) {
        for (uint32_t i = 0; i < result->iterations; ++i) {
            fprintf(runtime->samples_csv,
                    "%s,%s,%s",
                    kind_name(bench_case->kind),
                    bench_case->group,
                    bench_case->name);
            for (size_t p = 0; p < GRAPE_BENCHMARK_MAX_PARAMS; ++p) {
                const grape_benchmark_param_t *param = &bench_case->params[p];
                fprintf(runtime->samples_csv,
                        ",%s,%.9g",
                        param->name ? param->name : "",
                        param->name ? param->value : 0.0);
            }
            fprintf(runtime->samples_csv,
                    ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
                    samples[i].iteration_index,
                    samples[i].work_us,
                    samples[i].present_us,
                    samples[i].total_us,
                    samples[i].refresh_wait_us);
        }
        fflush(runtime->samples_csv);
    }
}
