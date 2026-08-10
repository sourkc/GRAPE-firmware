#include "grape_benchmark_internal.h"

extern const uint8_t grape_benchmark_font_start[]
    asm("_binary_grape_benchmark_font_ttf_start");
extern const uint8_t grape_benchmark_font_end[]
    asm("_binary_grape_benchmark_font_ttf_end");

esp_err_t grape_benchmark_fixture_font_load(grape_font_t **out_font)
{
    if (!out_font) {
        return ESP_ERR_INVALID_ARG;
    }

    return grape_font_load_memory(
        grape_benchmark_font_start,
        (size_t)(grape_benchmark_font_end - grape_benchmark_font_start),
        out_font
    );
}
