#pragma once

/*
 * Set to 1 to run the automated GRAPE benchmark at boot.
 * Set to 0 to run the existing interactive/demo animation in main.c.
 */
#define GRAPE_APP_RUN_BENCHMARK 0
#define GRAPE_APP_BENCHMARK_SUITE_MASK GRAPE_BENCHMARK_SUITE_ALL


/* Shader language demo (Mandelbrot). */
#define GRAPE_APP_RUN_SHADER_DEMO 1

/* First TrueType glyph proof of life. */
#define GRAPE_APP_RUN_FONT_DEMO 0

/* SVG document proof of life. */
#define GRAPE_APP_RUN_VECTOR_DEMO 0

/* Demo diagnostics. */
#define GRAPE_APP_DAMAGE_STATS_INTERVAL_MS 5000

#define EXPERIMENTAL_SET_CLOCK_400_MHZ 0
