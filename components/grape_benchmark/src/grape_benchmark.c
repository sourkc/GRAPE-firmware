#include "grape_benchmark_internal.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "grape_bench";

static uint32_t elapsed_u32(int64_t start_us, int64_t end_us)
{
    int64_t elapsed = end_us - start_us;

    if (elapsed <= 0) {
        return 0;
    }

    if ((uint64_t)elapsed > UINT32_MAX) {
        return UINT32_MAX;
    }

    return (uint32_t)elapsed;
}

static void result_add_sample(
    grape_benchmark_result_t *result,
    const grape_benchmark_frame_sample_t *sample
)
{
    result->frames++;

    result->update_total_us += sample->update_us;
    result->present_total_us += sample->present_us;
    result->frame_total_us += sample->frame_us;

    if (sample->update_us < result->update_min_us) {
        result->update_min_us = sample->update_us;
    }
    if (sample->update_us > result->update_max_us) {
        result->update_max_us = sample->update_us;
    }

    if (sample->present_us < result->present_min_us) {
        result->present_min_us = sample->present_us;
    }
    if (sample->present_us > result->present_max_us) {
        result->present_max_us = sample->present_us;
    }

    if (sample->frame_us < result->frame_min_us) {
        result->frame_min_us = sample->frame_us;
    }
    if (sample->frame_us > result->frame_max_us) {
        result->frame_max_us = sample->frame_us;
    }
}

static esp_err_t run_warmup(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state
)
{
    for (uint32_t frame = 0; frame < runtime->config.warmup_frames; ++frame) {
        esp_err_t ret = bench_case->step(runtime, bench_case, state, frame);
        if (ret != ESP_OK) {
            return ret;
        }

        ret = grape_present(runtime->grape);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t run_measured(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state,
    grape_benchmark_result_t *result,
    grape_benchmark_frame_sample_t *samples
)
{
    *result = (grape_benchmark_result_t){
        .update_min_us = UINT32_MAX,
        .present_min_us = UINT32_MAX,
        .frame_min_us = UINT32_MAX,
    };

    grape_profile_reset();

    int64_t benchmark_start_us = esp_timer_get_time();

    for (uint32_t frame = 0; frame < runtime->config.measured_frames; ++frame) {
        uint32_t sequence_frame = runtime->config.warmup_frames + frame;

        int64_t frame_start_us = esp_timer_get_time();

        int64_t update_start_us = frame_start_us;
        esp_err_t ret =
            bench_case->step(runtime, bench_case, state, sequence_frame);
        int64_t update_end_us = esp_timer_get_time();

        if (ret != ESP_OK) {
            return ret;
        }

        int64_t present_start_us = update_end_us;
        ret = grape_present(runtime->grape);
        int64_t present_end_us = esp_timer_get_time();

        if (ret != ESP_OK) {
            return ret;
        }

        grape_benchmark_frame_sample_t sample = {
            .frame_index = frame,
            .update_us = elapsed_u32(update_start_us, update_end_us),
            .present_us = elapsed_u32(present_start_us, present_end_us),
            .frame_us = elapsed_u32(frame_start_us, present_end_us),
        };

        samples[frame] = sample;
        result_add_sample(result, &sample);
    }

    result->elapsed_us =
        (uint64_t)(esp_timer_get_time() - benchmark_start_us);

    grape_profile_snapshot(&result->profile);

    if (result->frames == 0) {
        result->update_min_us = 0;
        result->present_min_us = 0;
        result->frame_min_us = 0;
    }

    return ESP_OK;
}

static esp_err_t run_case(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case
)
{
    void *state = NULL;

    esp_err_t ret = bench_case->setup(
        runtime,
        bench_case,
        &state
    );
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Setup failed for %s/%s: %s",
                 bench_case->group,
                 bench_case->name,
                 esp_err_to_name(ret));
        return ret;
    }

    ret = run_warmup(runtime, bench_case, state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Warmup failed for %s/%s: %s",
                 bench_case->group,
                 bench_case->name,
                 esp_err_to_name(ret));
        bench_case->teardown(runtime, bench_case, state);
        return ret;
    }

    grape_benchmark_frame_sample_t *samples = calloc(
        runtime->config.measured_frames,
        sizeof(*samples)
    );
    if (!samples) {
        bench_case->teardown(runtime, bench_case, state);
        return ESP_ERR_NO_MEM;
    }

    grape_benchmark_result_t result;
    ret = run_measured(
        runtime,
        bench_case,
        state,
        &result,
        samples
    );

    if (ret == ESP_OK) {
        grape_benchmark_report_case(
            runtime,
            bench_case,
            &result,
            samples
        );
    } else {
        ESP_LOGE(TAG, "Measurement failed for %s/%s: %s",
                 bench_case->group,
                 bench_case->name,
                 esp_err_to_name(ret));
    }

    free(samples);

    bench_case->teardown(runtime, bench_case, state);

    /*
     * Clear the previous case from the display outside the measured region.
     * This also gives FreeRTOS a clean scheduling point between cases.
     */
    if (ret == ESP_OK) {
        esp_err_t clear_ret = grape_invalidate_all(runtime->grape);
        if (clear_ret == ESP_OK) {
            clear_ret = grape_present(runtime->grape);
        }
        if (clear_ret != ESP_OK) {
            ret = clear_ret;
        }
    }

    if (runtime->config.case_cooldown_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(runtime->config.case_cooldown_ms));
    }

    return ret;
}

