#pragma once

/*
 * GRAPE telemetry level:
 *   0 - disabled
 *   1 - coarse frame/damage/presentation timings
 *   2 - detailed renderer timings
 */
#ifndef GRAPE_TELEMETRY_LEVEL
#define GRAPE_TELEMETRY_LEVEL 0
#endif

#if GRAPE_TELEMETRY_LEVEL < 0 || GRAPE_TELEMETRY_LEVEL > 2
#error "GRAPE_TELEMETRY_LEVEL must be 0, 1, or 2"
#endif

#ifndef GRAPE_TELEMETRY_REPORT_INTERVAL_MS
#define GRAPE_TELEMETRY_REPORT_INTERVAL_MS 1000
#endif

#ifndef GRAPE_TELEMETRY_AUTO_REPORT_ENABLE
#define GRAPE_TELEMETRY_AUTO_REPORT_ENABLE (GRAPE_TELEMETRY_LEVEL >= 2)
#endif
