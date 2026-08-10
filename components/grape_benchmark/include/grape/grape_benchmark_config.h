#pragma once

#define GRAPE_BENCHMARK_OUTPUT_DIRECTORY             "/sdcard"
#define GRAPE_BENCHMARK_WARMUP_ITERATIONS            6
#define GRAPE_BENCHMARK_MEASURED_ITERATIONS          32
#define GRAPE_BENCHMARK_CASE_COOLDOWN_MS             20
#define GRAPE_BENCHMARK_FIXED_DT_US                   16667U
#define GRAPE_BENCHMARK_SEED                          0x47524150U
#define GRAPE_BENCHMARK_WDT_TIMEOUT_MS                60000U
#define GRAPE_BENCHMARK_SD_RETRY_MS                    1000U

#define GRAPE_BENCHMARK_WRITE_SUMMARY_CSV            1
#define GRAPE_BENCHMARK_WRITE_SAMPLES_CSV            1
#define GRAPE_BENCHMARK_LOG_EACH_CASE                1
#define GRAPE_BENCHMARK_BUILD_LABEL                  "coverage-v3"
