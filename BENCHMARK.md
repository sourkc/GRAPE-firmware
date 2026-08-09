# GRAPE benchmark

GRAPE's benchmark is a deterministic performance suite, not an animation used as
an informal FPS test. It is implemented as the `grape_benchmark` component and
keeps benchmark-only access to renderer internals behind
`grape_benchmark_hooks.h`.

The frozen performance coverage map is documented in `BENCHMARK_COVERAGE.md`.

## Running it

Set the boot mode in `main/app_config.h`:

```c
#define GRAPE_APP_RUN_BENCHMARK 1
```

The benchmark does not require an SD card while it is running. Summary, sample,
and metadata output is accumulated in PSRAM. After the selected suites finish,
the runner attempts to mount the onboard SD card and writes the buffered reports.
If no card is present, the completed result stays in PSRAM and the runner waits,
retrying every `GRAPE_BENCHMARK_SD_RETRY_MS`, until a card is inserted and all
reports are saved successfully.

Defaults are in:

```text
components/grape_benchmark/include/grape/grape_benchmark_config.h
```

A caller can override any default through `grape_benchmark_config_t`, including
`suite_mask`. For example:

```c
grape_benchmark_config_t config = GRAPE_BENCHMARK_CONFIG_DEFAULT();
config.suite_mask = GRAPE_BENCHMARK_SUITE_DAMAGE_PLAN |
                    GRAPE_BENCHMARK_SUITE_PIXEL_BACKENDS;
ESP_ERROR_CHECK(grape_benchmark_run(grape, &config));
```

Available suite-mask bits are:

- `GRAPE_BENCHMARK_SUITE_DAMAGE_MARK`
- `GRAPE_BENCHMARK_SUITE_DAMAGE_PLAN`
- `GRAPE_BENCHMARK_SUITE_COMPOSITOR`
- `GRAPE_BENCHMARK_SUITE_PIXEL_BACKENDS`
- `GRAPE_BENCHMARK_SUITE_THREE_SHEAR`
- `GRAPE_BENCHMARK_SUITE_FRAGMENTATION`
- `GRAPE_BENCHMARK_SUITE_PRESENTATION`
- `GRAPE_BENCHMARK_SUITE_LIFECYCLE`
- `GRAPE_BENCHMARK_SUITE_SCENES`
- `GRAPE_BENCHMARK_SUITE_ALL`

## Measurement model

Each case uses the same lifecycle:

```text
setup
  -> deterministic warmup iterations
  -> reset telemetry
  -> deterministic measured iterations
       -> timed workload
       -> optional normal grape_present()
       -> untimed per-iteration reset
  -> telemetry snapshot / case metrics
  -> teardown
```

The runner owns timing and reporting. Cases do not invent their own stopwatch
scheme. Telemetry automatic reporting is disabled for the duration of the suite
so a periodic report cannot reset a measurement window mid-case.

The benchmark also temporarily extends the ESP-IDF Task Watchdog timeout to
`GRAPE_BENCHMARK_WDT_TIMEOUT_MS` (60 seconds by default) so sustained
CPU-saturation microbenchmarks are not mistaken for a stuck renderer. The normal
`CONFIG_ESP_TASK_WDT_TIMEOUT_S` timeout, idle-core mask, and panic policy are
restored when the suite exits. This changes watchdog tolerance only; it does not
insert scheduler delays into measured workloads.

Animation/state generation uses a fixed seed, a fixed timestep, and iteration
number. Faster renderer versions therefore execute the same sequence of scene
states as slower versions.

Cases are tagged as:

- `MICRO`: isolates one subsystem and intentionally bypasses unrelated pipeline work.
- `PIPELINE`: exercises a controlled combination of subsystems.
- `SCENE`: runs normal update -> damage -> plan -> composite -> present behavior.
- `LIFECYCLE`: measures allocation/resource/list operations rather than FPS work.

## Benchmark families

### Damage marking

Sweeps A8 texture size, surface count, and occupancy pattern. It includes empty,
fully opaque, sparse-cell, half-cell, and dense-minus-one-cell occupancy, plus
shared-texture invalidation fan-out. The benchmark clears accumulated logical
damage outside the timed region after every iteration.

