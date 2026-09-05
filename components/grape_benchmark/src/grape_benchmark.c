#include "grape_benchmark_internal.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "grape_bench";

#define GRAPE_BENCHMARK_STACK_WARNING_BYTES 512U

static uint32_t benchmark_stack_free_bytes(void)
{
    uint64_t free_bytes = (uint64_t)uxTaskGetStackHighWaterMark(NULL) *
                          sizeof(StackType_t);
    return free_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)free_bytes;
}

static void benchmark_note_stack_headroom(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    const char *phase
)
{
    if (!runtime) {
        return;
    }

    uint32_t free_bytes = benchmark_stack_free_bytes();
    if (free_bytes >= runtime->stack_min_free_bytes) {
        return;
    }

    runtime->stack_min_free_bytes = free_bytes;
    if (bench_case) {
        ESP_LOGI(TAG,
                 "Stack minimum free=%" PRIu32 " bytes after %s/%s (%s)",
                 free_bytes,
                 bench_case->group,
                 bench_case->name,
                 phase ? phase : "case");
    } else {
        ESP_LOGI(TAG, "Stack minimum free=%" PRIu32 " bytes", free_bytes);
    }

    if (free_bytes < GRAPE_BENCHMARK_STACK_WARNING_BYTES) {
        ESP_LOGW(TAG,
                 "Benchmark stack headroom is low: %" PRIu32 " bytes",
                 free_bytes);
    }
}

typedef struct {
    bool telemetry_auto_report;
    bool debug_layers[GRAPE_DEBUG_LAYER_COUNT];
    bool task_wdt_extended;
} grape_benchmark_environment_t;

#if CONFIG_ESP_TASK_WDT_EN && CONFIG_ESP_TASK_WDT_INIT
static uint32_t benchmark_task_wdt_idle_mask(void)
{
    uint32_t mask = 0;

#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
    mask |= (1U << 0);
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
    mask |= (1U << 1);
#endif

    return mask;
}

static esp_err_t benchmark_task_wdt_reconfigure(uint32_t timeout_ms)
{
    const esp_task_wdt_config_t config = {
        .timeout_ms = timeout_ms,
        .idle_core_mask = benchmark_task_wdt_idle_mask(),
#if CONFIG_ESP_TASK_WDT_PANIC
        .trigger_panic = true,
#else
        .trigger_panic = false,
#endif
    };
    return esp_task_wdt_reconfigure(&config);
}

static esp_err_t benchmark_task_wdt_extend(
    grape_benchmark_environment_t *environment
)
{
    if (GRAPE_BENCHMARK_WDT_TIMEOUT_MS <=
        (CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U)) {
        return ESP_OK;
    }

    esp_err_t ret = benchmark_task_wdt_reconfigure(
        GRAPE_BENCHMARK_WDT_TIMEOUT_MS
    );
    if (ret == ESP_OK) {
        environment->task_wdt_extended = true;
        ESP_LOGI(TAG,
                 "Task watchdog timeout extended to %" PRIu32 " ms for benchmark",
                 (uint32_t)GRAPE_BENCHMARK_WDT_TIMEOUT_MS);
    }
    return ret;
}

static void benchmark_task_wdt_restore(
    grape_benchmark_environment_t *environment
)
{
    if (!environment || !environment->task_wdt_extended) {
        return;
    }

    esp_err_t ret = benchmark_task_wdt_reconfigure(
        CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U
    );
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore task watchdog timeout: %s",
                 esp_err_to_name(ret));
        return;
    }

    environment->task_wdt_extended = false;
}
#else
static esp_err_t benchmark_task_wdt_extend(
    grape_benchmark_environment_t *environment
)
{
    (void)environment;
    return ESP_OK;
}

static void benchmark_task_wdt_restore(
    grape_benchmark_environment_t *environment
)
{
    (void)environment;
}
#endif

