# PERFORMANCE.md
#### Use this file as a guide to write fast and optimized code for the ESP32-P4


### Avoid `int64_t floorf()` in hot paths

On the ESP32-P4 rev 1.3, converting the result of `floorf()` to an
`int64_t` is EXTREMELY expensive.

Measured at 360 MHz:

| Operation | Cycles/op |
|---|---:|
| `(int64_t)floorf(a - b)` | ~678 |
| `(int32_t)floorf(a - b)` | ~56 |
| cast + correction | ~24 |

If a value is immediately discarded when it is outside a positive range,
do not do this:

```c
int64_t source_y = (int64_t)floorf(a - b);

if (source_y < 0 || source_y >= height) {
    continue;
}
```
Do this instead:

```c
float source_y_f = a - b;

if (source_y_f < 0.0f || source_y_f >= (float)height) {
continue;
}

int32_t source_y = (int32_t)source_y_f;
```

This is valid because after checking that the value is non-negative,
integer truncation produces the same result as floorf().

DO NOT blindly replace every `floorf()` with an integer cast.

For negative non-integer values they are not equivalent:

`floorf(-7.5f) == -8`

`(int32_t)-7.5f == -7`

Only use the early-bounds-check version when negative values are discarded
before the integer result is needed.

### Prefer 32-bit coordinates unless 64-bit range is actually required

The ESP32-P4 is a 32-bit CPU. In the benchmark:

`(int64_t)floorf(...)`  ~678 cycles/op
`(int32_t)floorf(...)`   ~56 cycles/op

Do not use `int64_t` for framebuffer/texture coordinates just for safety
when their possible range already fits comfortably inside `int32_t`.

### Do not put invariant format checks inside per-pixel loops

If the texture format is known before rasterization begins, dispatch to a
format-specific rasterizer once instead of checking A8/RGB565/RGB888/RGBA8888 for
every pixel.

The duplicated raster loops are intentional.

### Some functions might require forced inline
By inspecting the disassembly, that can be generated using
```bash
riscv32-esp-elf-objdump -d -S build\grape.elf > dissasembly.txt
```
it was discovered that the compiler might not inline some functions 
that are supposed to be inlined. The fix is to add the flag `__attribute__((always_inline)`
which has a downside of making the compiled image larger, but might significantly
improve performance in hot paths. 

**NOTICE: Please always run the benchmark before committing this change, there is 
no guarantee that this fix works**