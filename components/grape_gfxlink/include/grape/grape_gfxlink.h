#pragma once

#include "esp_err.h"
#include "grape/grape.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t grape_gfxlink_init(grape_context_t *grape);
esp_err_t grape_gfxlink_process(grape_context_t *grape);

#ifdef __cplusplus
}
#endif
