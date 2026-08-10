#include "grape_benchmark_internal.h"

#include <string.h>

enum { GRAPE_BENCHMARK_FROZEN_SUITE_COUNT = 14 };

static const grape_benchmark_suite_t s_suites[] = {
    { "damage_mark", GRAPE_BENCHMARK_SUITE_DAMAGE_MARK, grape_benchmark_damage_mark_cases },
    { "damage_plan", GRAPE_BENCHMARK_SUITE_DAMAGE_PLAN, grape_benchmark_damage_plan_cases },
    { "compositor", GRAPE_BENCHMARK_SUITE_COMPOSITOR, grape_benchmark_compositor_cases },
    { "pixel_backends", GRAPE_BENCHMARK_SUITE_PIXEL_BACKENDS, grape_benchmark_pixel_backend_cases },
    { "three_shear", GRAPE_BENCHMARK_SUITE_THREE_SHEAR, grape_benchmark_three_shear_cases },
    { "fragmentation", GRAPE_BENCHMARK_SUITE_FRAGMENTATION, grape_benchmark_fragmentation_cases },
    { "presentation", GRAPE_BENCHMARK_SUITE_PRESENTATION, grape_benchmark_presentation_cases },
    { "lifecycle", GRAPE_BENCHMARK_SUITE_LIFECYCLE, grape_benchmark_lifecycle_cases },
    { "scenes", GRAPE_BENCHMARK_SUITE_SCENES, grape_benchmark_scene_cases },
    { "vector", GRAPE_BENCHMARK_SUITE_VECTOR, grape_benchmark_vector_cases },
    { "svg", GRAPE_BENCHMARK_SUITE_SVG, grape_benchmark_svg_cases },
    { "font", GRAPE_BENCHMARK_SUITE_FONT, grape_benchmark_font_cases },
    { "glyph_cache", GRAPE_BENCHMARK_SUITE_GLYPH_CACHE, grape_benchmark_glyph_cache_cases },
    { "text", GRAPE_BENCHMARK_SUITE_TEXT, grape_benchmark_text_cases },
};

_Static_assert(
    sizeof(s_suites) / sizeof(s_suites[0]) == GRAPE_BENCHMARK_FROZEN_SUITE_COUNT,
    "benchmark coverage v2 must keep all registered suites"
);

esp_err_t grape_benchmark_validate_registry(void)
{
    uint32_t mask_union = 0;

    for (size_t suite_index = 0;
         suite_index < sizeof(s_suites) / sizeof(s_suites[0]);
         ++suite_index) {
        const grape_benchmark_suite_t *suite = &s_suites[suite_index];
        if (!suite->name || !suite->cases || suite->mask == 0U ||
            (mask_union & suite->mask) != 0U) {
            return ESP_ERR_INVALID_STATE;
        }
        mask_union |= suite->mask;

        size_t case_count = 0;
        const grape_benchmark_case_t *cases = suite->cases(&case_count);
        if (!cases || case_count == 0) {
            return ESP_ERR_INVALID_STATE;
        }

        for (size_t case_index = 0; case_index < case_count; ++case_index) {
            const grape_benchmark_case_t *bench_case = &cases[case_index];
            if (!bench_case->group || !bench_case->name ||
                !bench_case->iteration ||
                bench_case->kind > GRAPE_BENCHMARK_KIND_LIFECYCLE ||
                (bench_case->flags & ~(GRAPE_BENCHMARK_CASE_PRESENT |
                                       GRAPE_BENCHMARK_CASE_CAPTURE_REFRESH_WAIT)) != 0U) {
                return ESP_ERR_INVALID_STATE;
            }

            for (size_t previous = 0; previous < case_index; ++previous) {
                if (strcmp(cases[previous].group, bench_case->group) == 0 &&
                    strcmp(cases[previous].name, bench_case->name) == 0) {
                    return ESP_ERR_INVALID_STATE;
                }
            }
        }
    }

    return mask_union == GRAPE_BENCHMARK_SUITE_ALL
        ? ESP_OK
        : ESP_ERR_INVALID_STATE;
}

const grape_benchmark_suite_t *grape_benchmark_suites(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_suites) / sizeof(s_suites[0]);
    }
    return s_suites;
}
