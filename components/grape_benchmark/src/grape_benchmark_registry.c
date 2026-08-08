#include "grape_benchmark_internal.h"

static const grape_benchmark_suite_t s_suites[] = {
    {
        .name = "shapes",
        .cases = grape_benchmark_shape_cases,
    },
};

const grape_benchmark_suite_t *grape_benchmark_suites(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_suites) / sizeof(s_suites[0]);
    }

    return s_suites;
}
