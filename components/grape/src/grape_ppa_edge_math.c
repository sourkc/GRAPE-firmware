#include <limits.h>
#include "grape_ppa_edge_math.h"

static uint64_t edge_gcd(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

bool grape_ppa_edge_encode(int64_t row, int64_t sx, int64_t sy,
                           uint32_t width, uint32_t height, int round_bias,
                           grape_ppa_edge_coeff_t *out)
{
    if (!out || !width || !height || width > 64 || height > 64 ||
        (round_bias != 0 && round_bias != 128) ||
        sx == INT64_MIN || sy == INT64_MIN) return false;

    int64_t dx, dy, end_x, end_y, end_xy;
    if (__builtin_mul_overflow(sx, (int64_t)(width - 1), &dx) ||
        __builtin_mul_overflow(sy, (int64_t)(height - 1), &dy) ||
        __builtin_add_overflow(row, dx, &end_x) ||
        __builtin_add_overflow(row, dy, &end_y) ||
        __builtin_add_overflow(end_x, dy, &end_xy)) return false;

    const int64_t corners[4] = {row, end_x, end_y, end_xy};
    bool all_in = true, all_out = true;
    for (unsigned i = 0; i < 4; ++i) {
        all_in &= corners[i] >= 0;
        all_out &= corners[i] < 0;
    }
    if (all_in || all_out) {
        *out = (grape_ppa_edge_coeff_t){0, 0, 0, (all_in ? 129 : 126) * 256};
        return true;
    }

    const uint64_t g = edge_gcd((uint64_t)(sx < 0 ? -sx : sx),
                                (uint64_t)(sy < 0 ? -sy : sy));
    if (!g) return false;
    const int64_t a = sx / (int64_t)g;
    const int64_t b = sy / (int64_t)g;
    /* Integer x,y: sign(g*(a*x+b*y)+row) == sign(a*x+b*y+floor(row/g)).
     * This retains the original top-left -1 bias, with no rounded slopes. */
    int64_t k = row / (int64_t)g;
    if (row % (int64_t)g < 0) --k;
    if (a < -512 || a > 511 || b < -1024 || b > 1023 ||
        k < -100000 || k > 100000) return false;
    const int64_t d = 32768 + k - round_bias;
    if (d < -131072 || d > 131071) return false;

    /* No dependence on clipping, wraparound, or signed output behavior. */
    for (unsigned y = 0; y < 2; ++y) {
        for (unsigned x = 0; x < 2; ++x) {
            const int64_t n = a * (x ? width - 1 : 0) +
                              b * (y ? height - 1 : 0) + d;
            if (n < 256 || n + round_bias > 65279) return false;
        }
    }
    *out = (grape_ppa_edge_coeff_t){(int32_t)a, (int32_t)b, 0, (int32_t)d};
    return true;
}
