#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "grape/grape_benchmark.h"
#include "grape/grape_telemetry.h"

#define GRAPE_BENCHMARK_MAX_PARAMS 4

typedef struct {
    const char *name;
    double value;
} grape_benchmark_param_t;

typedef struct grape_benchmark_runtime grape_benchmark_runtime_t;
typedef struct grape_benchmark_case grape_benchmark_case_t;

typedef esp_err_t (*grape_benchmark_setup_fn)(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void **out_state
);

typedef esp_err_t (*grape_benchmark_step_fn)(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state,
    uint32_t sequence_frame
);

typedef void (*grape_benchmark_teardown_fn)(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    void *state
);

struct grape_benchmark_case {
    const char *group;
    const char *name;
    const void *user_data;
    grape_benchmark_setup_fn setup;
    grape_benchmark_step_fn step;
    grape_benchmark_teardown_fn teardown;
    grape_benchmark_param_t params[GRAPE_BENCHMARK_MAX_PARAMS];
};

typedef struct {
    uint32_t frame_index;
    uint32_t update_us;
    uint32_t present_us;
    uint32_t frame_us;
} grape_benchmark_frame_sample_t;

typedef struct {
    uint32_t frames;
    uint64_t elapsed_us;
    uint64_t update_total_us;
    uint64_t present_total_us;
    uint64_t frame_total_us;
    uint32_t update_min_us;
    uint32_t update_max_us;
    uint32_t present_min_us;
    uint32_t present_max_us;
    uint32_t frame_min_us;
    uint32_t frame_max_us;
    grape_telemetry_snapshot_t telemetry;
} grape_benchmark_result_t;

struct grape_benchmark_runtime {
    grape_context_t *grape;
    grape_benchmark_config_t config;
    FILE *summary_csv;
    FILE *samples_csv;
};

esp_err_t grape_benchmark_report_open(grape_benchmark_runtime_t *runtime);
void grape_benchmark_report_close(grape_benchmark_runtime_t *runtime);
void grape_benchmark_report_metadata(grape_benchmark_runtime_t *runtime);
void grape_benchmark_report_case(
    grape_benchmark_runtime_t *runtime,
    const grape_benchmark_case_t *bench_case,
    const grape_benchmark_result_t *result,
    const grape_benchmark_frame_sample_t *samples
);

typedef const grape_benchmark_case_t *(*grape_benchmark_case_provider_fn)(
    size_t *out_count
);

typedef struct {
    const char *name;
    grape_benchmark_case_provider_fn cases;
} grape_benchmark_suite_t;

const grape_benchmark_case_t *grape_benchmark_shape_cases(size_t *out_count);
const grape_benchmark_suite_t *grape_benchmark_suites(size_t *out_count);
