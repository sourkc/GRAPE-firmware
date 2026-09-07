#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Integer numerator of one DMA2D CSC lane: (a*x+b*y+c*z+d)/256. */
typedef struct {
    int32_t a, b, c, d;
} grape_ppa_edge_coeff_t;

/* Preserve the exact sign of row + sx*x + sy*y over a <=64x64 block.
 * round_bias is the hardware's measured numerator bias (0 or 128).
 * False means the caller MUST retain its original CPU rasterizer. */
bool grape_ppa_edge_encode(int64_t row, int64_t sx, int64_t sy,
                           uint32_t width, uint32_t height, int round_bias,
                           grape_ppa_edge_coeff_t *out);
