#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "grape/grape_telemetry_config.h"

#if GRAPE_TELEMETRY_LEVEL > 0
#include "esp_timer.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GRAPE_TELEMETRY_TIMER_LIST(X) \
    X(PRESENT,              1, "frame.present",            "present") \
    X(DAMAGE_MARK,          1, "damage.mark",              "damage_mark") \
    X(DAMAGE_PLAN,          1, "damage.plan",              "damage_plan") \
    X(DISPLAY_REFRESH_WAIT, 1, "display.refresh_wait",     "display_refresh_wait") \
    X(SURFACE_TRANSFORM,    2, "surface.transform",        "surface_transform") \
    X(SURFACE_RECACHE,      2, "surface.recache",          "surface_recache") \
    X(COMPOSITOR,           2, "render.compositor",        "compositor") \
    X(PPA_FILL,             2, "render.ppa_fill",          "ppa_fill") \
    X(CPU_FILL,             2, "render.cpu_fill",          "cpu_fill") \
    X(PPA_BLEND_DISPATCH,   2, "render.ppa_blend_dispatch", "ppa_blend_dispatch") \
    X(PPA_BLEND_HW,         2, "render.ppa_blend_hw",      "ppa_blend_hw") \
    X(PPA_ROTATE,           2, "render.ppa_rotate",        "ppa_rotate") \
    X(CPU_SURFACE_RASTER,   2, "render.cpu_surface_raster", "cpu_surface_raster") \
    X(SHEAR_PREP,           2, "shear.prep",               "shear_prep") \
    X(SHEAR_X1,             2, "shear.x1",                 "shear_x1") \
    X(SHEAR_Y,              2, "shear.y",                  "shear_y") \
    X(SHEAR_X2,             2, "shear.x2",                 "shear_x2") \
    X(SHEAR_CLEAR,          2, "shear.clear",              "shear_clear") \
    X(SHEAR_QUARTER_TURN,   2, "shear.quarter_turn",       "shear_quarter_turn") \
    X(SHEAR_COMPOSITE,      2, "shear.composite",          "shear_composite") \
    X(DISPLAY_SUBMIT,       2, "display.submit",           "display_submit")

typedef enum {
#define GRAPE_TELEMETRY_ENUM(name, level, label, csv) GRAPE_TELEMETRY_TIMER_##name,
    GRAPE_TELEMETRY_TIMER_LIST(GRAPE_TELEMETRY_ENUM)
#undef GRAPE_TELEMETRY_ENUM
    GRAPE_TELEMETRY_TIMER_COUNT,
} grape_telemetry_timer_t;

enum {
#define GRAPE_TELEMETRY_LEVEL_ENUM(name, level, label, csv) \
    GRAPE_TELEMETRY_TIMER_LEVEL_##name = level,
    GRAPE_TELEMETRY_TIMER_LIST(GRAPE_TELEMETRY_LEVEL_ENUM)
#undef GRAPE_TELEMETRY_LEVEL_ENUM
};

typedef struct {
    uint64_t total_us;
    uint64_t max_us;
    uint32_t calls;
} grape_telemetry_stat_t;

typedef struct {
    grape_telemetry_stat_t timers[GRAPE_TELEMETRY_TIMER_COUNT];
} grape_telemetry_snapshot_t;

const char *grape_telemetry_timer_name(grape_telemetry_timer_t timer);
const char *grape_telemetry_timer_csv_name(grape_telemetry_timer_t timer);
uint8_t grape_telemetry_timer_level(grape_telemetry_timer_t timer);
void grape_telemetry_record(grape_telemetry_timer_t timer, int64_t elapsed_us);
void grape_telemetry_reset(void);
void grape_telemetry_snapshot(grape_telemetry_snapshot_t *out_snapshot);
void grape_telemetry_set_auto_report(bool enabled);
bool grape_telemetry_auto_report_enabled(void);

#if GRAPE_TELEMETRY_AUTO_REPORT_ENABLE
void grape_telemetry_report_if_due(void);
#else
static inline void grape_telemetry_report_if_due(void)
{
}
#endif

