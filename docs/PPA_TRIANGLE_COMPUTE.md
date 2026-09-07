# Experimental DMA2D/PPA triangle coverage

Feature: `GRAPE_FEATURE_PPA_TRIANGLE_EDGES`, name `ppa.triangle_edges`, ID `0x00010004`.
It is experimental, remotely configurable through the existing feature API, and
**disabled by default**. Resetting the feature disables it again.

Three companion controls use the same feature API:

| Feature | Registry name / ID | Default |
| --- | --- | --- |
| `GRAPE_FEATURE_PPA_TRIANGLE_VALIDATE` | `ppa.triangle_validate` / `0x00010005` | AUTO (active when compiled) |
| `GRAPE_FEATURE_PPA_TRIANGLE_BATCH` | `ppa.triangle_batch` / `0x00010006` | Disabled |
| `GRAPE_FEATURE_PPA_TRIANGLE_OVERLAP` | `ppa.triangle_overlap` / `0x00010007` | Disabled |

No companion enables the triangle backend by itself. Validation remains
compiled by default, so a single firmware can measure it on and off. Batching
tries 64x64, then 32x32, then the original 16x16 blocks, using the largest size
whose coefficients and padded output ranges fit exactly across the triangle.
Small bounding boxes select smaller blocks to avoid unnecessary padding.

The original rasterizer functions remain unchanged. The new dispatch tries a
separate backend before entering those functions. Initial eligibility is:

- Single-sample rendering (`GRAPE_GPU_SAMPLE_COUNT_1`).
- Solid-color or push-color fragments, with alpha 255.
- Depth testing and depth writing both disabled.
- All tiles have exactly representable edge coefficients.

MSAA, textures, nonopaque colors, depth operations, and coefficients outside the
supported domain fall back to the original renderer. MVP, clipping and triangle
setup continue on the CPU. This first implementation is a correctness experiment;
it makes no performance claim.

## Enable through the feature API

```c
ESP_ERROR_CHECK(grape_feature_enable(grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES));

/* Optional, outside a render pass. Otherwise the first eligible triangle
 * initializes the backend and runs these checks automatically. */
ESP_ERROR_CHECK(grape_gpu_ppa_triangle_self_test(gpu));

/* Optional performance experiment. Keep an independent final image comparison. */
ESP_ERROR_CHECK(grape_feature_disable(grape, GRAPE_FEATURE_PPA_TRIANGLE_VALIDATE));
ESP_ERROR_CHECK(grape_feature_enable(grape, GRAPE_FEATURE_PPA_TRIANGLE_BATCH));
ESP_ERROR_CHECK(grape_feature_enable(grape, GRAPE_FEATURE_PPA_TRIANGLE_OVERLAP));

/* Render using the existing GPU API. */

grape_gpu_ppa_triangle_stats_t stats;
ESP_ERROR_CHECK(grape_gpu_get_ppa_triangle_stats(gpu, &stats));

/* Return to the original path using the same API. */
ESP_ERROR_CHECK(grape_feature_disable(grape, GRAPE_FEATURE_PPA_TRIANGLE_EDGES));
```

Use feature toggles, self-test, statistics, and GPU operations from the owning task,
with no concurrent use of that GPU context. `self_test` returns the cached successful
probe result on later calls. Disabling and reenabling the feature retains the
allocated backend and its successful probe; it does not repeat hardware bring-up.

No accelerator buffers are allocated until an enabled eligible draw or explicit
self-test. Availability initially indicates that the backend was compiled for the
supported configuration. `stats.ready` indicates successful device initialization
and probing. Enabling alone is not proof that a draw used hardware.

## Run the A/B board demo

Set the existing selectors in `main/app_config.h`:

```c
#define GRAPE_APP_MODE GRAPE_APP_MODE_DEMO
#define GRAPE_APP_DEMO 11U
```

Demo 11 is named `gpu_ppa_triangle`. The implementation does not change the selected
application mode or demo for you. Build/flash using your normal ESP-IDF environment:

```text
idf.py build
idf.py flash monitor
```

The demo:

