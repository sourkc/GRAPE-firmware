#include "grape/grape_telemetry.h"

#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"

/* Per-tile timers can now finish on either core. Keep locks short; never log
 * or query the timer clock while holding this lock. */
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *const s_timer_names[GRAPE_TELEMETRY_TIMER_COUNT] = {
#define GRAPE_TELEMETRY_NAME(name, level, label, csv) \
    [GRAPE_TELEMETRY_TIMER_##name] = label,
    GRAPE_TELEMETRY_TIMER_LIST(GRAPE_TELEMETRY_NAME)
#undef GRAPE_TELEMETRY_NAME
};

static const char *const s_timer_csv_names[GRAPE_TELEMETRY_TIMER_COUNT] = {
#define GRAPE_TELEMETRY_CSV_NAME(name, level, label, csv) \
    [GRAPE_TELEMETRY_TIMER_##name] = csv,
    GRAPE_TELEMETRY_TIMER_LIST(GRAPE_TELEMETRY_CSV_NAME)
#undef GRAPE_TELEMETRY_CSV_NAME
};

static const uint8_t s_timer_levels[GRAPE_TELEMETRY_TIMER_COUNT] = {
#define GRAPE_TELEMETRY_LEVEL_VALUE(name, level, label, csv) \
    [GRAPE_TELEMETRY_TIMER_##name] = level,
    GRAPE_TELEMETRY_TIMER_LIST(GRAPE_TELEMETRY_LEVEL_VALUE)
#undef GRAPE_TELEMETRY_LEVEL_VALUE
};

static grape_telemetry_stat_t s_stats[GRAPE_TELEMETRY_TIMER_COUNT];
#if GRAPE_TELEMETRY_LEVEL > 0
static uint64_t s_cumulative_total_us[GRAPE_TELEMETRY_TIMER_COUNT];
#endif
static bool s_auto_report_enabled = true;

#if GRAPE_TELEMETRY_LEVEL > 0
#include "esp_timer.h"

static int64_t s_window_start_us;
#endif

#if GRAPE_TELEMETRY_AUTO_REPORT_ENABLE
#include <inttypes.h>

#include "esp_log.h"

static const char *TAG = "grape_telemetry";
#endif

const char *grape_telemetry_timer_name(grape_telemetry_timer_t timer)
{
    if ((unsigned)timer >= GRAPE_TELEMETRY_TIMER_COUNT || !s_timer_names[timer]) {
        return "unknown";
    }

    return s_timer_names[timer];
}

const char *grape_telemetry_timer_csv_name(grape_telemetry_timer_t timer)
{
    if ((unsigned)timer >= GRAPE_TELEMETRY_TIMER_COUNT || !s_timer_csv_names[timer]) {
        return "unknown";
    }

    return s_timer_csv_names[timer];
}

uint8_t grape_telemetry_timer_level(grape_telemetry_timer_t timer)
{
    if ((unsigned)timer >= GRAPE_TELEMETRY_TIMER_COUNT) {
        return UINT8_MAX;
    }

    return s_timer_levels[timer];
}

void grape_telemetry_record(grape_telemetry_timer_t timer, int64_t elapsed_us)
{
#if GRAPE_TELEMETRY_LEVEL > 0
    if ((unsigned)timer >= GRAPE_TELEMETRY_TIMER_COUNT || elapsed_us < 0 ||
        grape_telemetry_timer_level(timer) > GRAPE_TELEMETRY_LEVEL) {
        return;
    }

    grape_telemetry_stat_t *stat = &s_stats[timer];
    uint64_t value = (uint64_t)elapsed_us;

    portENTER_CRITICAL(&s_stats_lock);
    stat->total_us += value;
    s_cumulative_total_us[timer] += value;
    stat->calls++;
    if (value > stat->max_us) {
        stat->max_us = value;
    }
    portEXIT_CRITICAL(&s_stats_lock);
#else
    (void)timer;
    (void)elapsed_us;
#endif
}

#if GRAPE_TELEMETRY_AUTO_REPORT_ENABLE
void grape_telemetry_report_if_due(void)
{
    if (!s_auto_report_enabled) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if (s_window_start_us == 0) {
        s_window_start_us = now;
        return;
    }

    int64_t window_us = now - s_window_start_us;
    if (window_us < (int64_t)GRAPE_TELEMETRY_REPORT_INTERVAL_MS * 1000LL) {
        return;
    }

    ESP_LOGI(TAG,
             "---- %.2f ms telemetry window (level=%d, nested timings are not additive) ----",
             (double)window_us / 1000.0,
             GRAPE_TELEMETRY_LEVEL);

    grape_telemetry_snapshot_t snapshot;
    portENTER_CRITICAL(&s_stats_lock);
    memcpy(snapshot.timers, s_stats, sizeof(s_stats));
    memset(s_stats, 0, sizeof(s_stats));
    portEXIT_CRITICAL(&s_stats_lock);

    for (int i = 0; i < GRAPE_TELEMETRY_TIMER_COUNT; ++i) {
        if (grape_telemetry_timer_level((grape_telemetry_timer_t)i) >
            GRAPE_TELEMETRY_LEVEL) {
            continue;
        }

        const grape_telemetry_stat_t *stat = &snapshot.timers[i];
        if (stat->calls == 0) {
            continue;
        }

        double total_ms = (double)stat->total_us / 1000.0;
        double avg_ms = total_ms / (double)stat->calls;
        double max_ms = (double)stat->max_us / 1000.0;
        double window_percent = ((double)stat->total_us * 100.0) /
                                (double)window_us;

        ESP_LOGI(TAG,
                 "%-24s total=%8.3f ms avg=%7.3f ms max=%7.3f ms calls=%" PRIu32 " window=%6.1f%%",
                 grape_telemetry_timer_name((grape_telemetry_timer_t)i),
                 total_ms,
                 avg_ms,
                 max_ms,
                 stat->calls,
                 window_percent);
    }

    grape_telemetry_reset();
    s_window_start_us = now;
}
#endif

void grape_telemetry_reset(void)
{
    portENTER_CRITICAL(&s_stats_lock);
    memset(s_stats, 0, sizeof(s_stats));
    portEXIT_CRITICAL(&s_stats_lock);
}

void grape_telemetry_snapshot(grape_telemetry_snapshot_t *out_snapshot)
{
    if (!out_snapshot) {
        return;
    }

    portENTER_CRITICAL(&s_stats_lock);
    memcpy(out_snapshot->timers, s_stats, sizeof(s_stats));
    portEXIT_CRITICAL(&s_stats_lock);
}

void grape_telemetry_set_auto_report(bool enabled)
{
    s_auto_report_enabled = enabled;
#if GRAPE_TELEMETRY_LEVEL > 0
    if (enabled) {
        s_window_start_us = esp_timer_get_time();
    }
#endif
}

bool grape_telemetry_auto_report_enabled(void)
{
    return s_auto_report_enabled;
}

#if GRAPE_TELEMETRY_LEVEL > 0
uint64_t grape_telemetry_timer_cumulative_us(grape_telemetry_timer_t timer)
{
    if ((unsigned)timer >= GRAPE_TELEMETRY_TIMER_COUNT) {
        return 0;
    }

    portENTER_CRITICAL(&s_stats_lock);
    const uint64_t total = s_cumulative_total_us[timer];
    portEXIT_CRITICAL(&s_stats_lock);
    return total;
}
#endif
