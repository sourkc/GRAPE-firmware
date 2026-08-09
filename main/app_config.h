#pragma once

/*
 * Set to 1 to run the automated GRAPE benchmark at boot.
 * Set to 0 to run the existing interactive/demo animation in main.c.
 */
#define GRAPE_APP_RUN_BENCHMARK 0
#define GRAPE_APP_BENCHMARK_SUITE_MASK GRAPE_BENCHMARK_SUITE_ALL

/* First vector-path proof of life. */
#define GRAPE_APP_RUN_VECTOR_DEMO 1

/* Demo diagnostics. */
#define GRAPE_APP_DAMAGE_STATS_INTERVAL_MS 5000
