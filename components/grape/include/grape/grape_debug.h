#pragma once

#include "grape/grape_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_context grape_context_t;

typedef enum {
    GRAPE_DEBUG_LAYER_DAMAGE_RECTS = 0,
    GRAPE_DEBUG_LAYER_COUNT,
} grape_debug_layer_t;

esp_err_t grape_debug_set_layer_enabled(grape_context_t *context,
                                        grape_debug_layer_t layer,
                                        bool enabled);
bool grape_debug_is_layer_enabled(const grape_context_t *context,
                                  grape_debug_layer_t layer);

#ifdef __cplusplus
}
#endif
