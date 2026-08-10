#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "grape/grape.h"
#include "grape/grape_benchmark_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GRAPE_BENCHMARK_SUITE_DAMAGE_MARK      = 1u << 0,
    GRAPE_BENCHMARK_SUITE_DAMAGE_PLAN      = 1u << 1,
    GRAPE_BENCHMARK_SUITE_COMPOSITOR       = 1u << 2,
    GRAPE_BENCHMARK_SUITE_PIXEL_BACKENDS   = 1u << 3,
    GRAPE_BENCHMARK_SUITE_THREE_SHEAR      = 1u << 4,
    GRAPE_BENCHMARK_SUITE_FRAGMENTATION    = 1u << 5,
    GRAPE_BENCHMARK_SUITE_PRESENTATION     = 1u << 6,
    GRAPE_BENCHMARK_SUITE_LIFECYCLE        = 1u << 7,
    GRAPE_BENCHMARK_SUITE_SCENES           = 1u << 8,
    GRAPE_BENCHMARK_SUITE_VECTOR           = 1u << 9,
    GRAPE_BENCHMARK_SUITE_SVG              = 1u << 10,
    GRAPE_BENCHMARK_SUITE_GLYPH_CACHE      = 1u << 11,
    GRAPE_BENCHMARK_SUITE_TEXT             = 1u << 12,
    GRAPE_BENCHMARK_SUITE_FONT             = 1u << 13,
    GRAPE_BENCHMARK_SUITE_ALL              = (1u << 14) - 1u,
} grape_benchmark_suite_mask_t;

typedef struct {
    const char *output_directory;
    uint32_t warmup_iterations;
    uint32_t measured_iterations;
    uint32_t case_cooldown_ms;
    uint32_t fixed_dt_us;
    uint32_t seed;
    uint32_t suite_mask;
    bool write_summary_csv;
    bool write_samples_csv;
    bool log_each_case;
} grape_benchmark_config_t;

#define GRAPE_BENCHMARK_CONFIG_DEFAULT()                         \
    {                                                            \
        .output_directory = GRAPE_BENCHMARK_OUTPUT_DIRECTORY,    \
        .warmup_iterations = GRAPE_BENCHMARK_WARMUP_ITERATIONS,  \
        .measured_iterations = GRAPE_BENCHMARK_MEASURED_ITERATIONS, \
        .case_cooldown_ms = GRAPE_BENCHMARK_CASE_COOLDOWN_MS,    \
        .fixed_dt_us = GRAPE_BENCHMARK_FIXED_DT_US,              \
        .seed = GRAPE_BENCHMARK_SEED,                            \
        .suite_mask = GRAPE_BENCHMARK_SUITE_ALL,                 \
        .write_summary_csv = GRAPE_BENCHMARK_WRITE_SUMMARY_CSV,  \
        .write_samples_csv = GRAPE_BENCHMARK_WRITE_SAMPLES_CSV,  \
        .log_each_case = GRAPE_BENCHMARK_LOG_EACH_CASE,          \
    }

esp_err_t grape_benchmark_run(
    grape_context_t *grape,
    const grape_benchmark_config_t *config
);

#ifdef __cplusplus
}
#endif
