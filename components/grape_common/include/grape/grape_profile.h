#pragma once

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
    GRAPE_PROFILE_METRIC_DISPLAY_BLIT,
    GRAPE_PROFILE_METRIC_LCD_DRAW_SUBMIT,
    GRAPE_PROFILE_METRIC_LCD_DRAW_WAIT,
    GRAPE_PROFILE_METRIC_COUNT,
} grape_profile_metric_t;

#if GRAPE_PROFILE_ENABLE

static inline int64_t grape_profile_timestamp(void)
{
    return esp_timer_get_time();
}

void grape_profile_record(grape_profile_metric_t metric, int64_t elapsed_us);
void grape_profile_report_if_due(void);
void grape_profile_reset(void);

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

#endif

#ifdef __cplusplus
}
#endif
