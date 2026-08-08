#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "grape/grape_profile_config.h"

#if GRAPE_PROFILE_ENABLE
#include "esp_timer.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GRAPE_PROFILE_METRIC_PRESENT = 0,
    GRAPE_PROFILE_METRIC_DAMAGE_ADD,
    GRAPE_PROFILE_METRIC_SURFACE_TRANSFORM,
    GRAPE_PROFILE_METRIC_SURFACE_RECACHE,
    GRAPE_PROFILE_METRIC_COMPOSITOR,
    GRAPE_PROFILE_METRIC_PPA_FILL,
    GRAPE_PROFILE_METRIC_CPU_FILL,
    GRAPE_PROFILE_METRIC_PPA_BLEND_DISPATCH,
    GRAPE_PROFILE_METRIC_PPA_BLEND_HW,
    GRAPE_PROFILE_METRIC_CPU_SURFACE_RASTER,
    GRAPE_PROFILE_METRIC_SHEAR_PREP,
    GRAPE_PROFILE_METRIC_SHEAR_X1,
    GRAPE_PROFILE_METRIC_SHEAR_Y,
    GRAPE_PROFILE_METRIC_SHEAR_X2,
    GRAPE_PROFILE_METRIC_SHEAR_CLEAR,
    GRAPE_PROFILE_METRIC_SHEAR_QUARTER_TURN,
    GRAPE_PROFILE_METRIC_SHEAR_COMPOSITE,
    GRAPE_PROFILE_METRIC_DISPLAY_BLIT,
    GRAPE_PROFILE_METRIC_LCD_DRAW_SUBMIT,
    GRAPE_PROFILE_METRIC_LCD_DRAW_WAIT,
    GRAPE_PROFILE_METRIC_COUNT,
} grape_profile_metric_t;

typedef struct {
    uint64_t total_us;
    uint64_t max_us;
    uint32_t calls;
} grape_profile_stat_t;

typedef struct {
    grape_profile_stat_t metrics[GRAPE_PROFILE_METRIC_COUNT];
} grape_profile_snapshot_t;

const char *grape_profile_metric_name(grape_profile_metric_t metric);

#if GRAPE_PROFILE_ENABLE

static inline int64_t grape_profile_timestamp(void)
{
    return esp_timer_get_time();
}

void grape_profile_record(grape_profile_metric_t metric, int64_t elapsed_us);
void grape_profile_report_if_due(void);
void grape_profile_reset(void);
void grape_profile_snapshot(grape_profile_snapshot_t *out_snapshot);
void grape_profile_set_auto_report(bool enabled);
bool grape_profile_auto_report_enabled(void);

#else

static inline int64_t grape_profile_timestamp(void)
{
    return 0;
}

static inline void grape_profile_record(grape_profile_metric_t metric, int64_t elapsed_us)
{
    (void)metric;
    (void)elapsed_us;
}

static inline void grape_profile_report_if_due(void)
{
}

static inline void grape_profile_reset(void)
{
}

static inline void grape_profile_snapshot(grape_profile_snapshot_t *out_snapshot)
{
    if (!out_snapshot) {
        return;
    }

    *out_snapshot = (grape_profile_snapshot_t){0};
}

static inline void grape_profile_set_auto_report(bool enabled)
{
    (void)enabled;
}

static inline bool grape_profile_auto_report_enabled(void)
{
    return false;
}

#endif

#ifdef __cplusplus
}
#endif
