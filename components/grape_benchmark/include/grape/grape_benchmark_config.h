#pragma once

/*
 * Benchmark defaults.
 *
 * The runner is intentionally independent from SD mounting. If /sdcard is
 * mounted before grape_benchmark_run(), CSV files are written there. If it
 * is not mounted, the benchmark still runs and prints summaries to the log.
 */

#define GRAPE_BENCHMARK_OUTPUT_DIRECTORY            "/sdcard"

#define GRAPE_BENCHMARK_WARMUP_FRAMES               8
#define GRAPE_BENCHMARK_MEASURED_FRAMES             45
#define GRAPE_BENCHMARK_CASE_COOLDOWN_MS            30

#define GRAPE_BENCHMARK_WRITE_SUMMARY_CSV           1
#define GRAPE_BENCHMARK_WRITE_SAMPLES_CSV           1
#define GRAPE_BENCHMARK_LOG_EACH_CASE               1

#define GRAPE_BENCHMARK_SUITE_BASIC                 1
#define GRAPE_BENCHMARK_SUITE_MOVEMENT              1
#define GRAPE_BENCHMARK_SUITE_OVERLAP               1
#define GRAPE_BENCHMARK_SUITE_OPACITY               1
#define GRAPE_BENCHMARK_SUITE_SCALE                 1
#define GRAPE_BENCHMARK_SUITE_ROTATION              1
#define GRAPE_BENCHMARK_SUITE_ROTATION_SCALE        1

#define GRAPE_BENCHMARK_MOVEMENT_CYCLE_FRAMES        60
#define GRAPE_BENCHMARK_BUILD_LABEL                  "dev"
