#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct grape_context grape_context_t;

typedef enum {
    GRAPE_FEATURE_MODE_AUTO = 0,
    GRAPE_FEATURE_MODE_ENABLED = 1,
    GRAPE_FEATURE_MODE_DISABLED = 2,
} grape_feature_mode_t;

typedef enum {
    GRAPE_FEATURE_UNAVAILABLE_NONE = 0,
    GRAPE_FEATURE_UNAVAILABLE_NOT_INITIALIZED = 1,
    GRAPE_FEATURE_UNAVAILABLE_NOT_COMPILED = 2,
    GRAPE_FEATURE_UNAVAILABLE_UNSUPPORTED_HARDWARE = 3,
    GRAPE_FEATURE_UNAVAILABLE_DEPENDENCY = 4,
    GRAPE_FEATURE_UNAVAILABLE_INIT_FAILED = 5,
    GRAPE_FEATURE_UNAVAILABLE_CONFLICT = 6,
} grape_feature_unavailable_reason_t;

typedef enum {
    GRAPE_FEATURE_FLAG_ACCELERATOR = 1u << 0,
    GRAPE_FEATURE_FLAG_HAS_FALLBACK = 1u << 1,
    GRAPE_FEATURE_FLAG_RUNTIME_TOGGLE = 1u << 2,
    GRAPE_FEATURE_FLAG_REMOTE_CONFIGURABLE = 1u << 3,
    GRAPE_FEATURE_FLAG_EXPERIMENTAL = 1u << 4,
} grape_feature_flags_t;

typedef uint32_t grape_feature_id_t;

enum {
    GRAPE_FEATURE_INVALID = 0,
#define GRAPE_FEATURE_ENTRY(symbol, id, name, default_mode, flags) \
    GRAPE_FEATURE_##symbol = id,
#include "grape/grape_feature_registry.def"
#undef GRAPE_FEATURE_ENTRY
};

typedef struct {
    grape_feature_id_t id;
    const char *name;
    grape_feature_mode_t mode;
    grape_feature_mode_t default_mode;
    uint32_t flags;
    bool available;
    bool active;
    grape_feature_unavailable_reason_t unavailable_reason;
} grape_feature_info_t;

size_t grape_feature_count(void);
esp_err_t grape_feature_get_info_at(const grape_context_t *context,
                                    size_t index,
                                    grape_feature_info_t *out_info);
esp_err_t grape_feature_get_info(const grape_context_t *context,
                                 grape_feature_id_t id,
                                 grape_feature_info_t *out_info);
esp_err_t grape_feature_get_mode(const grape_context_t *context,
                                 grape_feature_id_t id,
                                 grape_feature_mode_t *out_mode);
esp_err_t grape_feature_set_mode(grape_context_t *context,
                                 grape_feature_id_t id,
                                 grape_feature_mode_t mode);
esp_err_t grape_feature_enable(grape_context_t *context, grape_feature_id_t id);
esp_err_t grape_feature_disable(grape_context_t *context, grape_feature_id_t id);
esp_err_t grape_feature_reset(grape_context_t *context, grape_feature_id_t id);
bool grape_feature_is_available(const grape_context_t *context, grape_feature_id_t id);
bool grape_feature_is_active(const grape_context_t *context, grape_feature_id_t id);

/* Returns the total number available; writes at most capacity IDs. */
size_t grape_feature_list_available(const grape_context_t *context,
                                    grape_feature_id_t *out_ids,
                                    size_t capacity);

#ifdef __cplusplus
}
#endif
