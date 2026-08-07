#include "grape/grape_profile.h"

#if GRAPE_PROFILE_ENABLE

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

#include "esp_log.h"

static const char *TAG = "grape_profile";

typedef struct {
    uint64_t total_us;
    uint64_t max_us;
    uint32_t calls;
} grape_profile_stat_t;

static grape_profile_stat_t s_stats[GRAPE_PROFILE_METRIC_COUNT];
static int64_t s_window_start_us;

static const char *metric_name(grape_profile_metric_t metric)
{
    static const char *const names[GRAPE_PROFILE_METRIC_COUNT] = {
        [GRAPE_PROFILE_METRIC_PRESENT] = "present",
        [GRAPE_PROFILE_METRIC_DAMAGE_ADD] = "damage add",
        [GRAPE_PROFILE_METRIC_SURFACE_TRANSFORM] = "surface transform",
        [GRAPE_PROFILE_METRIC_SURFACE_RECACHE] = "surface recache",
        [GRAPE_PROFILE_METRIC_COMPOSITOR] = "compositor rect",
        [GRAPE_PROFILE_METRIC_PPA_FILL] = "PPA fill",
        [GRAPE_PROFILE_METRIC_CPU_FILL] = "CPU fill fallback",
        [GRAPE_PROFILE_METRIC_PPA_BLEND_DISPATCH] = "PPA blend dispatch",
        [GRAPE_PROFILE_METRIC_PPA_BLEND_HW] = "PPA blend hardware",
        [GRAPE_PROFILE_METRIC_CPU_SURFACE_RASTER] = "CPU surface raster",
        [GRAPE_PROFILE_METRIC_DISPLAY_BLIT] = "display blit",
        [GRAPE_PROFILE_METRIC_LCD_DRAW_SUBMIT] = "LCD draw submit",
        [GRAPE_PROFILE_METRIC_LCD_DRAW_WAIT] = "LCD transfer wait",
    };

    if ((unsigned)metric >= GRAPE_PROFILE_METRIC_COUNT || !names[metric]) {
        return "unknown";
    }
    return names[metric];
}

void grape_profile_record(grape_profile_metric_t metric, int64_t elapsed_us)
{
    if ((unsigned)metric >= GRAPE_PROFILE_METRIC_COUNT || elapsed_us < 0) {
        return;
    }

    grape_profile_stat_t *stat = &s_stats[metric];
    uint64_t value = (uint64_t)elapsed_us;

    stat->total_us += value;
    stat->calls++;
    if (value > stat->max_us) {
        stat->max_us = value;
    }
}

void grape_profile_report_if_due(void)
{
    int64_t now = esp_timer_get_time();

    if (s_window_start_us == 0) {
        s_window_start_us = now;
        return;
    }

    int64_t window_us = now - s_window_start_us;
    if (window_us < (int64_t)GRAPE_PROFILE_REPORT_INTERVAL_MS * 1000LL) {
        return;
    }

    ESP_LOGI(TAG, "---- %.2f ms profile window (nested timings are not additive) ----",
             (double)window_us / 1000.0);

    for (int i = 0; i < GRAPE_PROFILE_METRIC_COUNT; ++i) {
        grape_profile_stat_t *stat = &s_stats[i];
        if (stat->calls == 0) {
            continue;
        }

        double total_ms = (double)stat->total_us / 1000.0;
        double avg_ms = total_ms / (double)stat->calls;
        double max_ms = (double)stat->max_us / 1000.0;
        double window_percent = ((double)stat->total_us * 100.0) / (double)window_us;

        ESP_LOGI(TAG,
                 "%-19s total=%8.3f ms avg=%7.3f ms max=%7.3f ms calls=%" PRIu32 " window=%6.1f%%",
                 metric_name((grape_profile_metric_t)i),
                 total_ms,
                 avg_ms,
                 max_ms,
                 stat->calls,
                 window_percent);
    }

    grape_profile_reset();
    s_window_start_us = now;
}

void grape_profile_reset(void)
{
    for (size_t i = 0; i < GRAPE_PROFILE_METRIC_COUNT; ++i) {
        s_stats[i] = (grape_profile_stat_t){0};
    }
}

#endif