static esp_err_t prepare_environment(
    grape_benchmark_runtime_t *runtime,
    grape_benchmark_environment_t *environment
)
{
    if (!runtime || !environment) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(environment, 0, sizeof(*environment));
    environment->telemetry_auto_report = grape_telemetry_auto_report_enabled();
    grape_telemetry_set_auto_report(false);

    for (int layer = 0; layer < GRAPE_DEBUG_LAYER_COUNT; ++layer) {
        environment->debug_layers[layer] = grape_debug_is_layer_enabled(
            runtime->grape,
            (grape_debug_layer_t)layer
        );
        esp_err_t ret = grape_debug_set_layer_enabled(
            runtime->grape,
            (grape_debug_layer_t)layer,
            false
        );
        if (ret != ESP_OK) {
            return ret;
        }
    }

    esp_err_t ret = benchmark_task_wdt_extend(environment);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to extend task watchdog timeout: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static void restore_environment(
    grape_benchmark_runtime_t *runtime,
    grape_benchmark_environment_t *environment
)
{
    if (!runtime || !environment) {
        return;
    }

    for (int layer = 0; layer < GRAPE_DEBUG_LAYER_COUNT; ++layer) {
        grape_debug_set_layer_enabled(
            runtime->grape,
            (grape_debug_layer_t)layer,
            environment->debug_layers[layer]
        );
    }
    grape_telemetry_set_auto_report(environment->telemetry_auto_report);
    benchmark_task_wdt_restore(environment);
}

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

uint32_t grape_benchmark_hash_u32(uint32_t value)
{
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

float grape_benchmark_unit_f32(uint32_t seed, uint32_t index)
{
    uint32_t value = grape_benchmark_hash_u32(seed ^ grape_benchmark_hash_u32(index));
    return (float)(value & 0x00ffffffU) / 16777215.0f;
}

float grape_benchmark_fixed_time_s(const grape_benchmark_runtime_t *runtime,
                                   uint32_t sequence_iteration)
{
    if (!runtime) {
        return 0.0f;
    }
    return (float)((double)sequence_iteration *
                   (double)runtime->config.fixed_dt_us / 1000000.0);
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t lhs = *(const uint32_t *)a;
    uint32_t rhs = *(const uint32_t *)b;
    return (lhs > rhs) - (lhs < rhs);
}

static uint32_t percentile_sorted(const uint32_t *values, size_t count, double p)
{
    if (!values || count == 0) {
        return 0;
    }
    double position = p * (double)(count - 1U);
    size_t index = (size_t)ceil(position);
    if (index >= count) {
        index = count - 1U;
    }
    return values[index];
}

static grape_benchmark_distribution_t distribution_from_samples(
    const grape_benchmark_sample_t *samples,
    size_t count,
    size_t field_offset
)
{
    grape_benchmark_distribution_t out = {0};
    if (!samples || count == 0) {
        return out;
    }

    uint32_t *sorted = malloc(count * sizeof(*sorted));
    if (!sorted) {
        return out;
    }

    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *base = (const uint8_t *)&samples[i];
        uint32_t value = *(const uint32_t *)(base + field_offset);
        sorted[i] = value;
        total += value;
    }

    qsort(sorted, count, sizeof(*sorted), compare_u32);
    out.mean_us = (double)total / (double)count;
    out.min_us = sorted[0];
    out.p50_us = percentile_sorted(sorted, count, 0.50);
    out.p95_us = percentile_sorted(sorted, count, 0.95);
    out.p99_us = percentile_sorted(sorted, count, 0.99);
    out.max_us = sorted[count - 1U];

    double sum_sq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double delta = (double)sorted[i] - out.mean_us;
        sum_sq += delta * delta;
    }
    out.stddev_us = sqrt(sum_sq / (double)count);

    free(sorted);
    return out;
}

static uint32_t resolve_warmup_iterations(
    const grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case
)
{
    return bench_case->warmup_iterations != 0
        ? bench_case->warmup_iterations
        : runtime->config.warmup_iterations;
}

static uint32_t resolve_measured_iterations(
    const grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case
)
{
    return bench_case->measured_iterations != 0
        ? bench_case->measured_iterations
        : runtime->config.measured_iterations;
}

static esp_err_t execute_iteration(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state,
    uint32_t sequence_iteration,
    bool measured,
    grape_benchmark_sample_t *out_sample
)
{
    bool capture_refresh = measured &&
        (bench_case->flags & (GRAPE_BENCHMARK_CASE_PRESENT |
                              GRAPE_BENCHMARK_CASE_CAPTURE_REFRESH_WAIT)) != 0U;
    uint64_t refresh_before = capture_refresh
        ? grape_telemetry_timer_cumulative_us(
            GRAPE_TELEMETRY_TIMER_DISPLAY_REFRESH_WAIT
        )
        : 0;

    if (measured) {
        grape_benchmark_function_profile_start();
    }

    int64_t total_start = measured ? esp_timer_get_time() : 0;
    int64_t work_start = total_start;

    esp_err_t ret = bench_case->iteration
        ? bench_case->iteration(runtime, bench_case, state, sequence_iteration)
        : ESP_OK;

    int64_t work_end = measured ? esp_timer_get_time() : 0;
    if (ret != ESP_OK) {
        if (measured) {
            grape_benchmark_function_profile_stop();
        }
        return ret;
    }

    uint32_t present_us = 0;
    if ((bench_case->flags & GRAPE_BENCHMARK_CASE_PRESENT) != 0U) {
        int64_t present_start = measured ? esp_timer_get_time() : 0;
        ret = grape_present(runtime->grape);
        int64_t present_end = measured ? esp_timer_get_time() : 0;
        if (ret != ESP_OK) {
            if (measured) {
                grape_benchmark_function_profile_stop();
            }
            return ret;
        }
        if (measured) {
            present_us = elapsed_u32(present_start, present_end);
        }
    }

    int64_t total_end = measured ? esp_timer_get_time() : 0;
    uint64_t refresh_after = capture_refresh
        ? grape_telemetry_timer_cumulative_us(
            GRAPE_TELEMETRY_TIMER_DISPLAY_REFRESH_WAIT
        )
        : refresh_before;

    if (measured) {
        grape_benchmark_function_profile_stop();
    }

    if (bench_case->after_iteration) {
        bench_case->after_iteration(
            runtime,
            bench_case,
            state,
            sequence_iteration
        );
    }

    if (measured && out_sample) {
        uint64_t refresh_delta = refresh_after - refresh_before;
        if (refresh_delta > UINT32_MAX) {
            refresh_delta = UINT32_MAX;
        }
        *out_sample = (grape_benchmark_sample_t) {
            .work_us = elapsed_u32(work_start, work_end),
            .present_us = present_us,
            .total_us = elapsed_u32(total_start, total_end),
            .refresh_wait_us = (uint32_t)refresh_delta,
        };
    }

    return ESP_OK;
}

static void benchmark_service(grape_benchmark_runtime_t *runtime)
{
    int64_t now = esp_timer_get_time();
    if (now - runtime->service_last_us >= GRAPE_BENCHMARK_SERVICE_INTERVAL_US) {
        vTaskDelay(1);
        runtime->service_last_us = esp_timer_get_time();
    }
}

static esp_err_t run_warmup(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state,
    uint32_t warmup_iterations
)
{
    for (uint32_t i = 0; i < warmup_iterations; ++i) {
        benchmark_service(runtime);
        esp_err_t ret = execute_iteration(
            runtime,
            bench_case,
            state,
            i,
            false,
            NULL
        );
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
    uint32_t warmup_iterations,
    uint32_t measured_iterations,
    grape_benchmark_result_t *result,
    grape_benchmark_sample_t *samples
)
{
    memset(result, 0, sizeof(*result));
    grape_telemetry_reset();
    grape_benchmark_function_profile_reset();

    uint64_t measured_elapsed_us = 0;
    for (uint32_t i = 0; i < measured_iterations; ++i) {
        benchmark_service(runtime);
        uint32_t sequence = warmup_iterations + i;

        esp_err_t ret = execute_iteration(
            runtime,
            bench_case,
            state,
            sequence,
            true,
            &samples[i]
        );
        if (ret != ESP_OK) {
            return ret;
        }
        samples[i].iteration_index = i;
        measured_elapsed_us += samples[i].total_us;
    }
    result->iterations = measured_iterations;
    result->elapsed_us = measured_elapsed_us;

    result->work = distribution_from_samples(
        samples,
        measured_iterations,
        offsetof(grape_benchmark_sample_t, work_us)
    );
    result->present = distribution_from_samples(
        samples,
        measured_iterations,
        offsetof(grape_benchmark_sample_t, present_us)
    );
    result->total = distribution_from_samples(
        samples,
        measured_iterations,
        offsetof(grape_benchmark_sample_t, total_us)
    );
    result->refresh_wait = distribution_from_samples(
        samples,
        measured_iterations,
        offsetof(grape_benchmark_sample_t, refresh_wait_us)
    );

    grape_telemetry_snapshot(&result->telemetry);

    if (bench_case->collect_metrics) {
        result->metric_count = bench_case->collect_metrics(
            runtime,
            bench_case,
            state,
            result->metrics,
            GRAPE_BENCHMARK_MAX_METRICS
        );
        if (result->metric_count > GRAPE_BENCHMARK_MAX_METRICS) {
            result->metric_count = GRAPE_BENCHMARK_MAX_METRICS;
        }
    }

    return ESP_OK;
}

static esp_err_t reset_between_cases(grape_benchmark_runtime_t *runtime)
{
    grape_benchmark_damage_clear(runtime->grape);
    esp_err_t ret = grape_invalidate_all(runtime->grape);
    if (ret == ESP_OK) {
        ret = grape_present(runtime->grape);
    }
    grape_benchmark_damage_clear(runtime->grape);
    return ret;
}

static esp_err_t run_case(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case
)
{
    void *state = NULL;
    grape_benchmark_sample_t *samples = NULL;
    grape_benchmark_result_t *result = NULL;

    esp_err_t ret = bench_case->setup
        ? bench_case->setup(runtime, bench_case, &state)
        : ESP_OK;
    benchmark_note_stack_headroom(runtime, bench_case, "setup");

    if (ret == ESP_ERR_NOT_SUPPORTED) {
        grape_benchmark_report_skip(runtime, bench_case, ret);
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Setup failed for %s/%s: %s",
                 bench_case->group, bench_case->name, esp_err_to_name(ret));
        return ret;
    }

    uint32_t warmup_iterations = resolve_warmup_iterations(runtime, bench_case);
    uint32_t measured_iterations = resolve_measured_iterations(runtime, bench_case);
    if (measured_iterations == 0) {
        ret = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    runtime->service_last_us = esp_timer_get_time();
    ret = run_warmup(runtime, bench_case, state, warmup_iterations);
    benchmark_note_stack_headroom(runtime, bench_case, "warmup");
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            grape_benchmark_report_skip(runtime, bench_case, ret);
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "Warmup failed for %s/%s: %s",
                     bench_case->group, bench_case->name, esp_err_to_name(ret));
        }
        goto cleanup;
    }

    samples = calloc(measured_iterations, sizeof(*samples));
    if (!samples) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    result = calloc(1, sizeof(*result));
    if (!result) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    if (bench_case->before_measurement) {
        ret = bench_case->before_measurement(runtime, bench_case, state);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Measurement setup failed for %s/%s: %s",
                     bench_case->group, bench_case->name, esp_err_to_name(ret));
            goto cleanup;
        }
    }

    ret = run_measured(
        runtime,
        bench_case,
        state,
        warmup_iterations,
        measured_iterations,
        result,
        samples
    );
    benchmark_note_stack_headroom(runtime, bench_case, "measurement");

    if (ret == ESP_ERR_NOT_SUPPORTED) {
        grape_benchmark_report_skip(runtime, bench_case, ret);
        ret = ESP_OK;
    } else if (ret == ESP_OK) {
        grape_benchmark_report_function_profile(
            runtime,
            bench_case,
            result->iterations
        );
        grape_benchmark_report_case(runtime, bench_case, result, samples);
    } else {
        ESP_LOGE(TAG, "Measurement failed for %s/%s: %s",
                 bench_case->group, bench_case->name, esp_err_to_name(ret));
    }

    if (ret == ESP_OK && runtime->report_error != ESP_OK) {
        ret = runtime->report_error;
    }

cleanup:
    free(result);
    free(samples);

    if (ret == ESP_OK && runtime->report_error != ESP_OK) {
        ret = runtime->report_error;
    }

    if (bench_case->teardown) {
        bench_case->teardown(runtime, bench_case, state);
    }
    benchmark_note_stack_headroom(runtime, bench_case, "teardown");

    if (ret == ESP_OK) {
        esp_err_t reset_ret = reset_between_cases(runtime);
        if (reset_ret != ESP_OK) {
            ret = reset_ret;
        }
    }

    if (runtime->config.case_cooldown_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(runtime->config.case_cooldown_ms));
    }
    return ret;
}

static esp_err_t capture_references(grape_benchmark_runtime_t *runtime)
{
    if (!GRAPE_BENCHMARK_CAPTURE_REFERENCES) return ESP_OK;
    ESP_LOGI(TAG, "Timing complete. Starting separate deterministic reference replay.");
    size_t suite_count = 0;
    const grape_benchmark_suite_t *suites = grape_benchmark_suites(&suite_count);
    for (size_t s = 0; s < suite_count; ++s) {
        if (!(runtime->config.suite_mask & suites[s].mask)) continue;
        size_t count = 0;
        const grape_benchmark_case_t *cases = suites[s].cases(&count);
        for (size_t c = 0; c < count; ++c) {
            const grape_benchmark_case_t *bc = &cases[c];
            if (!bc->capture_reference) continue;
            ESP_LOGI(TAG, "Reference replay: %s/%s", bc->group, bc->name);
            void *state = NULL;
            esp_err_t ret = bc->setup ? bc->setup(runtime, bc, &state) : ESP_OK;
            if (ret == ESP_ERR_NOT_SUPPORTED) continue;
            if (ret != ESP_OK) return ret;
            for (uint32_t frame = 0; frame < GRAPE_BENCHMARK_REFERENCE_FRAMES; ++frame) {
                ret = execute_iteration(runtime, bc, state, frame, false, NULL);
                if (ret == ESP_OK) ret = bc->capture_reference(runtime, bc, state, frame);
                if (ret != ESP_OK) break;
                vTaskDelay(1);
            }
            if (bc->teardown) bc->teardown(runtime, bc, state);
            if (ret != ESP_OK) return ret;
            ret = reset_between_cases(runtime);
            if (ret != ESP_OK) return ret;
        }
    }
    return ESP_OK;
}

