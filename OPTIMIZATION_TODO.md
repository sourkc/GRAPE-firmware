# Optimization TODO

## GPU M3.5 optimization track

- [x] **MC1 / measurement baseline**
  - Opt-in per-pass GPU stats (normal rendering remains uninstrumented).
  - Stage timings: pass, vertex transform, clip, triangle setup, tile bin, tile raster, resolve.
  - Work counters: draw/input/post-clip/rasterized triangles, culls/degenerates, active tiles, tile refs, bbox pixels.
  - Deterministic `gpu3d` benchmark suite with M3 textured cube and 12-cube 4x MSAA stress case.
- [x] **MC2 / tile-job abstraction**
  - Active binned tiles are materialized as immutable `grape_gpu_tile_job_t` records.
  - CPU0 consumes jobs through a single claim seam; execution remains strictly single-core.
  - Shared tile scratch is intentionally unchanged until MC3, preserving raster behavior.
- [x] **MC3 / worker-local tile scratch**
  - `grape_gpu_worker_t` now owns color/depth tile scratch and its allocation lifetime.
  - Tile init, raster, depth, and resolve paths receive an explicit worker instead of shared context scratch.
  - CPU0 still drains all jobs through one primary worker; no concurrency or scheduling change yet.
- [x] **MC4 / opportunistic CPU1 GPU worker** (implementation complete; device acceptance pending)
  - CPU0 remains the primary worker and owns setup, binning, resource management and pass completion.
  - Core 1 helper runs at priority 1, below unchanged GFXLINK (4) / TinyUSB (5); it blocks on a private wake semaphore between nonempty multi-job batches.
  - Atomic shared claims assign each tile once. Cancellation stops new claims on error; CPU0 always joins in-flight work before releasing inputs or resetting the queue.
  - Private worker scratch, dirty bounds and timing totals are merged after the completion fence. Global telemetry updates/snapshots are protected across cores.
  - GPU contexts use internal RAM for atomic state. Scratch/task/semaphore allocation failure falls back to CPU0; context destruction wakes and joins helper shutdown before freeing resources.
  - `CONFIG_GRAPE_GPU_MULTICORE` defaults on for dual-core builds; disable for A/B comparisons. Unicore and automatic function-profiling builds use CPU0 only (the function profiler is single-writer).
  - Existing 1x direct raster path and GFXLINK synchronous request behavior remain unchanged; MC4 distributes the MC2/MC3 MSAA tile path.
  - Strict ESP32-P4 object compile checks: all eight GPU translation units plus telemetry, `-Wall -Wextra -Werror`, multicore on/off and telemetry 0/3.
  - Device acceptance: build/flash both worker configurations and compare deterministic color AND depth output for cube/grid12, 2x/4x MSAA, LOAD/CLEAR, partial edge tiles, depth on/off, and repeated/empty passes. Require identical output and unchanged active-tile/reference counts.
  - Device acceptance: verify `grape_gpu1` affinity=1, priority=1, blocked between batches; measure stack high-water mark and repeat create/render/destroy under heap/task-allocation pressure. Check fallback and no use-after-free/hangs on render errors.
  - Device acceptance: run GPU3D benchmark and real GFXLINK/USB traffic; require no corruption, deadlocks, watchdog resets or comms starvation. Compare `pass_us` / benchmark `work` time for speedup. `tile_raster_us` and `resolve_us` sum worker elapsed times, including preemption, and are not batch wall time. Aggressive synthetic preemption remains MC5.
- [ ] **MC5 / preemption torture test**.
- [ ] **MC6 / asynchronous GFXLINK request/completion path**.
- [ ] **MC7 / transport backpressure**.
- [ ] **MC8 / resource lifetime + command ordering hardening**.

* **CPU raster/compositor**

    * Eliminate `rgba8_t` temporary stack traffic; keep source/destination/result channels in registers.
    * Consider specializing compositing by render-target format so RGB565/RGB888 decisions happen outside the pixel loop.
    * Add opaque / identity-tint fast paths, especially RGB565→RGB565 direct-copy cases.
    * Revisit `mul8()`; benchmark exact `/255` alternatives and possibly LUT-based versions.
    * Inspect remaining per-pixel address/transform arithmetic for integer/fixed-point opportunities.
* **Three-shear / rotation**

    * Apply relevant caching/inlining improvements to the three-shear A8 path.
    * Profile rotation-specific bottlenecks separately; direct-raster optimizations barely affect these.
* **Damage system**

    * Force-inline/test tiny hot helpers such as `tile_region_empty()`.
    * Optimize `mark_float_aabb()`, `parallelogram_intersects_rect()`, occupancy checks, and planner tile scans.
    * Revisit `tile_index` / tile bitmap operations for cheaper addressing/bit manipulation.
* **Low-level later**

    * Inspect hot loops for ESP32-P4 PIE/SIMD opportunities.
    * Consider handwritten RISC-V only after the C-generated assembly is cleaned up.
    * Investigate fixed-point/integer affine rasterization to reduce FPU work.
* **Measurement**

    * Keep the profiler → disassembly → one controlled change → benchmark workflow.
    * Preserve the current optimized build as the new performance baseline.