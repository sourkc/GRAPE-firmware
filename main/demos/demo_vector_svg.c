#include <math.h>
#include <stdint.h>

#include "demo_internal.h"
#include "esp_log.h"

static const char *TAG = "GRAPE_DEMO";

extern const uint8_t grape_demo_svg_start[] asm("_binary_grape_demo_svg_start");

static grape_svg_document_t *s_vector_demo_document;

esp_err_t grape_demo_vector_svg_run(grape_context_t *grape)
{
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) {
        return ESP_ERR_INVALID_STATE;
    }

    const float margin = 32.0f;
    grape_svg_document_config_t svg_config = GRAPE_SVG_DOCUMENT_CONFIG_DEFAULT();
    svg_config.x = margin;
    svg_config.y = margin;
    svg_config.width = fmaxf((float)display->width - margin * 2.0f, 1.0f);
    svg_config.height = fmaxf((float)display->height - margin * 2.0f, 1.0f);
    svg_config.samples_per_axis = 2;
    svg_config.padding_pixels = 2;

    esp_err_t ret = grape_svg_document_create(
        grape,
        (const char *)grape_demo_svg_start,
        &svg_config,
        &s_vector_demo_document
    );
    if (ret != ESP_OK) {
        return ret;
    }

    ret = grape_present(grape);
    if (ret != ESP_OK) {
        grape_svg_document_destroy(s_vector_demo_document);
        s_vector_demo_document = NULL;
        return ret;
    }

    const grape_svg_view_box_t *view_box =
        grape_svg_document_view_box(s_vector_demo_document);
    ESP_LOGI(TAG,
             "SVG demo: %u layers, viewBox %.1f %.1f %.1f %.1f",
             (unsigned)grape_svg_document_layer_count(s_vector_demo_document),
             view_box ? view_box->min_x : 0.0f,
             view_box ? view_box->min_y : 0.0f,
             view_box ? view_box->width : 0.0f,
             view_box ? view_box->height : 0.0f);
    return ESP_OK;
}
