#include "grape_benchmark_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"

static const char *TAG = "grape_bench";

static const char *metric_csv_name(grape_profile_metric_t metric)
{
    static const char *const names[GRAPE_PROFILE_METRIC_COUNT] = {
        [GRAPE_PROFILE_METRIC_PRESENT] = "present",
        [GRAPE_PROFILE_METRIC_DAMAGE_ADD] = "damage_add",
        [GRAPE_PROFILE_METRIC_DAMAGE_PLAN] = "damage_plan",
        [GRAPE_PROFILE_METRIC_SURFACE_TRANSFORM] = "surface_transform",
        [GRAPE_PROFILE_METRIC_SURFACE_RECACHE] = "surface_recache",
        [GRAPE_PROFILE_METRIC_COMPOSITOR] = "compositor",
        [GRAPE_PROFILE_METRIC_PPA_FILL] = "ppa_fill",
        [GRAPE_PROFILE_METRIC_CPU_FILL] = "cpu_fill",
        [GRAPE_PROFILE_METRIC_PPA_BLEND_DISPATCH] = "ppa_blend_dispatch",
        [GRAPE_PROFILE_METRIC_PPA_BLEND_HW] = "ppa_blend_hw",
        [GRAPE_PROFILE_METRIC_CPU_SURFACE_RASTER] = "cpu_surface_raster",
        [GRAPE_PROFILE_METRIC_SHEAR_PREP] = "shear_prep",
        [GRAPE_PROFILE_METRIC_SHEAR_X1] = "shear_x1",
        [GRAPE_PROFILE_METRIC_SHEAR_Y] = "shear_y",
        [GRAPE_PROFILE_METRIC_SHEAR_X2] = "shear_x2",
        [GRAPE_PROFILE_METRIC_SHEAR_CLEAR] = "shear_clear",
        [GRAPE_PROFILE_METRIC_SHEAR_QUARTER_TURN] = "shear_quarter_turn",
        [GRAPE_PROFILE_METRIC_SHEAR_COMPOSITE] = "shear_composite",
        [GRAPE_PROFILE_METRIC_LCD_DRAW_SUBMIT] = "lcd_draw_submit",
        [GRAPE_PROFILE_METRIC_LCD_DRAW_WAIT] = "lcd_draw_wait",
    };

    if ((unsigned)metric >= GRAPE_PROFILE_METRIC_COUNT || !names[metric]) {
        return "unknown";
    }

    return names[metric];
}

static bool make_path(
    char *buffer,
    size_t buffer_size,
    const char *directory,
    const char *filename
)
{
    if (!buffer || buffer_size == 0 || !directory || !filename) {
        return false;
    }

    size_t length = strlen(directory);
    const char *separator = (length > 0 && directory[length - 1] == '/') ? "" : "/";

    int written = snprintf(
        buffer,
        buffer_size,
        "%s%s%s",
        directory,
        separator,
        filename
    );

    return written > 0 && (size_t)written < buffer_size;
}

static void write_summary_header(FILE *file)
{
    if (!file) {
        return;
    }

    fprintf(file,
            "group,case,"
            "param0_name,param0_value,param1_name,param1_value,"
            "param2_name,param2_value,param3_name,param3_value,"
            "frames,elapsed_us,fps,"
            "update_wall_avg_us,update_wall_min_us,update_wall_max_us,"
            "present_wall_avg_us,present_wall_min_us,present_wall_max_us,"
            "frame_wall_avg_us,frame_wall_min_us,frame_wall_max_us");

    for (int i = 0; i < GRAPE_PROFILE_METRIC_COUNT; ++i) {
        const char *name = metric_csv_name((grape_profile_metric_t)i);
        fprintf(file,
                ",%s_total_us,%s_avg_us,%s_max_us,%s_calls",
                name, name, name, name);
    }

    fputc('\n', file);
    fflush(file);
}

static void write_samples_header(FILE *file)
{
    if (!file) {
        return;
    }

    fprintf(file,
            "group,case,frame,update_us,present_us,frame_us\n");
    fflush(file);
}

