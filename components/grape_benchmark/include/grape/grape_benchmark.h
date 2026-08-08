#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "grape/grape.h"
#include "grape/grape_benchmark_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *output_directory;
    uint32_t warmup_frames;
    uint32_t measured_frames;
    uint32_t case_cooldown_ms;
    bool write_summary_csv;
    bool write_samples_csv;
    bool log_each_case;
} grape_benchmark_config_t;

#define GRAPE_BENCHMARK_CONFIG_DEFAULT()                         \
    {                                                            \
        .output_directory = GRAPE_BENCHMARK_OUTPUT_DIRECTORY,    \
        .warmup_frames = GRAPE_BENCHMARK_WARMUP_FRAMES,          \
        .measured_frames = GRAPE_BENCHMARK_MEASURED_FRAMES,      \
        .case_cooldown_ms = GRAPE_BENCHMARK_CASE_COOLDOWN_MS,    \
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
