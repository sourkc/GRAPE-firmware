# GRAPE benchmark coverage map v1

This inventory is frozen as the coverage checklist for the first deterministic
benchmark suite. Items are grouped into benchmark families rather than treated
as separate test programs.

## Frame/pipeline and surface state

Covered systems: whole-frame update/present, no/partial/full redraw behavior,
surface transform setters and recache, position/rotation/scale/origin, opacity,
tint, visibility, texture replacement effects, Z ordering, surface list
insertion/removal, creation/destruction, and old/new coverage marking.

Primary suites: `damage_mark`, `compositor`, `lifecycle`, `scenes`.

## Textures and A8 occupancy

Covered systems: A8/RGB565/RGB888 textures, internal/PSRAM source memory,
allocation/destruction, sharing/fan-out, invalidation, occupancy allocation and
rebuild, empty/all-cell-occupied/partial occupancy, texture size/stride, and
occupancy-cell scaling behavior.

Primary suites: `damage_mark`, `pixel_backend`, `lifecycle`.

## Damage marking/history/planning

Covered systems: explicit/tile damage, transformed whole-quad marking,
quad-vs-tile tests, occupancy-cell projection, old/new surface coverage,
logical/current/previous/render visible damage roles, planner root scan,
horizontal/vertical split search, child shrinking, candidate evaluation,
profitable split selection, max-rectangle limit, rectangle-overhead cost,
fullscreen comparison, dirty-shape dependence, and planner quality/overdraw.

Primary suites: `damage_mark`, `damage_plan`, `fragmentation`, `scenes`.

## Compositor and pixel work

Covered systems: per-rectangle compositor invocation, Z-list traversal,
bounds rejection, backend dispatch, PPA/CPU background fill, PPA A8 blend,
CPU affine A8/RGB565/RGB888, alpha/tint arithmetic, framebuffer read-modify-write,
destination pixel format, direct full-stride framebuffer access, operation size,
fixed accelerator-call overhead, source memory domain, and feature fallback.

Primary suites: `compositor`, `pixel_backend`, `fragmentation`, `scenes`.

## Three-shear rotation

Covered systems: eligibility/prep, normalized angle, coefficients, transformed
intermediate bounds, scratch allocation/reuse, X1/Y/X2 passes, clears, exact
quarter-turn path, final PPA/CPU A8 composite, intermediate memory traffic, and
angle/texture-size scaling. Experimental PPA A8 rotation remains represented by
the Feature API and is skipped on unsupported pre-v3 hardware.

Primary suite: `three_shear`; integrated coverage: `scenes/rotation`.

## PPA and feature/capability dispatch

Covered systems: fill/blend accelerator availability, runtime enable/disable,
accelerated vs fallback behavior, PPA-compatible framebuffer target, operation
setup overhead, many-small vs larger block behavior, and PPA source/destination
memory interaction. The current codebase has no PPA framebuffer-copy path, so no
copy benchmark is included.

Primary suite: `pixel_backend`; secondary: `three_shear`, `fragmentation`.

## Direct backbuffer, display submission, and VSync

Covered systems: hidden framebuffer rendering, dirty rectangle fragmentation,
display begin-frame target acquisition, current full-width dirty-Y-span
submission behavior, `esp_lcd_panel_draw_bitmap` submission, double-buffer swap,
refresh callback/semaphore wait, 60 Hz phase quantization, and same rendered area
with different physical Y spans.

Primary suites: `fragmentation`, `presentation`, `scenes`.

## Debug/telemetry/measurement infrastructure

Debug overlays remain correctness tooling and are intentionally disabled for
performance cases. Telemetry instrumentation overhead can be compared by
building at levels 0/1/2. The benchmark runner itself owns deterministic state,
warmup, measurement windows, raw samples, quantiles, CSV output, metadata, and
feature skips; telemetry automatic reporting is disabled during benchmark runs.

Primary suite: benchmark framework itself; not treated as renderer workload.

## Global scaling dimensions

The suite intentionally exercises: surface count, texture count/size, shared
texture fan-out, dirty tile count and spatial distribution, rectangle count,
rendered pixels, overlap depth, alpha, rotation angle, scale/motion, number of
changed surfaces, PSRAM/internal memory, CPU/PPA path selection, framebuffer
resolution/pixel format effects, and lifecycle object counts.