1. Saves all four feature modes.
2. Tests the same geometry at 64x64 and 256x256 target sizes.
3. Warms each mode once, then times eight passes: CPU, `checked16`, `fast16`,
   `checked64`, and `fast64`. Checked modes include per-block CPU validation;
   fast modes disable it. The 64 modes enable adaptive larger blocks.
   It then measures `overlap_checked64`, `overlap_fast16`, and `overlap_fast64`.
4. Compares every mode's final RGBA bytes against the CPU reference outside the
   timing. Requires hardware use, zero errors/mismatches, zero validation work
   in fast modes, and actual larger-block use for the 256x256 batch tests.
5. Compares feature-off and feature-on 4x MSAA results for both sizes.
6. Displays the small scene and restores all previous feature modes on exit.

Device probes and warmup are excluded from timings and reported counter deltas.
`CPU/feature` above 1.0 means a measured speedup; below 1.0 means a slowdown.
The scene is repeated without changing geometry, so this measures a warm workload.
Checked modes are omitted if validation was compiled out. The standalone device
probes and final image comparisons always remain in place.

The scene includes a shared diagonal, fractional vertices, and viewport clipping.
The clear color has nontrivial RGB and alpha, so preservation of untouched pixels
is tested as well as the triangle color. A CPU fallback that happens to match the
image cannot produce a passing hardware test on its own.

Successful bring-up logs include:

```text
CSC lane/rounding and PPA triangle probes passed (bias=...)
256x256 fast64: 8 passes=... us, CPU/feature=...x, RGBA=PASS
fast64: hardware=... blocks=... large=... pixels=... validated=0 coefficients=...
PASS: both scene sizes, all timing modes, and 4x fallback match
```

The batch timing demo passed on the user's board. The overlap additions still
need board measurement.

## What runs on hardware

Each work block uses a persistent three-byte coordinate image `(x,y,0)`.
DMA2D computes the three triangle edge expressions using custom CSC coefficients.
A second PPA blend job applies three simultaneous key thresholds and generates a
black/white coverage image. The CPU commits the covered pixels to the existing
RGBA attachment, preserving its byte order and alpha convention.

The four reusable buffers now reserve 48 KiB of internal DMA memory for 64x64
blocks, plus descriptor and driver state. Coordinates regenerate when the block
size changes. At equal covered area, 64x64 blocks can reduce the number of transfer
pairs by up to 16 times; this is not a predicted overall speedup. Large blocks may
process padding, and coefficients may require smaller blocks or CPU fallback.

This is deliberately **two hardware passes plus CPU commit**. It is not yet the
proposed fused DMA2D-to-PPA draw. The public PPA driver is used for blending, and
the IDF DMA channel allocator is used for CSC; no global channel is commandeered
and no ESP-IDF source files are patched. The narrowly scoped CSC bridge needs
the private channel structure to program the channel actually assigned to the job.

## Overlapping CPU work with transfers

With `ppa.triangle_overlap` enabled, a shared block cursor assigns one block to
hardware. While waiting for CSC or PPA completion, the same rendering task claims
other blocks and renders them in chunks of up to four rows, using the unchanged
opaque CPU rasterizer through a private bridge. It checks completion between
chunks, advances CSC to PPA, and commits the hardware block once ready. The CPU
finishes any partly rendered block before another hardware block is submitted.
The amount assigned to CPU follows completion timing rather than a fixed split.

CPU and hardware blocks have disjoint pixel bounds. Hardware writes only its
private buffers, and CPU commits all target pixels. All work finishes before the
triangle call returns, preserving draw order even for overlapping triangles.
This retains the single-sample, opaque, no-depth eligibility rules and does not
add a task or alter the existing MSAA worker.

The CPU work context belongs to the calling task and exists only during the
triangle call. ISR callbacks only signal completion. Every transfer outcome clears
temporary work pointers before returning; a timeout still quarantines DMA buffers.
If a transfer or mask check fails after CPU blocks have been drawn, the original
renderer redraws the complete opaque triangle safely.

Overlap logs include `cpu_blocks`, `cpu_pixels`, and `mixed_triangles`. A warning
reports when no CPU block was claimed during the larger scene's transfer waits.
Completions may already be ready when checked; correctness alone does not prove
useful overlap. `hardware` counts triangles using hardware, including mixed
triangles, while `blocks` counts only accelerator blocks.

