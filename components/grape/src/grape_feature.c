#include "grape_internal.h"

typedef struct {
    grape_feature_id_t id;
    const char *name;
    grape_feature_mode_t default_mode;
    uint32_t flags;
} grape_feature_descriptor_t;

static const grape_feature_descriptor_t s_feature_descriptors[] = {
#define GRAPE_FEATURE_ENTRY(symbol, feature_id, feature_name, feature_default_mode, feature_flags) \
    {                                                                                              \
        .id = GRAPE_FEATURE_##symbol,                                                              \
        .name = feature_name,                                                                      \
        .default_mode = feature_default_mode,                                                      \
        .flags = feature_flags,                                                                    \
    },
#include "grape/grape_feature_registry.def"
#undef GRAPE_FEATURE_ENTRY
};

_Static_assert(
    sizeof(s_feature_descriptors) / sizeof(s_feature_descriptors[0]) == GRAPE_FEATURE_SLOT_COUNT,
    "feature registry/state size mismatch"
);

static int feature_index_from_id(grape_feature_id_t id)
{
    switch (id) {
#define GRAPE_FEATURE_ENTRY(symbol, feature_id, feature_name, feature_default_mode, feature_flags) \
        case GRAPE_FEATURE_##symbol:                                                                \
            return GRAPE_FEATURE_SLOT_##symbol;
#include "grape/grape_feature_registry.def"
#undef GRAPE_FEATURE_ENTRY
        default:
            return -1;
    }
}

static void resolve_feature(grape_context_t *context, size_t index)
{
    grape_feature_state_t *state = &context->features[index];
    state->active = state->available && state->mode != GRAPE_FEATURE_MODE_DISABLED;
}

static void fill_info(const grape_context_t *context, size_t index, grape_feature_info_t *out_info)
{
    const grape_feature_descriptor_t *descriptor = &s_feature_descriptors[index];
    const grape_feature_state_t *state = &context->features[index];

    *out_info = (grape_feature_info_t) {
        .id = descriptor->id,
        .name = descriptor->name,
        .mode = state->mode,
        .default_mode = descriptor->default_mode,
        .flags = descriptor->flags,
        .available = state->available,
        .active = state->active,
        .unavailable_reason = state->unavailable_reason,
    };
}

void grape_feature_init(grape_context_t *context)
{
    if (!context) {
        return;
    }

    for (size_t i = 0; i < GRAPE_FEATURE_SLOT_COUNT; ++i) {
        context->features[i] = (grape_feature_state_t) {
            .mode = s_feature_descriptors[i].default_mode,
            .available = false,
            .active = false,
            .unavailable_reason = GRAPE_FEATURE_UNAVAILABLE_NOT_INITIALIZED,
        };
    }
}

esp_err_t grape_feature_set_availability(grape_context_t *context,
                                         grape_feature_id_t id,
                                         bool available,
                                         grape_feature_unavailable_reason_t reason)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = feature_index_from_id(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (available) {
        reason = GRAPE_FEATURE_UNAVAILABLE_NONE;
    } else if (reason == GRAPE_FEATURE_UNAVAILABLE_NONE) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_feature_state_t *state = &context->features[index];
    state->available = available;
    state->unavailable_reason = reason;
    resolve_feature(context, (size_t)index);
    return ESP_OK;
}

size_t grape_feature_count(void)
{
    return GRAPE_FEATURE_SLOT_COUNT;
}

esp_err_t grape_feature_get_info_at(const grape_context_t *context,
                                    size_t index,
                                    grape_feature_info_t *out_info)
{
    if (!context || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }
    if (index >= GRAPE_FEATURE_SLOT_COUNT) {
        return ESP_ERR_NOT_FOUND;
    }

    fill_info(context, index, out_info);
    return ESP_OK;
}

esp_err_t grape_feature_get_info(const grape_context_t *context,
                                 grape_feature_id_t id,
                                 grape_feature_info_t *out_info)
{
    if (!context || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = feature_index_from_id(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    fill_info(context, (size_t)index, out_info);
    return ESP_OK;
}

esp_err_t grape_feature_get_mode(const grape_context_t *context,
                                 grape_feature_id_t id,
                                 grape_feature_mode_t *out_mode)
{
    if (!out_mode) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_feature_info_t info;
    esp_err_t ret = grape_feature_get_info(context, id, &info);
    if (ret != ESP_OK) {
        return ret;
    }

    *out_mode = info.mode;
    return ESP_OK;
}

esp_err_t grape_feature_set_mode(grape_context_t *context,
                                 grape_feature_id_t id,
                                 grape_feature_mode_t mode)
{
    if (!context || mode < GRAPE_FEATURE_MODE_AUTO || mode > GRAPE_FEATURE_MODE_DISABLED) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = feature_index_from_id(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const grape_feature_descriptor_t *descriptor = &s_feature_descriptors[index];
    grape_feature_state_t *state = &context->features[index];

    if ((descriptor->flags & GRAPE_FEATURE_FLAG_RUNTIME_TOGGLE) == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (mode == GRAPE_FEATURE_MODE_ENABLED && !state->available) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    state->mode = mode;
    resolve_feature(context, (size_t)index);
    return ESP_OK;
}

esp_err_t grape_feature_enable(grape_context_t *context, grape_feature_id_t id)
{
    return grape_feature_set_mode(context, id, GRAPE_FEATURE_MODE_ENABLED);
}

esp_err_t grape_feature_disable(grape_context_t *context, grape_feature_id_t id)
{
    return grape_feature_set_mode(context, id, GRAPE_FEATURE_MODE_DISABLED);
}

esp_err_t grape_feature_reset(grape_context_t *context, grape_feature_id_t id)
{
    if (!context) {
        return ESP_ERR_INVALID_ARG;
    }

    int index = feature_index_from_id(id);
    if (index < 0) {
        return ESP_ERR_NOT_FOUND;
    }

    return grape_feature_set_mode(context, id, s_feature_descriptors[index].default_mode);
}

bool grape_feature_is_available(const grape_context_t *context, grape_feature_id_t id)
{
    if (!context) {
        return false;
    }

    int index = feature_index_from_id(id);
    return index >= 0 && context->features[index].available;
}

bool grape_feature_is_active(const grape_context_t *context, grape_feature_id_t id)
{
    if (!context) {
        return false;
    }

    int index = feature_index_from_id(id);
    return index >= 0 && context->features[index].active;
}

size_t grape_feature_list_available(const grape_context_t *context,
                                    grape_feature_id_t *out_ids,
                                    size_t capacity)
{
    if (!context || (capacity > 0 && !out_ids)) {
        return 0;
    }

    size_t available_count = 0;
    for (size_t i = 0; i < GRAPE_FEATURE_SLOT_COUNT; ++i) {
        if (!context->features[i].available) {
            continue;
        }

        if (available_count < capacity) {
            out_ids[available_count] = s_feature_descriptors[i].id;
        }
        ++available_count;
    }

    return available_count;
}