esp_err_t grape_benchmark_report_open(grape_benchmark_runtime_t *runtime)
{
    if (!runtime) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!runtime->config.output_directory) {
        return ESP_OK;
    }

    char path[256];

    if (runtime->config.write_summary_csv &&
        make_path(path, sizeof(path), runtime->config.output_directory,
                  "grape_benchmark_summary.csv")) {
        runtime->summary_csv = fopen(path, "w");
        if (!runtime->summary_csv) {
            ESP_LOGW(TAG,
                     "Could not open %s (%s); summary will only be logged",
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
            ESP_LOGW(TAG,
                     "Could not open %s (%s); per-frame samples will not be saved",
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

    const grape_display_info_t *display =
        grape_get_display_info(runtime->grape);

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

    fprintf(file, "profiler_enabled=%d\n", GRAPE_PROFILE_ENABLE);
    fprintf(file, "warmup_frames=%" PRIu32 "\n", runtime->config.warmup_frames);
    fprintf(file, "measured_frames=%" PRIu32 "\n", runtime->config.measured_frames);

    fclose(file);
    ESP_LOGI(TAG, "Metadata: %s", path);
}

void grape_benchmark_report_case(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    const grape_benchmark_result_t *result,
    const grape_benchmark_frame_sample_t *samples
)
{
    if (!runtime || !bench_case || !result || result->frames == 0) {
        return;
    }

    double fps =
        ((double)result->frames * 1000000.0) / (double)result->elapsed_us;

    double update_avg =
        (double)result->update_total_us / (double)result->frames;
    double present_avg =
        (double)result->present_total_us / (double)result->frames;
    double frame_avg =
        (double)result->frame_total_us / (double)result->frames;

    if (runtime->config.log_each_case) {
        const grape_profile_stat_t *cpu_raster =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_CPU_SURFACE_RASTER];
        const grape_profile_stat_t *ppa_blend =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_PPA_BLEND_HW];
        const grape_profile_stat_t *lcd_wait =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_LCD_DRAW_WAIT];

        double cpu_raster_per_frame =
            (double)cpu_raster->total_us / (double)result->frames;
        double ppa_blend_per_frame =
            (double)ppa_blend->total_us / (double)result->frames;
        double lcd_wait_per_frame =
            (double)lcd_wait->total_us / (double)result->frames;

        ESP_LOGI(TAG,
                 "%-15s %-28s FPS=%7.2f frame=%7.3f ms present=%7.3f ms update=%7.3f ms",
                 bench_case->group,
                 bench_case->name,
                 fps,
                 frame_avg / 1000.0,
                 present_avg / 1000.0,
                 update_avg / 1000.0);

        ESP_LOGI(TAG,
                 "  per-frame: CPU raster=%7.3f ms PPA blend=%7.3f ms LCD wait=%7.3f ms",
                 cpu_raster_per_frame / 1000.0,
                 ppa_blend_per_frame / 1000.0,
                 lcd_wait_per_frame / 1000.0);

        const grape_profile_stat_t *shear_prep =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_SHEAR_PREP];
        const grape_profile_stat_t *shear_x1 =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_SHEAR_X1];
        const grape_profile_stat_t *shear_y =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_SHEAR_Y];
        const grape_profile_stat_t *shear_x2 =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_SHEAR_X2];
        const grape_profile_stat_t *shear_clear =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_SHEAR_CLEAR];
        const grape_profile_stat_t *shear_composite =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_SHEAR_COMPOSITE];
        const grape_profile_stat_t *shear_quarter =
            &result->profile.metrics[GRAPE_PROFILE_METRIC_SHEAR_QUARTER_TURN];

        if (shear_prep->calls || shear_x1->calls || shear_y->calls ||
            shear_x2->calls || shear_quarter->calls || shear_composite->calls) {
            double prep_per_frame =
                (double)shear_prep->total_us / (double)result->frames;
            double x1_per_frame =
                (double)shear_x1->total_us / (double)result->frames;
            double y_per_frame =
                (double)shear_y->total_us / (double)result->frames;
            double x2_per_frame =
                (double)shear_x2->total_us / (double)result->frames;
            double clear_per_frame =
                (double)shear_clear->total_us / (double)result->frames;
            double composite_per_frame =
                (double)shear_composite->total_us / (double)result->frames;
            double quarter_per_frame =
                (double)shear_quarter->total_us / (double)result->frames;

            ESP_LOGI(TAG,
                     "  shear: prep=%7.3f ms X1=%7.3f ms Y=%7.3f ms X2=%7.3f ms clear=%7.3f ms composite=%7.3f ms quarter=%7.3f ms",
                     prep_per_frame / 1000.0,
                     x1_per_frame / 1000.0,
                     y_per_frame / 1000.0,
                     x2_per_frame / 1000.0,
                     clear_per_frame / 1000.0,
                     composite_per_frame / 1000.0,
                     quarter_per_frame / 1000.0);
        }
    }

    if (runtime->summary_csv) {
        fprintf(runtime->summary_csv,
                "%s,%s",
                bench_case->group,
                bench_case->name);

        for (size_t i = 0; i < GRAPE_BENCHMARK_MAX_PARAMS; ++i) {
            const grape_benchmark_param_t *param = &bench_case->params[i];
            fprintf(runtime->summary_csv,
                    ",%s,%.9g",
                    param->name ? param->name : "",
                    param->name ? param->value : 0.0);
        }

        fprintf(runtime->summary_csv,
                ",%" PRIu32 ",%" PRIu64 ",%.6f"
                ",%.3f,%" PRIu32 ",%" PRIu32
                ",%.3f,%" PRIu32 ",%" PRIu32
                ",%.3f,%" PRIu32 ",%" PRIu32,
                result->frames,
                result->elapsed_us,
                fps,
                update_avg,
                result->update_min_us,
                result->update_max_us,
                present_avg,
                result->present_min_us,
                result->present_max_us,
                frame_avg,
                result->frame_min_us,
                result->frame_max_us);

        for (int i = 0; i < GRAPE_PROFILE_METRIC_COUNT; ++i) {
            const grape_profile_stat_t *stat = &result->profile.metrics[i];
            double avg = stat->calls
                ? (double)stat->total_us / (double)stat->calls
                : 0.0;

            fprintf(runtime->summary_csv,
                    ",%" PRIu64 ",%.3f,%" PRIu64 ",%" PRIu32,
                    stat->total_us,
                    avg,
                    stat->max_us,
                    stat->calls);
        }

        fputc('\n', runtime->summary_csv);
        fflush(runtime->summary_csv);
    }

    if (runtime->samples_csv && samples) {
        for (uint32_t i = 0; i < result->frames; ++i) {
            fprintf(runtime->samples_csv,
                    "%s,%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
                    bench_case->group,
                    bench_case->name,
                    samples[i].frame_index,
                    samples[i].update_us,
                    samples[i].present_us,
                    samples[i].frame_us);
        }
        fflush(runtime->samples_csv);
    }
}