Additional timings report coefficient/coordinate `prepare`, transfer setup and
cache maintenance `driver`, blocking `wait`, CPU raster chunks `cpu_render`, mask
`validate`, and hardware-result `commit`, in microseconds. Waiting excludes CPU
chunks. These are task-side measurements, not hardware engine execution times;
they do not sum exactly to pass time because draw setup, clear, polling, and other
bookkeeping remain outside those categories. Instrumentation also has a cost.
The overall `CPU/feature` ratio remains the performance result to compare.

## Exact edge encoding

For the original integer edge `E = row + sx*x + sy*y`, let
`g = gcd(abs(sx), abs(sy))`. At integer pixel indices:

```text
E >= 0  iff  (sx/g)*x + (sy/g)*y + floor(row/g) >= 0
```

This retains the original top-left `-1` bias; it does not round or normalize edge
slopes approximately. The CSC numerator is that reduced integer expression plus
32768 minus the measured hardware rounding bias. PPA checks each output byte
against 128. Constant-sign edges become constant values. Overflow, coefficient
widths, and the entire padded tile's output range are checked before submission.
The complete triangle is preflighted before any destination writes.

The device probe verifies full-byte identity/channel order, distinguishes floor
from fixed round-to-nearest behavior (other models are rejected), exercises signed
coefficients, checks all eight combinations of the three key thresholds, and
executes an exact test triangle through CSC and PPA. CPU validation catches any
remaining hardware/model discrepancy before a tile is committed.

## Build controls and diagnostics

- `CONFIG_GRAPE_PPA_TRIANGLE_COMPUTE`: default `y` on supported P4 configurations;
  only compiles the capability. Runtime default remains disabled.
- `CONFIG_GRAPE_PPA_TRIANGLE_VALIDATE`: default `y`; compares every generated mask
  with the original CPU edge equations before writing the tile when the runtime
  validation feature is active. Keep it compiled for same-run on/off comparisons.
- The private CSC bridge is pinned to **ESP-IDF 5.5.4** and excludes
  `CONFIG_DMA2D_ISR_IRAM_SAFE`. Unsupported builds retain the registry entry as
  unavailable/NOT_COMPILED and use CPU rendering. Port and revalidate the bridge
  explicitly for a different IDF version.
- The older P4 revision is supported by this RGB888 computation path; it does not
  depend on GRAY8 SRM rotation.

`grape_gpu_get_ppa_triangle_stats` returns cumulative per-GPU-context counters,
independent of normal pass statistics: triangles seen/completed, tiles computed,
unsupported/coefficients fallbacks, pixels validated, mismatches, errors, timeouts,
last error, measured rounding bias and readiness. `compute_us` covers completed
triangle jobs including hardware waits, optional validation and CPU commit, but
excludes initialization and coefficient preflight. Use the demo's wall time for
end-to-end A/B comparison.

`tiles_computed` counts actual CSC/PPA block pairs at any block size.
`large_tiles_computed` counts pairs larger than 16x16; `pixels_computed` includes
the padding processed by hardware. These counters exclude initialization probes.

On allocation, transfer, probe, or validation failure, the feature becomes
unavailable and subsequent work uses the CPU. Earlier verified writes from a
partially completed opaque triangle are safe to redraw by the original path.
Failed GPU state is not automatically retried.

Each hardware wait is bounded to one second. IDF cannot safely cancel every queued
transaction. A timed-out job is never reused and its DMA-visible buffers and callback
state are retained until late completion allows cleanup, or until reboot if it
never completes. No whole-PPA/DMA reset is performed because other clients may be
using the hardware. CPU fallback protects the render target; it cannot repair a
peripheral fault that also prevents other DMA clients from making progress.

### Descriptor coherency correction after the first board run

The first P4 v1.3 run reached demo 11, then timed out during initialization.
The demo propagated `ESP_ERR_TIMEOUT` to `main`'s `ESP_ERROR_CHECK`, which caused
the reported abort. That run did not validate the custom arithmetic or timing.

The original backend synchronized the coordinate/output buffers but omitted the
DMA descriptors. P4 internal SRAM is cached: `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`
does not make CPU descriptor writes automatically visible to DMA. The correction
allocates each descriptor in its own aligned, padded cache-line storage, writes
both descriptors back before enqueue, and invalidates the RX descriptor before
reading hardware-written status in the waiting task. RX EOF starts cleared,
matching the IDF memory-to-memory tests.