#if GRAPE_TELEMETRY_LEVEL > 0
uint64_t grape_telemetry_timer_cumulative_us(grape_telemetry_timer_t timer);
#else
static inline uint64_t grape_telemetry_timer_cumulative_us(
    grape_telemetry_timer_t timer
)
{
    (void)timer;
    return 0;
}
#endif

#define GRAPE_TELEMETRY_CONCAT_INNER(a, b) a##b
#define GRAPE_TELEMETRY_CONCAT(a, b) GRAPE_TELEMETRY_CONCAT_INNER(a, b)

#if GRAPE_TELEMETRY_LEVEL > 0

typedef struct {
    grape_telemetry_timer_t timer;
    int64_t start_us;
    bool active;
    bool loop_once;
} grape_telemetry_scope_t;

static inline __attribute__((always_inline)) grape_telemetry_scope_t grape_telemetry_scope_begin(
    grape_telemetry_timer_t timer,
    bool enabled
)
{
    if (!enabled) {
        return (grape_telemetry_scope_t){
            .loop_once = true,
        };
    }

    return (grape_telemetry_scope_t){
        .timer = timer,
        .start_us = esp_timer_get_time(),
        .active = true,
        .loop_once = true,
    };
}

static inline __attribute__((always_inline)) void grape_telemetry_scope_cleanup(grape_telemetry_scope_t *scope)
{
    if (!scope || !scope->active) {
        return;
    }

    grape_telemetry_record(
        scope->timer,
        esp_timer_get_time() - scope->start_us
    );
}

#define GRAPE_TIME_SCOPE(name) \
    GRAPE_TIME_SCOPE_IMPL(name, __COUNTER__)
#define GRAPE_TIME_SCOPE_IMPL(name, counter) \
    GRAPE_TIME_SCOPE_IMPL2(name, counter)
#define GRAPE_TIME_SCOPE_IMPL2(name, counter) \
    grape_telemetry_scope_t GRAPE_TELEMETRY_CONCAT(_grape_time_scope_, counter) \
        __attribute__((cleanup(grape_telemetry_scope_cleanup))) = \
            grape_telemetry_scope_begin( \
                GRAPE_TELEMETRY_TIMER_##name, \
                GRAPE_TELEMETRY_LEVEL >= GRAPE_TELEMETRY_TIMER_LEVEL_##name \
            )

#define GRAPE_TIME_BLOCK(name) \
    GRAPE_TIME_BLOCK_IMPL(name, __COUNTER__)
#define GRAPE_TIME_BLOCK_IMPL(name, counter) \
    GRAPE_TIME_BLOCK_IMPL2(name, counter)
#define GRAPE_TIME_BLOCK_IMPL2(name, counter) \
    for (grape_telemetry_scope_t GRAPE_TELEMETRY_CONCAT(_grape_time_block_, counter) \
             __attribute__((cleanup(grape_telemetry_scope_cleanup))) = \
                 grape_telemetry_scope_begin( \
                     GRAPE_TELEMETRY_TIMER_##name, \
                     GRAPE_TELEMETRY_LEVEL >= GRAPE_TELEMETRY_TIMER_LEVEL_##name \
                 ); \
         GRAPE_TELEMETRY_CONCAT(_grape_time_block_, counter).loop_once; \
         GRAPE_TELEMETRY_CONCAT(_grape_time_block_, counter).loop_once = false)

#else

#define GRAPE_TIME_SCOPE(name) do { } while (0)
#define GRAPE_TIME_BLOCK(name) \
    GRAPE_TIME_BLOCK_DISABLED_IMPL(__COUNTER__)
#define GRAPE_TIME_BLOCK_DISABLED_IMPL(counter) \
    GRAPE_TIME_BLOCK_DISABLED_IMPL2(counter)
#define GRAPE_TIME_BLOCK_DISABLED_IMPL2(counter) \
    for (bool GRAPE_TELEMETRY_CONCAT(_grape_time_block_disabled_, counter) = true; \
         GRAPE_TELEMETRY_CONCAT(_grape_time_block_disabled_, counter); \
         GRAPE_TELEMETRY_CONCAT(_grape_time_block_disabled_, counter) = false)

#endif

#ifdef __cplusplus
}
#endif
