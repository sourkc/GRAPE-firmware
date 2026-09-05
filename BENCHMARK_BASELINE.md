# GRAPE baseline benchmark package

This package prepares the **unchanged renderer** for the sequence:

1. Measure the current `-Og` renderer with this benchmark harness.
2. Change only compiler optimization to `-O2` and measure again.
3. Implement renderer optimizations incrementally and repeat the same tests.

`sdkconfig`, `sdkconfig.defaults`, dependency versions, clocks, shader sources,
renderer algorithms, PPA dispatch and GPU worker behavior are preserved. The only
GRAPE-core changes are read-only benchmark capture hooks. No renderer correctness
fixes or performance optimizations have been included in this package.

## First board run

1. Extract the source ZIP into a fresh directory. Use your usual ESP-IDF 5.5.4
   environment and the same Waveshare board, panel and power configuration.
2. Build, flash and monitor using your usual procedure (`idf.py build`, then
   `idf.py flash monitor`, adding your port if needed). The package already selects
   benchmark mode in `main/app_config.h`; no demo-selection edit is necessary.
3. Keep an SD card inserted with **at least 250 MB free per run**. Reports are
   buffered during timing. Raw reference files are written only afterward.
4. Wait for **“Benchmark results saved; SD card is safe to remove”**. The earlier
   “Timing results saved” message is followed by reference replay; it is not the
   completion message.
5. Copy the entire new `/sdcard/grape_run_0001` folder to your PC. Subsequent runs
   automatically select the next unused numbered folder; they do not overwrite
   previous results. Keep raw files together with their CSV manifests.
6. Repeat the unchanged build a few times to establish normal run-to-run variation.
   Reboot between captures and keep background traffic and environmental conditions
   consistent. No GFXLINK server is running in this benchmark application mode.

The default is the **baseline profile: 16 warmup + 128 measured iterations** for
each of the 26 selected cases. Cases are deterministic; rendering does not follow
wall-clock animation time. Reference replay always starts from fresh state and
runs frames 0–3, independently of the timing profile.

## What the focused run covers

Sixteen 2D cases exercise RGB565 copies, RGB565/RGBA 2× enlargement, A8/RGBA blends,
four overlapping opaque surfaces, moving 8×8 alpha edits in 512×512 textures,
A8 rotation, bilinear RGBA rotation, a two-stage brightness chain, the existing
simple procedural gradient at 135 rows, and paired CPU/PPA fills of 16×16 and
16×256 pixels. Fill cases isolate work without physical presentation in the timed
loop. Reference replay presents them afterward; setup seeds a visible pattern so
a no-op fill cannot silently pass the comparison.

Ten 3D cases cover the textured cube at 1×/2×/4×, twelve cubes, thin geometry,
depth overdraw, 48 small cubes, near-plane clipping, alternating CLEAR/LOAD passes,
and a 4× cube presented at 2× scale. The original two GPU case names are retained.

All fifteen original suites remain registered. To run their broader vector,
font, text, SVG, rotation, lifecycle and damage coverage as well, set
`GRAPE_APP_BENCHMARK_SUITE_MASK` in `main/app_config.h` to
`GRAPE_BENCHMARK_SUITE_ALL`. The new capture callbacks cover the focused 2D/GPU
cases; older cases still provide timing/metrics but do not gain golden images.
Keep the same suite selection across a comparison.

## Profiles and instrumentation

Edit `components/grape_benchmark/include/grape/grape_benchmark_config.h`:

| Setting | Purpose |
|---|---|
| `GRAPE_BENCHMARK_PROFILE_SMOKE` | 6 warmup / 32 samples; quick bring-up |
| `GRAPE_BENCHMARK_PROFILE_BASELINE` | 16 / 128; default focused comparison |
| `GRAPE_BENCHMARK_PROFILE_LONG` | 32 / 1024; longer variability/tail investigation |
| `GRAPE_BENCHMARK_RUN_LABEL` | Optional descriptive label such as `baseline`, `O2`, `copy-fastpath` |
| `GRAPE_BENCHMARK_GPU_STATS` | Default **0** for performance runs; set **1** only for separate diagnostic runs |
| `GRAPE_BENCHMARK_CAPTURE_REFERENCES` | Default **1**; exact comparison requires completed captures |

Use the same profile for baseline and candidate. Changing duration changes which
animation states are measured. Original suites may have their own iteration
overrides; actual counts remain in the summary. Long mode is intended primarily
for the focused selection: a full long run can produce substantial buffered CSV
data. The 128-sample p99 is near the maximum and should not be treated as a stable
tail-latency estimate.