Probe logs now identify `CSC identity`, `CSC rounding`, `PPA thresholds`, and
`CSC/PPA triangle`. Timeout logs include the current operation and the last DMA
stage (queued, picked, starting, EOF callback). For a PPA timeout, the DMA stage
describes the preceding CSC operation. These diagnostics do not reset shared
channels. Demo failures still return an error; a failed experiment must not be
reported as a passing A/B comparison.

The next board run confirmed that the timeout was resolved and all arithmetic
probes passed. The measured rounding bias is 128. Across eight 1x passes, all 48
post-clipping triangles used hardware (192 tiles), with zero errors or mask
mismatches; the final RGBA comparison also passed.

Measured time was 4,888 us for CPU and 46,313 us for the feature, excluding probe
time. This implementation was approximately 9.47 times slower for this small scene
with validation enabled. This is evidence of correct non-color computation on the
accelerators for the tested inputs, not a performance win or a general matrix unit.

That run then exposed a demo setup error: the 4x MSAA reference pass reused the
1x pipeline, which `grape_gpu_bind_pipeline` correctly rejected with
`ESP_ERR_INVALID_STATE`. The demo now creates a separate 4x pipeline and uses it
for both the CPU reference and feature-enabled fallback pass. Both pipelines are
released during cleanup. The renderer's sample-count checks are unchanged.
The subsequent board log reported PASS for 1x RGBA, feature toggles and 4x fallback.
That successful run measured CPU=4,901 us and feature=46,192 us (about 9.42 times
slower). The batching run subsequently passed all image and fallback comparisons.
At 64x64, CPU took 3,677 us and fast64 took 13,941 us. At 256x256, CPU took
86,270 us and fast64 took 147,430 us. Batching reduced large-scene accelerator
blocks from 1,960 to 192, but the serial accelerator path remained slower.

## Host verification

`tools/tests/test_ppa_edge_math.py` compiles the actual encoder and compares accepted
predicates to arbitrary-precision reference edges and randomized triangles. It also
checks rejection without output modification, field limits, overflow, signed
remainders, and both admitted rounding models.

`tools/tests/test_ppa_feature_api.py` executes the actual registry/API using a minimal
host context and checks defaults, availability, context isolation, toggles, reset,
and failure state. Both accept `--clang`, `--lld`, and `--build-dir`, matching the
existing Windows host-test tooling.

`tools/tests/test_ppa_dma_handoff.py` executes the production submission/wait/EOF
functions with separate CPU and DMA memory views. It checks descriptor publication,
stale RX status, all six cache-operation failure points, enqueue failure, timeout
quarantine, and rejection of busy or failed state. It uses the same compiler options
as the other host tests. It does not emulate CSC or PPA hardware.

Observed in this implementation session: 2,363,719 pixel-sign comparisons across
9,977 accepted edge cases, 11,123 rejected cases, and 2,999 randomized nondegenerate
triangles; 12 feature-state checks; existing renderer host checks passed. Firmware
builds passed for the existing application selection and the new demo selection.
These checks do not substitute for running the board demo.

The batch update extended encoder checks to 32x32 and 64x64: 4,924,487 pixel-sign
comparisons passed. Feature tests cover 18 states. DMA handoff tests cover all
three block sizes. `test_ppa_batch_render.py` exercises actual batch dispatch and
pixel commit with a software arithmetic stand-in: 12 geometry/mode combinations,
partial blocks, coefficient-driven shrinking, untouched pixels, validation-off
counters, and rejection of a corrupted mask before commit. Hardware execution
and performance still require the expanded board demo.

Overlap verification exercises 24 mode/geometry cases using production block
dispatch, CPU rasterization, and commit code with a software CSC/PPA stand-in.
It covers partial blocks, ordered overlapping triangles, clearing temporary work
pointers, and CPU recovery after a transfer error following partial CPU work.
Completion-wait tests cover work before delayed completion, immediate completion,
and an absolute timeout despite continuously available CPU work. The feature API
test now covers 21 states. These host checks do not establish a hardware speedup.
