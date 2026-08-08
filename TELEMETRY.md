# GRAPE telemetry

Telemetry configuration lives in:

`components/grape_common/include/grape/grape_telemetry_config.h`

Set `GRAPE_TELEMETRY_LEVEL` to:

- `0` - disabled. Timing instrumentation compiles out globally.
- `1` - coarse frame, damage, and refresh-wait timing.
- `2` - detailed renderer, PPA, and three-shear timing in addition to level 1.

The automatic report interval is controlled by
`GRAPE_TELEMETRY_REPORT_INTERVAL_MS`. Automatic generic telemetry reports are enabled by default only at level 2 via `GRAPE_TELEMETRY_AUTO_REPORT_ENABLE`; level 1 feeds the lightweight damage report without adding a second periodic log.

## Instrumentation API

Time a whole function or lexical scope with one line:

```c
GRAPE_TIME_SCOPE(DAMAGE_MARK);
```

The scope is ended automatically, including when the function returns early.

Time a smaller region with:

```c
GRAPE_TIME_BLOCK(PPA_FILL) {
    ret = grape_ppa_fill(context, rect, context->background);
}
```

Timer identifiers, display names, CSV names, and required telemetry levels are
registered once in `grape_telemetry.h`. Adding a timer does not require adding
fields to the telemetry accumulator or reporter.

## Coarse timers

Level 1 enables:

- `PRESENT` - the complete `grape_present()` call.
- `DAMAGE_MARK` - logical damage marking.
- `DAMAGE_PLAN` - adaptive damage-rectangle planning.
- `DISPLAY_REFRESH_WAIT` - waiting for the refresh boundary after framebuffer submission.

The demo's periodic `DAMAGE:` report uses these same timers. It no longer owns
separate timestamping code.

## Detailed timers

Level 2 also enables:

- `SURFACE_TRANSFORM`
- `SURFACE_RECACHE`
- `COMPOSITOR`
- `PPA_FILL`
- `CPU_FILL`
- `PPA_BLEND_DISPATCH`
- `PPA_BLEND_HW`
- `CPU_SURFACE_RASTER`
- `SHEAR_PREP`
- `SHEAR_X1`
- `SHEAR_Y`
- `SHEAR_X2`
- `SHEAR_CLEAR`
- `SHEAR_QUARTER_TURN`
- `PPA_ROTATE`
- `SHEAR_COMPOSITE`
- `DISPLAY_SUBMIT`

Timings are nested, so totals and report-window percentages are not expected to
add to 100%.

## Snapshot API

Benchmarks consume telemetry directly instead of parsing log output:

```c
grape_telemetry_snapshot_t snapshot;
grape_telemetry_snapshot(&snapshot);
```

`grape_telemetry_reset()` resets the reporting/snapshot window. Internal
monotonic timer totals used for per-frame deltas are kept separately, so an
automatic report or benchmark reset cannot break frame-local timing.

The benchmark component disables automatic reporting while a benchmark suite is
running, resets the snapshot counters after warm-up, captures a telemetry
snapshot for each measured case, and restores the previous automatic-report
setting afterward.
