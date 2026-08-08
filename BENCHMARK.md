# GRAPE benchmark

The benchmark is a separate `grape_benchmark` component so renderer code does not
need benchmark-specific logic.

## Running it

`main/app_config.h` selects the boot mode:

```c
#define GRAPE_APP_RUN_BENCHMARK 1
```

Set it to `0` to run the existing demo in `main.c`.

The benchmark defaults live in:

```text
components/grape_benchmark/include/grape/grape_benchmark_config.h
```

The important switches are:

- `GRAPE_BENCHMARK_WARMUP_FRAMES`
- `GRAPE_BENCHMARK_MEASURED_FRAMES`
- `GRAPE_BENCHMARK_CASE_COOLDOWN_MS`
- `GRAPE_BENCHMARK_WRITE_SUMMARY_CSV`
- `GRAPE_BENCHMARK_WRITE_SAMPLES_CSV`
- the individual `GRAPE_BENCHMARK_SUITE_*` switches

The benchmark disables telemetry's automatic periodic report while it is
running, resets the telemetry window immediately before each measured section,
and takes a telemetry snapshot after the measured frames.

## Current cases

The initial shape suite covers:

- empty `grape_present()` overhead
- full-screen background redraw
- isolated circle and square redraw
- circle movement
- square movement
- both moving
- two objects crossing/intersecting
- stationary and moving overlap
- opacity values
- fixed scale values
- affine rotation from 0 to 90 degrees in 5 degree increments
- three-shear rotation from 0 to 90 degrees in 5 degree increments
- 45-degree rotation combined with several scales

Movement is deterministic and based on frame number rather than wall-clock time,
so different renderer versions perform the same sequence of transforms.

The rotation suite deliberately forces the two CPU rotation backends instead of
using the normal automatic/PPA path. The CSV groups are `rotation_affine` and
`rotation_shear`, with the angle stored in the `angle_deg` parameter. This makes
frame time and CPU raster time directly plottable against rotation angle for both
methods. The `rotation_shear_ppa_y` group explicitly enables
`GRAPE_FEATURE_PPA_A8_ROTATE`; unsupported hardware reports that setup as not
supported rather than silently benchmarking the direct Y-shear fallback.

The three-shear backend is currently experimental. It handles A8 textures at
1:1 scale for normalized rotations from -90 to +90 degrees. Unsupported surfaces
fall back to the affine rasterizer. `GRAPE_ROTATION_BACKEND_AUTO` preserves the
normal renderer behavior and does not select the experimental shear path yet.

The first shear implementation uses point sampling in the three shear passes.
Because the image is resampled three times, edge pixels can differ slightly from
the single-pass affine reference. Exact +/-90 degree rotations use a dedicated
quarter-turn copy inside the shear backend to avoid the edge loss caused by three
successive nearest-neighbor resamples at that boundary. The affine backend
remains available for visual and performance comparison while the shear path is
evaluated.

## Reports

The benchmark always logs one short result per case.

If `GRAPE_BENCHMARK_OUTPUT_DIRECTORY` points at a mounted VFS filesystem, it
also writes:

```text
grape_benchmark_summary.csv
grape_benchmark_samples.csv
grape_benchmark_metadata.txt
```

The default output directory is `/sdcard`.

The benchmark intentionally does **not** mount an SD card itself. Storage
mounting is board/application policy. This means the same benchmark can later
write to a P4-local SD card, flash filesystem, USB storage, or another mounted
filesystem without changing the benchmark component.

The current benchmark application mounts the onboard microSD slot on the
Waveshare ESP32-P4-WIFI6-DEV-KIT through `grape_storage` before running the
benchmark and unmounts it afterward. The board configuration uses 4-bit SDMMC
on GPIO43/44/39/40/41/42 and SDMMC IO power from on-chip LDO channel 4. Card
formatting on mount failure is disabled.

`grape_benchmark_summary.csv` contains one row per case, including FPS,
frame/update/present min/average/max values, parameters, and all GRAPE telemetry
totals/averages/max/call counts. The direct wall-clock columns use the
`*_wall_*` names so they do not collide with telemetry timer column names.

`grape_benchmark_samples.csv` contains per-frame update, present, and whole-frame
timings. Samples are buffered in RAM during the measured section and only
written afterward so SD writes do not contaminate the timing.

`grape_benchmark_metadata.txt` records the IDF version, target, chip revision,
core count, CPU frequency, PSRAM size, display information, and benchmark
settings.

## Adding future suites

Benchmark cases use setup/step/teardown callbacks. The runner owns warm-up,
timing, telemetry capture, report writing, and cleanup sequencing.

The current cases are in:

```text
components/grape_benchmark/src/cases/grape_benchmark_shapes.c
```

Future features such as fonts, TTF rendering, JPEG decoding, texture streaming,
surface-count sweeps, or GFXLINK throughput should be added as new case/suite
files rather than special-cased in the runner.

A case should:

1. Allocate/configure its test resources in `setup`.
2. Perform only the per-frame scene update in `step`.
3. Release resources in `teardown`.

The benchmark runner calls `grape_present()` itself so all cases get the same
frame timing methodology.
