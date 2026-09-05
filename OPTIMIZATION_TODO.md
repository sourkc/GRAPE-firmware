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
- [ ] **MC4 / opportunistic CPU1 GPU worker**: low priority under TinyUSB/GFXLINK.
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