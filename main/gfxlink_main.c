#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "grape/grape.h"
#include "grape/grape_gfxlink.h"

static const char *TAG = "GRAPE";

void app_main(void)
{
    grape_context_t *grape = NULL;
    grape_config_t config = GRAPE_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(grape_init(&config, &grape));

    grape_gfxlink_t *link = NULL;
    ESP_ERROR_CHECK(grape_gfxlink_start(grape, &link));
    ESP_LOGI(TAG, "GFXLINK ready");

    for (;;) {
        ESP_ERROR_CHECK(grape_gfxlink_process(link, portMAX_DELAY));
    }
}
