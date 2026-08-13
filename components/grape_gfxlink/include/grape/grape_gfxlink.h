#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "grape/grape.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_gfxlink grape_gfxlink_t;

esp_err_t grape_gfxlink_start(grape_context_t *grape, grape_gfxlink_t **out_link);
esp_err_t grape_gfxlink_process(grape_gfxlink_t *link, TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif
