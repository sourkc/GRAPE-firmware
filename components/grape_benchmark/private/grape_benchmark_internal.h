#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "grape/grape_benchmark.h"
#include "grape/grape_benchmark_hooks.h"
#include "grape/grape_telemetry.h"

#define GRAPE_BENCHMARK_MAX_PARAMS 6
#define GRAPE_BENCHMARK_MAX_METRICS 12

typedef enum {
    GRAPE_BENCHMARK_KIND_MICRO = 0,
    GRAPE_BENCHMARK_KIND_PIPELINE,
    GRAPE_BENCHMARK_KIND_SCENE,
    GRAPE_BENCHMARK_KIND_LIFECYCLE,
} grape_benchmark_kind_t;

typedef enum {
    GRAPE_BENCHMARK_CASE_PRESENT = 1u << 0,
    GRAPE_BENCHMARK_CASE_CAPTURE_REFRESH_WAIT = 1u << 1,
} grape_benchmark_case_flags_t;

typedef struct {
    const char *name;
    double value;
} grape_benchmark_param_t;

typedef struct {
    const char *name;
    const char *unit;
    double value;
} grape_benchmark_metric_t;

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
    bool enabled;
} grape_benchmark_text_buffer_t;

typedef struct grape_benchmark_runtime grape_benchmark_runtime_t;
typedef struct grape_benchmark_case grape_benchmark_case_t;

typedef esp_err_t (*grape_benchmark_setup_fn)(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void **out_state
);

typedef esp_err_t (*grape_benchmark_iteration_fn)(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state,
    uint32_t sequence_iteration
);

typedef void (*grape_benchmark_after_iteration_fn)(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state,
    uint32_t sequence_iteration
);

typedef esp_err_t (*grape_benchmark_before_measurement_fn)(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state
);

typedef size_t (*grape_benchmark_collect_metrics_fn)(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state,
    grape_benchmark_metric_t *out_metrics,
    size_t capacity
);

typedef void (*grape_benchmark_teardown_fn)(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state
);

struct grape_benchmark_case {
    const char *group;
    const char *name;
    grape_benchmark_kind_t kind;
    uint32_t flags;
    uint32_t warmup_iterations;
    uint32_t measured_iterations;
    const void *user_data;
    grape_benchmark_setup_fn setup;
    grape_benchmark_iteration_fn iteration;
    grape_benchmark_after_iteration_fn after_iteration;
    grape_benchmark_before_measurement_fn before_measurement;
    grape_benchmark_collect_metrics_fn collect_metrics;
    grape_benchmark_teardown_fn teardown;
    grape_benchmark_param_t params[GRAPE_BENCHMARK_MAX_PARAMS];
};

typedef struct {
    uint32_t iteration_index;
    uint32_t work_us;
    uint32_t present_us;
    uint32_t total_us;
    uint32_t refresh_wait_us;
} grape_benchmark_sample_t;

typedef struct {
    double mean_us;
    double stddev_us;
    uint32_t min_us;
    uint32_t p50_us;
    uint32_t p95_us;
    uint32_t p99_us;
    uint32_t max_us;
} grape_benchmark_distribution_t;

typedef struct {
    uint32_t iterations;
    uint64_t elapsed_us;
    grape_benchmark_distribution_t work;
    grape_benchmark_distribution_t present;
    grape_benchmark_distribution_t total;
    grape_benchmark_distribution_t refresh_wait;
    grape_telemetry_snapshot_t telemetry;
    grape_benchmark_metric_t metrics[GRAPE_BENCHMARK_MAX_METRICS];
    size_t metric_count;
} grape_benchmark_result_t;

struct grape_benchmark_runtime {
    grape_context_t *grape;
    grape_benchmark_config_t config;
    grape_benchmark_text_buffer_t summary_buffer;
    grape_benchmark_text_buffer_t samples_buffer;
    grape_benchmark_text_buffer_t metadata_buffer;
    grape_benchmark_text_buffer_t function_profile_buffer;
    esp_err_t report_error;
    uint32_t stack_min_free_bytes;
};

typedef const grape_benchmark_case_t *(*grape_benchmark_case_provider_fn)(
    size_t *out_count
);

typedef struct {
    const char *name;
    uint32_t mask;
    grape_benchmark_case_provider_fn cases;
} grape_benchmark_suite_t;

uint32_t grape_benchmark_hash_u32(uint32_t value);
float grape_benchmark_unit_f32(uint32_t seed, uint32_t index);
float grape_benchmark_fixed_time_s(const grape_benchmark_runtime_t *runtime,
                                   uint32_t sequence_iteration);

esp_err_t grape_benchmark_report_open(grape_benchmark_runtime_t *runtime);
void grape_benchmark_report_close(grape_benchmark_runtime_t *runtime);
esp_err_t grape_benchmark_report_save_wait(grape_benchmark_runtime_t *runtime);
void grape_benchmark_report_metadata(grape_benchmark_runtime_t *runtime);
void grape_benchmark_report_case(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    const grape_benchmark_result_t *result,
    const grape_benchmark_sample_t *samples
);
void grape_benchmark_report_function_profile(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    uint32_t measured_iterations
);
void grape_benchmark_report_skip(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    esp_err_t reason
);

const grape_benchmark_case_t *grape_benchmark_damage_mark_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_damage_plan_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_compositor_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_pixel_backend_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_three_shear_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_fragmentation_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_presentation_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_lifecycle_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_scene_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_vector_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_svg_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_font_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_glyph_cache_cases(size_t *out_count);
const grape_benchmark_case_t *grape_benchmark_text_cases(size_t *out_count);

esp_err_t grape_benchmark_fixture_font_load(grape_font_t **out_font);
esp_err_t grape_benchmark_validate_registry(void);
const grape_benchmark_suite_t *grape_benchmark_suites(size_t *out_count);
