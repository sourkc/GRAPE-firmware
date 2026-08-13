#include "grape/grape_gfxlink.h"

esp_err_t grape_gfxlink_init(grape_context_t *grape)
{
    return grape ? ESP_OK : ESP_ERR_INVALID_ARG;
}