esp_err_t grape_benchmark_run(
    grape_context_t *grape,
    const grape_benchmark_config_t *config
)
{
    if (!grape) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_benchmark_runtime_t runtime = {
        .grape = grape,
        .config = GRAPE_BENCHMARK_CONFIG_DEFAULT(),
        .stack_min_free_bytes = UINT32_MAX,
        .references_pending = GRAPE_BENCHMARK_CAPTURE_REFERENCES != 0,
    };
    if (config) {
        runtime.config = *config;
    }

    esp_err_t ret = grape_benchmark_validate_registry();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Benchmark registry validation failed");
        return ret;
    }

    grape_benchmark_environment_t environment;
    ret = prepare_environment(&runtime, &environment);
    if (ret != ESP_OK) {
        restore_environment(&runtime, &environment);
        return ret;
    }

    ret = reset_between_cases(&runtime);
    if (ret != ESP_OK) {
        restore_environment(&runtime, &environment);
        return ret;
    }

    ret = grape_benchmark_report_open(&runtime);
    if (ret != ESP_OK) {
        restore_environment(&runtime, &environment);
        return ret;
    }

    size_t suite_count = 0;
    const grape_benchmark_suite_t *suites = grape_benchmark_suites(&suite_count);

    size_t selected_case_count = 0;
    for (size_t suite_index = 0; suite_index < suite_count; ++suite_index) {
        if ((runtime.config.suite_mask & suites[suite_index].mask) == 0U) {
            continue;
        }
        size_t case_count = 0;
        suites[suite_index].cases(&case_count);
        selected_case_count += case_count;
    }

    ESP_LOGI(TAG,
             "Starting deterministic benchmark: %u cases, seed=0x%08" PRIx32
             ", dt=%" PRIu32 " us",
             (unsigned)selected_case_count,
             runtime.config.seed,
             runtime.config.fixed_dt_us);
    benchmark_note_stack_headroom(&runtime, NULL, NULL);

#if GRAPE_TELEMETRY_LEVEL < 2
    ESP_LOGW(TAG,
             "Telemetry level %d: detailed backend timing columns will be zero",
             GRAPE_TELEMETRY_LEVEL);
#endif

    if (grape_benchmark_function_profile_enabled()) {
        ESP_LOGW(TAG,
                 "Function profiling enabled: benchmark timing results are instrumented and not comparable to normal runs");
    }

    size_t global_case_index = 0;
    for (size_t suite_index = 0; suite_index < suite_count; ++suite_index) {
        const grape_benchmark_suite_t *suite = &suites[suite_index];
        if ((runtime.config.suite_mask & suite->mask) == 0U) {
            continue;
        }

        size_t case_count = 0;
        const grape_benchmark_case_t *cases = suite->cases(&case_count);
        ESP_LOGI(TAG, "Suite %s: %u cases", suite->name, (unsigned)case_count);

        for (size_t case_index = 0; case_index < case_count; ++case_index) {
            ++global_case_index;
            ESP_LOGI(TAG,
                     "[%u/%u] %s/%s",
                     (unsigned)global_case_index,
                     (unsigned)selected_case_count,
                     cases[case_index].group,
                     cases[case_index].name);
            ret = run_case(&runtime, &cases[case_index]);
            if (ret != ESP_OK) {
                grape_benchmark_report_close(&runtime);
                restore_environment(&runtime, &environment);
                return ret;
            }
        }
    }

    ESP_LOGI(TAG,
             "Benchmark minimum main-task stack headroom: %" PRIu32 " bytes",
             runtime.stack_min_free_bytes);

    /* Save timings before any optional reference replay can fail. */
    ret = grape_benchmark_report_save_wait(&runtime);
    if (ret == ESP_OK) ret = capture_references(&runtime);
    if (ret == ESP_OK) grape_benchmark_report_references_complete(&runtime);
    if (ret == ESP_OK) ret = grape_benchmark_report_save_wait(&runtime);
    restore_environment(&runtime, &environment);
    grape_benchmark_report_close(&runtime);
    return ret;
}