The function profiler stays disabled. It counts calls and disables the second GPU
worker when enabled. Normal measurements must not be compared against that mode.
GPU stage statistics are now explicitly opt-in; the old GPU benchmarks always
enabled them. Therefore new results should be compared against this harness's
baseline, not assumed equivalent to earlier v4 benchmark captures.

No per-pixel instrumentation was added to renderer loops. Existing GPU diagnostic
counters include triangle counts, active tiles and tile references. The alpha-edit
cases additionally record final occupancy and the known steady-state edited pixel
count; these are **not** claimed to be measured occupancy-scan counts. Detailed
shaded-pixel, cache-transaction and scan counters remain future diagnostic work.

The harness lets idle tasks run outside timed samples after approximately 100 ms
of elapsed work. This avoids long runs starving the watchdog. The interval is
recorded in metadata. It does not change the renderer's internal shader delays.

## Files and timing interpretation

- `grape_benchmark_metadata.txt`: harness/profile identity, run label, ELF SHA-256,
  compiler and configured optimization, IDF version, chip revision, clocks, cache,
  display, instrumentation, PPA availability and GPU multicore build eligibility.
- `grape_benchmark_summary.csv`: work/present/total distributions and available
  metrics for every case, including explicit unsupported-case rows.
- `grape_benchmark_samples.csv`: individual timing samples for investigating noise.
- `grape_benchmark_references.csv`: each reference's dimensions, format, frame,
  sample count, byte count, CRC32 and raw filename.
- `*.raw`: dense reference pixels, without allocation/stride padding. Formats are
  `rgb565le`, `rgb888`, `rgba8888`, and `d16le`; depth samples are interleaved per
  pixel, then pixels per row. Lazy-cleared depth is read as its logical clear value
  without modifying the renderer's depth state.

`work_us` measures the case iteration. `present_us` measures `grape_present`, which
includes composition, submission and display wait; it is not exclusively scanout
wait. `total_us` includes both calls but excludes setup, harness service gaps,
reference capture, CSV formatting and SD writes. It is not sustained physical FPS.
The telemetry-derived refresh-wait column is unavailable at the default telemetry
level; metadata explicitly marks it unavailable. Zero must not be read as no wait.
GPU worker raster/resolve durations are sums of worker elapsed time, not parallel
batch wall time. Use complete pass/frame timing for performance conclusions.

Timing reports are saved before reference replay starts. If capture fails or is
interrupted, `references_complete=0` remains in the saved metadata. The completion
marker is appended only after all selected supported capture cases finish. The
comparison tool checks the final marker and expected frames/planes.

References preserve existing renderer behavior, including any existing bugs.
Matching them is a regression check, not proof of independent rendering correctness.
Coverage includes representative cases, not every API state. Keep additional
targeted tests for each later optimization or correctness fix.

## Compare baseline and O2

After recording the baseline, use `idf.py menuconfig` → Compiler options →
Optimization Level → Optimize for performance (`-O2`). Confirm the resulting
`CONFIG_COMPILER_OPTIMIZATION_PERF=y`, rebuild and record another run. Do not also
change clocks, MSAA, filtering, telemetry, worker settings or the benchmark profile.
The emitted metadata records the configured optimization; preserve
`build/compile_commands.json` and the ELF/map with each build when investigating
effective per-file overrides.

Run on the PC, from the extracted project's root (Python 3.9+; no extra packages):

```text
python tools/benchmark_compare.py "C:/captures/baseline" "C:/captures/O2" --output comparison.json
```

The tool reports per-case work/present/total mean ratios, flags work or total
regressions greater than 5%, verifies raw-file sizes and CRCs, compares exact bytes,
and rejects missing cases, incomplete captures or mismatched measurement settings.
An intentional optimization-level/ELF/run-label change is allowed. A difference in
image bytes is always reported; do not silently increase tolerances to accept it.
Later quality-changing work needs a separately reviewed tolerance policy.

Exit codes: **0** comparable with no flagged regressions or output differences;
**1** output differences or timing regression flags to investigate; **2** invalid,
incomplete or incompatible inputs. `--allow-config-change` permits an intentional
settings mismatch while still printing it. A 5% flag is a triage threshold, not a
statistical significance claim. Repeat both builds before accepting small gains.

Host comparison-tool checks:

```text
python -m unittest discover -s tools/tests -p test_benchmark_compare.py -v
```

The package was compiled and linked against the installed ESP-IDF 5.5.4 ESP32-P4
toolchain with `-Og`; the firmware passed its application-partition size check.
No board was flashed or timed during implementation. Hardware execution, SD capture
and actual baseline performance remain the next step on your board.
