#include "esp_log.h"
#include "grape/grape.h"
#include "grape/grape_gfxlink.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "GRAPE";

void app_main(void)
{
    grape_context_t *grape = NULL;
    grape_config_t config = GRAPE_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(grape_init(&config, &grape));
    ESP_ERROR_CHECK(grape_gfxlink_init(grape));
    ESP_ERROR_CHECK(grape_present(grape));

    const grape_display_info_t *display = grape_get_display_info(grape);
    if (display) {
        ESP_LOGI(TAG, "GFXLINK ready: %s %lux%lu",
                 display->name,
                 (unsigned long)display->width,
                 (unsigned long)display->height);
    }

    while (1) {
        ESP_ERROR_CHECK(grape_gfxlink_process(grape));
        ESP_ERROR_CHECK(grape_present(grape));
        vTaskDelay(1);
    }
}