### Damage planner

Feeds predetermined tile bitmaps directly to the planner. Cases include solid
regions, horizontal/vertical stripes, checkerboard, deterministic noise at
multiple densities, and distributed compact islands. Reports include dirty tile
count, split count, split candidates, final rectangle count, final pixels,
overdraw ratio, and fullscreen fallback.

### Compositor traversal

Sweeps surface count x render-rectangle count x intersection ratio using a tiny
source surface so traversal/rejection/dispatch scaling is visible without a
large raster workload dominating it.

### Pixel backends

Sweeps operation area for PPA fill, CPU fill, PPA A8 blend, CPU A8 affine,
CPU RGB565 affine, and CPU RGB888 affine. Surface cases include opaque and
partial-alpha paths. A8 includes paired internal-RAM/PSRAM source cases.

Telemetry provides the backend-specific hardware/CPU timer totals while the
runner also records complete direct-compositor invocation latency.

### Three-shear rotation

Sweeps texture sizes 32/64/128/256/512 and angles 0, 5, 15, 30, 45, 60, 75, 85,
89, and 90 degrees. The normal cases use the PPA final A8 composite when
available. Selected 128px angles also force the CPU final composite.

The telemetry snapshot exposes preparation, X1, Y, X2, clear, quarter-turn,
and composite timings. Scratch-buffer capacity is reported as a case metric.

### Fragmentation

Keeps rendered pixel area approximately constant while changing the number of
render rectangles through 1, 2, 4, 8, and 16. The same static surface scene is
underneath every case. This is intended to calibrate fixed per-rectangle cost and
therefore the damage planner's rectangle-overhead heuristic.

### Presentation

Measures full-width dirty bands with 16/64/256/640/full-height Y spans and a
same-rendered-area pair of tests whose two small rectangles are either close
together or at opposite ends of the display. This directly exposes the current
driver behavior of submitting the full display width from minimum dirty Y to
maximum dirty Y.

`display.submit` and `display.refresh_wait` remain separate telemetry timers.
The runner stores per-iteration samples so refresh/VSync distributions can be
analyzed instead of relying only on averages.

### Lifecycle

Measures surface create/destroy batches, texture create/destroy batches, A8
occupancy rebuilds, and Z-order reordering at increasing object counts.

### Canonical scenes

Normal end-to-end scenes cover:

- motion-heavy transforming opaque surfaces
- deep semi-transparent overdraw
- many small spatially fragmented moving surfaces
- three-shear rotation-heavy scenes
- a ten-square legacy/reference scene

These are intentionally separate from the microbenchmarks. They answer whether
GRAPE as a whole became faster after an optimization; they do not identify the
cause by themselves.

## Reports

After measurement completes and the output directory becomes available, the suite writes:

```text
grape_benchmark_summary.csv
grape_benchmark_samples.csv
grape_benchmark_metadata.txt
```

The summary contains one row per case with:

- case kind/group/name and parameters
- iteration count and iterations/second
- mean, standard deviation, min, p50, p95, p99, max for workload, present, and total time
- every registered GRAPE telemetry timer total/average/max/call count
- benchmark-specific named metrics

The samples CSV contains per-iteration workload/present/total timing. All report
text is buffered in PSRAM for the full run and written only after benchmarking is
complete, so SD-card availability and file I/O cannot affect measured iterations.
The `iteration` field is the zero-based measured-sample index within each case.

Metadata records the build label, IDF target/version, chip revision, CPU
frequency, PSRAM, display information, fixed timestep, deterministic seed, suite
mask, and iteration defaults.

## Benchmark rules

1. Vary one primary stress axis in microbenchmarks.
2. Use fixed seeds and fixed timesteps; never wall-clock animation state.
3. Keep warmup and file I/O outside measured regions.
4. Skip unsupported hardware features explicitly; never silently benchmark a fallback under an accelerator label.
5. Do not optimize renderer code in benchmark-framework commits.
6. Preserve raw samples for quantile/outlier analysis.
7. Use canonical scenes only after microbenchmarks establish where time is spent.
