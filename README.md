# GRAPE

...Wow such empty...

## TODO:
- [x] USE PPA for rotation and scaling!
- [ ] grape_texture_invalidate_rect(texture, x, y, w, h); (I forgot what this means)
- [ ] Re-test PPA-rotated Y shear on ESP32-P4 rev 3.x (GRAY8 SRM is unavailable on the current pre-v3 chip)
  - TEST THIS ONCE CHIP V3.X ARRIVES!
- [x] Tile-based adaptive damage grouping
- [x] Per-texture occupancy masks for transparent/irregular textures
- [ ] Merge the different debug options into one unified system
- [x] Build a deterministic benchmark suite
- [x] Centralize hardware features and their lookup
- [ ] Tackle watchdog and vtaskdelay bs so we never get this error ever again

## Chip revision overlays

Pre-v3 silicon:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;config/esp32p4_rev_pre_v3.defaults" build
```

Rev 3.x silicon:

```text
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;config/esp32p4_rev3.defaults" build
```

## Debug layers

Debug overlays are rendered after normal surfaces directly into the active render target. They do not create normal scene damage; their physical draw/cleanup coverage is tracked separately so debug rendering cannot feed back into logical scene damage.

The first built-in layer visualizes the final logical damage rectangles for the current frame with a translucent red fill and stronger border:

```c
ESP_ERROR_CHECK(
    grape_debug_set_layer_enabled(
        grape,
        GRAPE_DEBUG_LAYER_DAMAGE_RECTS,
        true
    )
);
```

Disable it with the same call and `false`. The previous overlay area is queued as render-only damage so it is repainted once and disappears cleanly. Debug layers are intended for correctness/debugging rather than performance measurements.

## Telemetry

GRAPE uses one compile-time telemetry system for coarse diagnostics and detailed
profiling. Set `GRAPE_TELEMETRY_LEVEL` in
`components/grape_common/include/grape/grape_telemetry_config.h` to `0`, `1`, or
`2`. See `TELEMETRY.md` for timer registration, scope/block instrumentation, and
benchmark integration.

## Features

Optional hardware/implementation capabilities use the unified Feature API. It
separates capability availability from runtime policy and exposes stable feature
IDs suitable for future GFXLINK discovery/control. See `FEATURES.md`.

## Benchmark

GRAPE includes deterministic micro, pipeline, lifecycle, and end-to-end scene
benchmarks. See `BENCHMARK.md` for running/reporting details and
`BENCHMARK_COVERAGE.md` for the frozen performance coverage map.

## Vector paths

GRAPE vector paths support filled contours built from `move_to`, `line_to`,
`quad_to`, `cubic_to`, and `close`. Relative-coordinate helpers are available
for each drawing command, along with horizontal/vertical line helpers and
SVG-style smooth quadratic/cubic helpers. These convenience calls normalize
immediately into the core line, quadratic, and cubic commands.

Quadratic and cubic Bezier curves are flattened adaptively at rasterization
time, then the existing non-zero winding A8 rasterizer handles fill and
antialiasing. The resulting mask is an ordinary A8 texture and uses the normal
GRAPE surface/compositor pipeline.

Elliptical arcs, strokes, SVG parsing, and font loading are not implemented yet.

## Planned stuff
- JPEG images (hardware)
- Vector graphics
  - Font rendering
  - Eventually maybe SVG rendering
- GFXLINK
  - USB
  - SPI
- Graphics driver port for PC
- OpenGL port for ESP32
- 