# Optimization TODO

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