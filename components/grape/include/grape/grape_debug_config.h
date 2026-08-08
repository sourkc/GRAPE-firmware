#pragma once

/*
 * GRAPE debug/diagnostic configuration.
 *
 * Set to 1 to enable the lightweight damage diagnostics used by the demo:
 * - mark/plan timing inside the damage system
 * - periodic DAMAGE: reports in main.c
 *
 * Set to 0 for clean performance measurements. The timing calls and demo
 * reporting code are compiled out.
 */
#define GRAPE_DAMAGE_DIAGNOSTICS_ENABLE 0