esp_err_t grape_benchmark_run(
    grape_context_t *grape,
    const grape_benchmark_config_t *config
)
{
    if (!grape) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_benchmark_config_t resolved =
        config ? *config : (grape_benchmark_config_t)GRAPE_BENCHMARK_CONFIG_DEFAULT();

    if (resolved.measured_frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_benchmark_runtime_t runtime = {
        .grape = grape,
        .config = resolved,
    };

    size_t suite_count = 0;
    const grape_benchmark_suite_t *suites =
        grape_benchmark_suites(&suite_count);

    size_t total_case_count = 0;
    for (size_t suite_index = 0; suite_index < suite_count; ++suite_index) {
        size_t suite_case_count = 0;
        suites[suite_index].cases(&suite_case_count);
        total_case_count += suite_case_count;
    }

    ESP_LOGI(TAG,
             "Starting benchmark: %u warmup + %u measured frames, %u cases in %u suites",
             (unsigned)resolved.warmup_frames,
             (unsigned)resolved.measured_frames,
             (unsigned)total_case_count,
             (unsigned)suite_count);

#if !GRAPE_PROFILE_ENABLE
    ESP_LOGW(TAG,
             "GRAPE profiler is disabled; CSV profiler columns will be zero");
#endif

    bool previous_auto_report = grape_profile_auto_report_enabled();
    grape_profile_set_auto_report(false);

    esp_err_t ret = grape_benchmark_report_open(&runtime);
    if (ret != ESP_OK) {
        grape_profile_set_auto_report(previous_auto_report);
        return ret;
    }

    int64_t suite_start_us = esp_timer_get_time();

    size_t global_case_index = 0;

    for (size_t suite_index = 0;
         suite_index < suite_count && ret == ESP_OK;
         ++suite_index) {

        size_t suite_case_count = 0;
        const grape_benchmark_case_t *cases =
            suites[suite_index].cases(&suite_case_count);

        ESP_LOGI(TAG,
                 "Suite %s: %u cases",
                 suites[suite_index].name,
                 (unsigned)suite_case_count);

        for (size_t case_index = 0;
             case_index < suite_case_count;
             ++case_index) {

            ++global_case_index;

            ESP_LOGI(TAG,
                     "[%u/%u] %s/%s",
                     (unsigned)global_case_index,
                     (unsigned)total_case_count,
                     cases[case_index].group,
                     cases[case_index].name);

            ret = run_case(&runtime, &cases[case_index]);
            if (ret != ESP_OK) {
                break;
            }
        }
    }

    int64_t suite_elapsed_us = esp_timer_get_time() - suite_start_us;

    grape_benchmark_report_close(&runtime);
    grape_profile_reset();
    grape_profile_set_auto_report(previous_auto_report);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "Benchmark complete in %.2f s",
                 (double)suite_elapsed_us / 1000000.0);

        if (resolved.output_directory) {
            ESP_LOGI(TAG,
                     "If the SD/VFS path was mounted, reports are in %s",
                     resolved.output_directory);
        }
    }

    return ret;
}
