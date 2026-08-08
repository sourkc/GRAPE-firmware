# GRAPE compile-time profiler

Profiler switches live in:

`components/grape_common/include/grape/grape_profile_config.h`

Set `GRAPE_PROFILE_ENABLE` to `0` to compile out the profiler globally, or leave it at `1` and toggle each timing independently.

The report interval is controlled by `GRAPE_PROFILE_REPORT_INTERVAL_MS`.

Current timings:

- `GRAPE_PROFILE_PRESENT` - entire `grape_present()` call.
- `GRAPE_PROFILE_DAMAGE_ADD` - marking logical dirty tiles.
- `GRAPE_PROFILE_DAMAGE_PLAN` - extracting tile runs and building the adaptive render-rectangle plan.
- `GRAPE_PROFILE_SURFACE_TRANSFORM` - complete surface transform update, including recache and damage tracking.
- `GRAPE_PROFILE_SURFACE_RECACHE` - sin/cos cache and transformed-bounds calculation.
- `GRAPE_PROFILE_COMPOSITOR` - one dirty rectangle from clear through display blit.
- `GRAPE_PROFILE_PPA_FILL` - PPA background-fill dispatch including the blocking PPA operation.
- `GRAPE_PROFILE_CPU_FILL` - software background-fill fallback.
- `GRAPE_PROFILE_PPA_BLEND_DISPATCH` - the full PPA blend fast-path check/configuration/dispatch attempt for one surface.
- `GRAPE_PROFILE_PPA_BLEND_HW` - time spent specifically in `ppa_do_blend()` for surfaces that actually reach hardware.
- `GRAPE_PROFILE_CPU_SURFACE_RASTER` - software rasterization of one fallback surface over its clipped bounds.
- `GRAPE_PROFILE_SHEAR_PREP` - three-shear bounds, buffer-size calculations, and reusable-buffer checks before the passes.
- `GRAPE_PROFILE_SHEAR_X1` - complete first horizontal shear pass, including its output-buffer clear.
- `GRAPE_PROFILE_SHEAR_Y` - complete vertical shear pass, including its output-buffer clear.
- `GRAPE_PROFILE_SHEAR_X2` - complete second horizontal shear pass, including its output-buffer clear.
- `GRAPE_PROFILE_SHEAR_CLEAR` - time spent specifically clearing shear output buffers. This is nested inside X1/Y/X2.
- `GRAPE_PROFILE_SHEAR_QUARTER_TURN` - the special exact ±90° path.
- `GRAPE_PROFILE_SHEAR_COMPOSITE` - compositing the completed A8 shear image into GRAPE's display scratch buffer.
- `GRAPE_PROFILE_DISPLAY_BLIT` - full display blit, including waiting until the source buffer is safe to reuse.
- `GRAPE_PROFILE_LCD_DRAW_SUBMIT` - `esp_lcd_panel_draw_bitmap()` submission only.
- `GRAPE_PROFILE_LCD_DRAW_WAIT` - wait for `on_color_trans_done` after submission.

Reports contain total, average, maximum, call count, and percentage of the report window. Timings are nested, so percentages and totals are not expected to add to 100%.

Timings add a small amount of measurement overhead because each enabled region calls `esp_timer_get_time()`.

## Benchmark integration

The benchmark component temporarily disables the profiler's automatic interval
reporting, resets the counters after its warm-up frames, and captures a
`grape_profile_snapshot_t` after each measured case.

The snapshot API is intentionally public to GRAPE components:

```c
grape_profile_snapshot_t snapshot;
grape_profile_snapshot(&snapshot);
```

This lets benchmarks/reporters consume the same counters without parsing serial
log text. Automatic reporting is restored when the benchmark exits.

