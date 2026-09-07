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
 *   7 - screenshot (AA scene + PNG/JPEG to SD)
 *   8 - aa_rotate_checker
 *   9 - gpu_3d_cube (M2.2d optimized flat cube, 4x MSAA)
 *  10 - gpu_3d_textured_cube (M3 perspective-correct textures)
 */
#define GRAPE_APP_DEMO 1U

/* Benchmark suites used when GRAPE_APP_MODE == GRAPE_APP_MODE_BENCHMARK. */
#define GRAPE_APP_BENCHMARK_SUITE_MASK (GRAPE_BENCHMARK_REGRESSION_FOCUS ? \
    (GRAPE_BENCHMARK_SUITE_DAMAGE_PLAN | GRAPE_BENCHMARK_SUITE_PIXEL_BACKENDS | \
     GRAPE_BENCHMARK_SUITE_LIFECYCLE | GRAPE_BENCHMARK_SUITE_SCENES | \
     GRAPE_BENCHMARK_SUITE_TEXT | GRAPE_BENCHMARK_SUITE_BASELINE) : \
    GRAPE_BENCHMARK_SUITE_ALL)

#define EXPERIMENTAL_SET_CLOCK_400_MHZ 0
