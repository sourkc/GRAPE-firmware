#include <inttypes.h>

#include "app_config.h"
#include "demos/demo.h"
#include "esp_log.h"
#include "grape/grape.h"
#include "grape/grape_benchmark.h"
#include "grape/grape_gfxlink.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if EXPERIMENTAL_SET_CLOCK_400_MHZ
#include "esp_private/regi2c_ctrl.h"
#include "esp_rom_sys.h"
#include "hal/clk_tree_ll.h"
#endif

static const char *TAG = "GRAPE";

static void app_apply_experimental_clock(void)
{
#if EXPERIMENTAL_SET_CLOCK_400_MHZ
    REGI2C_CLOCK_ENABLE();
    clk_ll_cpll_set_config(400, 40);
    REGI2C_CLOCK_DISABLE();
    esp_rom_set_cpu_ticks_per_us(400);
#endif
}

static esp_err_t app_run_gfxlink(grape_context_t *grape)
{
    grape_gfxlink_t *link = NULL;
    esp_err_t ret = grape_gfxlink_start(grape, &link);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "mode=gfxlink: ready");
    for (;;) {
        ret = grape_gfxlink_process(link, portMAX_DELAY);
        if (ret != ESP_OK) {
            return ret;
        }
    }
}

static esp_err_t app_run_benchmark(grape_context_t *grape)
{
    grape_benchmark_config_t benchmark_config = GRAPE_BENCHMARK_CONFIG_DEFAULT();
    benchmark_config.suite_mask = GRAPE_APP_BENCHMARK_SUITE_MASK;
    ESP_LOGI(TAG, "mode=benchmark: suite_mask=0x%08" PRIx32, benchmark_config.suite_mask);
    return grape_benchmark_run(grape, &benchmark_config);
}

static esp_err_t app_run_demo(grape_context_t *grape, const grape_demo_t *demo)
{
    if (!demo) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "mode=demo: %" PRIu32 " - %s", demo->id, demo->name);
    return demo->run(grape);
}

void app_main(void)
{
    app_apply_experimental_clock();

    const grape_demo_t *demo = NULL;
    grape_config_t config = GRAPE_CONFIG_DEFAULT();

    if (GRAPE_APP_MODE == GRAPE_APP_MODE_DEMO) {
        demo = grape_demo_find(GRAPE_APP_DEMO);
        if (!demo) {
            ESP_LOGE(TAG, "Unknown demo ID: %u", (unsigned)GRAPE_APP_DEMO);
            grape_demo_log_available();
            ESP_ERROR_CHECK(ESP_ERR_NOT_FOUND);
        }
        if (demo->configure) {
            demo->configure(&config);
        }
    } else if (GRAPE_APP_MODE != GRAPE_APP_MODE_GFXLINK &&
               GRAPE_APP_MODE != GRAPE_APP_MODE_BENCHMARK) {
        ESP_LOGE(TAG, "Unknown application mode: %u", (unsigned)GRAPE_APP_MODE);
        ESP_ERROR_CHECK(ESP_ERR_INVALID_ARG);
    }

    grape_context_t *grape = NULL;
    ESP_ERROR_CHECK(grape_init(&config, &grape));

    esp_err_t ret = ESP_OK;
    switch (GRAPE_APP_MODE) {
        case GRAPE_APP_MODE_GFXLINK:
            ret = app_run_gfxlink(grape);
            break;

        case GRAPE_APP_MODE_BENCHMARK:
            ret = app_run_benchmark(grape);
            break;

        case GRAPE_APP_MODE_DEMO:
            ret = app_run_demo(grape, demo);
            break;

        default:
            ret = ESP_ERR_INVALID_ARG;
            break;
    }

    if (ret == ESP_OK && GRAPE_APP_MODE == GRAPE_APP_MODE_DEMO) {
        for (;;) {
            vTaskDelay(portMAX_DELAY);
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Application mode failed: %s", esp_err_to_name(ret));
    }

    grape_deinit(grape);
    ESP_ERROR_CHECK(ret);
}
