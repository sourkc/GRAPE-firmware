#pragma once

/*
 * Lightweight GRAPE diagnostics.
 *
 * Set to 1 while investigating damage/presentation costs. This enables
 * timestamping for damage mark/plan work, back-buffer synchronization,
 * and refresh-boundary waits, plus the demo's periodic DAMAGE report.
 * Set to 0 for clean performance measurements.
 */
#define GRAPE_DAMAGE_DIAGNOSTICS_ENABLE 1
