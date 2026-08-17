#pragma once

/*
 * Application operating mode:
 *   0 - GFXLINK server
 *   1 - benchmark suite
 *   2 - demo registry
 */
#define GRAPE_APP_MODE_GFXLINK   0U
#define GRAPE_APP_MODE_BENCHMARK 1U
#define GRAPE_APP_MODE_DEMO      2U

#define GRAPE_APP_MODE GRAPE_APP_MODE_DEMO

/*
 * Demo ID used when GRAPE_APP_MODE == GRAPE_APP_MODE_DEMO:
 *   0 - moving_squares
 *   1 - shader_raymarch
 *   2 - font
 *   3 - vector_svg
 *   4 - gpu_3d_triangle
 *   5 - surface_features
 *   6 - aa
 */
#define GRAPE_APP_DEMO 6U

/* Benchmark suites used when GRAPE_APP_MODE == GRAPE_APP_MODE_BENCHMARK. */
#define GRAPE_APP_BENCHMARK_SUITE_MASK GRAPE_BENCHMARK_SUITE_ALL

#define EXPERIMENTAL_SET_CLOCK_400_MHZ 0
