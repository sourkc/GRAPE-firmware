#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#include "grape/gfxlink_protocol.h"
#include "grape/grape_gfxlink.h"

#define GFXLINK_MAX_SURFACES 64U
#define GFXLINK_MAX_RESOURCES 32U
#define GFXLINK_MAX_TEXTURES 32U
#define GFXLINK_MAX_PATHS 32U
#define GFXLINK_MAX_SVG_DOCUMENTS 8U
#define GFXLINK_MAX_FONTS 8U
#define GFXLINK_RENDERER_MAX_PAYLOAD 256U
#define GFXLINK_QUEUE_DEPTH 4U
#define GFXLINK_TASK_STACK_SIZE 4096U
#define GFXLINK_TASK_PRIORITY 4U
#define GFXLINK_TINYUSB_TASK_STACK_SIZE 4096U
#define GFXLINK_TINYUSB_TASK_PRIORITY 5U
#define GFXLINK_CPU_CORE 1
#define GFXLINK_USB_FS_PACKET_SIZE 64U
#define GFXLINK_USB_HS_PACKET_SIZE 512U
#define GFXLINK_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
#define GFXLINK_TX_WAIT_MS 1500U
#define GFXLINK_FAST_DAMAGE_MAX 16U

static const char *TAG = "grape_gfxlink";

typedef struct {
    uint32_t handle;
    grape_surface_t *surface;
    grape_texture_t *owned_texture;
} gfxlink_surface_slot_t;

typedef struct {
    uint32_t handle;
    grape_texture_t *texture;
} gfxlink_texture_slot_t;

typedef struct {
    uint32_t handle;
    grape_path_t *path;
} gfxlink_path_slot_t;

typedef struct {
    uint32_t handle;
    grape_svg_document_t *document;
} gfxlink_svg_slot_t;

typedef struct {
    uint32_t handle;
    grape_font_t *font;
    uint8_t *backing_data;
    uint32_t backing_size;
} gfxlink_font_slot_t;

typedef struct {
    uint32_t handle;
    uint32_t kind;
    uint32_t total_size;
    uint32_t chunk_count;
    uint32_t committed_crc32;
    uint8_t *data;
    uint8_t received_bitmap[GFXLINK_RESOURCE_BITMAP_BYTES];
    uint8_t corrupt_bitmap[GFXLINK_RESOURCE_BITMAP_BYTES];
    bool committed;
} gfxlink_resource_slot_t;

typedef struct {
    uint32_t texture_handle;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} gfxlink_texture_damage_t;

typedef struct {
    uint8_t opcode;
    uint8_t fast_damage_count;
    uint16_t reserved;
    uint32_t sequence;
    uint32_t payload_size;
    gfxlink_texture_damage_t fast_damage[GFXLINK_FAST_DAMAGE_MAX];
    uint8_t payload[GFXLINK_RENDERER_MAX_PAYLOAD];
} gfxlink_renderer_request_t;

typedef struct {
    uint8_t opcode;
    uint32_t sequence;
    uint32_t payload_size;
    uint8_t payload[GFXLINK_RENDERER_MAX_PAYLOAD];
} gfxlink_renderer_response_t;

struct grape_gfxlink {
    grape_context_t *grape;
    QueueHandle_t renderer_requests;
    QueueHandle_t renderer_responses;
    TaskHandle_t task;
    uint32_t next_handle;
    gfxlink_surface_slot_t surfaces[GFXLINK_MAX_SURFACES];
    gfxlink_resource_slot_t resources[GFXLINK_MAX_RESOURCES];
    gfxlink_texture_slot_t textures[GFXLINK_MAX_TEXTURES];
    gfxlink_path_slot_t paths[GFXLINK_MAX_PATHS];
    gfxlink_svg_slot_t svg_documents[GFXLINK_MAX_SVG_DOCUMENTS];
    gfxlink_font_slot_t fonts[GFXLINK_MAX_FONTS];
    grape_glyph_cache_t *glyph_cache;
    gfxlink_texture_damage_t fast_damage[GFXLINK_FAST_DAMAGE_MAX];
    uint32_t fast_damage_count;
    gfxlink_status_t fast_write_status;
};

static grape_gfxlink_t *s_active_link;

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = GFXLINK_USB_VID,
    .idProduct = GFXLINK_USB_PID,
    .bcdDevice = 0x0200,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_qualifier_t s_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

static const uint8_t s_fs_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        1,
        0,
        GFXLINK_CONFIG_TOTAL_LEN,
        0x00,
        100
    ),
    TUD_VENDOR_DESCRIPTOR(
        GFXLINK_USB_INTERFACE,
        4,
        GFXLINK_USB_EP_OUT,
        GFXLINK_USB_EP_IN,
        GFXLINK_USB_FS_PACKET_SIZE
    ),
};

static const uint8_t s_hs_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(
        1,
        1,
        0,
        GFXLINK_CONFIG_TOTAL_LEN,
        0x00,
        100
    ),
    TUD_VENDOR_DESCRIPTOR(
        GFXLINK_USB_INTERFACE,
        4,
        GFXLINK_USB_EP_OUT,
        GFXLINK_USB_EP_IN,
        GFXLINK_USB_HS_PACKET_SIZE
    ),
};

static const char *s_string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "GRAPE",
    "GRAPE GFXLINK",
    "GFXLINK-M2",
    "GFXLINK",
};

static uint32_t from_le32(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap32(value);
#endif
}

static uint32_t to_le32(uint32_t value)
{
    return from_le32(value);
}

static uint16_t from_le16(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap16(value);
#endif
}

static uint16_t to_le16(uint16_t value)
{
    return from_le16(value);
}

static float float_from_le_bits(uint32_t bits)
{
    uint32_t native_bits = from_le32(bits);
    float value;
    memcpy(&value, &native_bits, sizeof(value));
    return value;
}

static uint32_t float_to_le_bits(float value)
{
    uint32_t native_bits;
    memcpy(&native_bits, &value, sizeof(native_bits));
    return to_le32(native_bits);
}

static uint32_t s_crc32_table[256];
static bool s_crc32_table_ready;

static void crc32_init_table(void)
{
    if (s_crc32_table_ready) {
        return;
    }

    for (uint32_t i = 0U; i < 256U; ++i) {
        uint32_t crc = i;
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
        }
        s_crc32_table[i] = crc;
    }
    s_crc32_table_ready = true;
}

static uint32_t crc32_ieee(const uint8_t *data, size_t size)
{
    crc32_init_table();
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < size; ++i) {
        crc = s_crc32_table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
    }
    return crc ^ 0xFFFFFFFFU;
}

static bool bitmap_test(const uint8_t *bitmap, uint32_t index)
{
    return (bitmap[index >> 3U] & (uint8_t)(1U << (index & 7U))) != 0U;
}

static void bitmap_set(uint8_t *bitmap, uint32_t index)
{
    bitmap[index >> 3U] |= (uint8_t)(1U << (index & 7U));
}

static void bitmap_clear(uint8_t *bitmap, uint32_t index)
{
    bitmap[index >> 3U] &= (uint8_t)~(1U << (index & 7U));
}

static uint32_t resource_chunk_count(uint32_t total_size)
{
    return (total_size + GFXLINK_RESOURCE_CHUNK_SIZE - 1U) / GFXLINK_RESOURCE_CHUNK_SIZE;
}

static uint32_t resource_chunk_size(const gfxlink_resource_slot_t *slot, uint32_t chunk_index)
{
    uint32_t offset = chunk_index * GFXLINK_RESOURCE_CHUNK_SIZE;
    uint32_t remaining = slot->total_size - offset;
    return remaining < GFXLINK_RESOURCE_CHUNK_SIZE ? remaining : GFXLINK_RESOURCE_CHUNK_SIZE;
}

static gfxlink_status_t status_from_esp_err(esp_err_t error)
{
    switch (error) {
        case ESP_OK:
            return GFXLINK_STATUS_OK;
        case ESP_ERR_INVALID_ARG:
        case ESP_ERR_INVALID_SIZE:
            return GFXLINK_STATUS_INVALID_ARGUMENT;
        case ESP_ERR_NO_MEM:
            return GFXLINK_STATUS_NO_MEMORY;
        case ESP_ERR_NOT_FOUND:
            return GFXLINK_STATUS_NOT_FOUND;
        case ESP_ERR_NOT_SUPPORTED:
            return GFXLINK_STATUS_UNSUPPORTED;
        case ESP_ERR_TIMEOUT:
        case ESP_ERR_INVALID_STATE:
            return GFXLINK_STATUS_BUSY;
        default:
            return GFXLINK_STATUS_INTERNAL;
    }
}

static void set_status_response(gfxlink_renderer_response_t *response, gfxlink_status_t status)
{
    gfxlink_status_response_t payload = {
        .status = (int32_t)to_le32((uint32_t)(int32_t)status),
    };
    memcpy(response->payload, &payload, sizeof(payload));
    response->payload_size = sizeof(payload);
}

static bool handle_in_use(const grape_gfxlink_t *link, uint32_t handle)
{
    for (uint32_t i = 0; i < GFXLINK_MAX_SURFACES; ++i) {
        if (link->surfaces[i].handle == handle) {
            return true;
        }
    }
    for (uint32_t i = 0; i < GFXLINK_MAX_RESOURCES; ++i) {
        if (link->resources[i].handle == handle) {
            return true;
        }
    }
    for (uint32_t i = 0; i < GFXLINK_MAX_TEXTURES; ++i) {
        if (link->textures[i].handle == handle) {
            return true;
        }
    }
    for (uint32_t i = 0; i < GFXLINK_MAX_PATHS; ++i) {
        if (link->paths[i].handle == handle) {
            return true;
        }
    }
    for (uint32_t i = 0; i < GFXLINK_MAX_SVG_DOCUMENTS; ++i) {
        if (link->svg_documents[i].handle == handle) {
            return true;
        }
    }
    for (uint32_t i = 0; i < GFXLINK_MAX_FONTS; ++i) {
        if (link->fonts[i].handle == handle) {
            return true;
        }
    }
    return false;
}

static uint32_t allocate_handle(grape_gfxlink_t *link)
{
    for (uint32_t attempt = 0; attempt < 256U; ++attempt) {
        uint32_t handle = link->next_handle++;
        if (link->next_handle == 0U) {
            link->next_handle = 1U;
        }
        if (handle != 0U && !handle_in_use(link, handle)) {
            return handle;
        }
    }
    return 0U;
}

static gfxlink_surface_slot_t *find_surface_slot(grape_gfxlink_t *link, uint32_t handle)
{
    if (handle == 0U) {
        return NULL;
    }
    for (uint32_t i = 0; i < GFXLINK_MAX_SURFACES; ++i) {
        if (link->surfaces[i].handle == handle && link->surfaces[i].surface) {
            return &link->surfaces[i];
        }
    }
    return NULL;
}

static gfxlink_surface_slot_t *allocate_surface_slot(grape_gfxlink_t *link)
{
    for (uint32_t i = 0; i < GFXLINK_MAX_SURFACES; ++i) {
        if (link->surfaces[i].handle == 0U &&
            !link->surfaces[i].surface &&
            !link->surfaces[i].owned_texture) {
            return &link->surfaces[i];
        }
    }
    return NULL;
}

static gfxlink_resource_slot_t *find_resource_slot(grape_gfxlink_t *link, uint32_t handle)
{
    if (handle == 0U) {
        return NULL;
    }
    for (uint32_t i = 0; i < GFXLINK_MAX_RESOURCES; ++i) {
        if (link->resources[i].handle == handle) {
            return &link->resources[i];
        }
    }
    return NULL;
}

static gfxlink_resource_slot_t *allocate_resource_slot(grape_gfxlink_t *link)
{
    for (uint32_t i = 0; i < GFXLINK_MAX_RESOURCES; ++i) {
        if (link->resources[i].handle == 0U) {
            return &link->resources[i];
        }
    }
    return NULL;
}

static gfxlink_texture_slot_t *find_texture_slot(grape_gfxlink_t *link, uint32_t handle)
{
    if (handle == 0U) {
        return NULL;
    }
    for (uint32_t i = 0; i < GFXLINK_MAX_TEXTURES; ++i) {
        if (link->textures[i].handle == handle && link->textures[i].texture) {
            return &link->textures[i];
        }
    }
    return NULL;
}

static gfxlink_texture_slot_t *allocate_texture_slot(grape_gfxlink_t *link)
{
    for (uint32_t i = 0; i < GFXLINK_MAX_TEXTURES; ++i) {
        if (link->textures[i].handle == 0U && !link->textures[i].texture) {
            return &link->textures[i];
        }
    }
    return NULL;
}

static gfxlink_path_slot_t *find_path_slot(grape_gfxlink_t *link, uint32_t handle)
{
    if (handle == 0U) return NULL;
    for (uint32_t i = 0U; i < GFXLINK_MAX_PATHS; ++i) {
        if (link->paths[i].handle == handle && link->paths[i].path) {
            return &link->paths[i];
        }
    }
    return NULL;
}

static gfxlink_path_slot_t *allocate_path_slot(grape_gfxlink_t *link)
{
    for (uint32_t i = 0U; i < GFXLINK_MAX_PATHS; ++i) {
        if (link->paths[i].handle == 0U && !link->paths[i].path) {
            return &link->paths[i];
        }
    }
    return NULL;
}

static gfxlink_svg_slot_t *find_svg_slot(grape_gfxlink_t *link, uint32_t handle)
{
    if (handle == 0U) return NULL;
    for (uint32_t i = 0U; i < GFXLINK_MAX_SVG_DOCUMENTS; ++i) {
        if (link->svg_documents[i].handle == handle &&
            link->svg_documents[i].document) {
            return &link->svg_documents[i];
        }
    }
    return NULL;
}

static gfxlink_svg_slot_t *allocate_svg_slot(grape_gfxlink_t *link)
{
    for (uint32_t i = 0U; i < GFXLINK_MAX_SVG_DOCUMENTS; ++i) {
        if (link->svg_documents[i].handle == 0U &&
            !link->svg_documents[i].document) {
            return &link->svg_documents[i];
        }
    }
    return NULL;
}

static gfxlink_font_slot_t *find_font_slot(grape_gfxlink_t *link, uint32_t handle)
{
    if (handle == 0U) return NULL;
    for (uint32_t i = 0U; i < GFXLINK_MAX_FONTS; ++i) {
        if (link->fonts[i].handle == handle && link->fonts[i].font) {
            return &link->fonts[i];
        }
    }
    return NULL;
}

static gfxlink_font_slot_t *allocate_font_slot(grape_gfxlink_t *link)
{
    for (uint32_t i = 0U; i < GFXLINK_MAX_FONTS; ++i) {
        if (link->fonts[i].handle == 0U && !link->fonts[i].font &&
            !link->fonts[i].backing_data) {
            return &link->fonts[i];
        }
    }
    return NULL;
}

static esp_err_t adopt_texture(grape_gfxlink_t *link,
                               grape_texture_t *texture,
                               uint32_t *out_handle)
{
    if (!texture || !out_handle) return ESP_ERR_INVALID_ARG;
    gfxlink_texture_slot_t *slot = allocate_texture_slot(link);
    uint32_t handle = allocate_handle(link);
    if (!slot || handle == 0U) return ESP_ERR_NO_MEM;
    slot->handle = handle;
    slot->texture = texture;
    *out_handle = handle;
    return ESP_OK;
}

static size_t pixel_bytes(uint32_t format)
{
    switch (format) {
        case GFXLINK_PIXEL_FORMAT_RGB565: return 2U;
        case GFXLINK_PIXEL_FORMAT_RGB888: return 3U;
        case GFXLINK_PIXEL_FORMAT_A8: return 1U;
        default: return 0U;
    }
}

static esp_err_t create_solid_surface(grape_gfxlink_t *link,
                                      const gfxlink_create_solid_surface_request_t *request,
                                      uint32_t *out_handle)
{
    uint32_t width = from_le32(request->width);
    uint32_t height = from_le32(request->height);
    if (!out_handle || width == 0U || height == 0U || width > 4096U || height > 4096U ||
        (uint64_t)width * (uint64_t)height > 4U * 1024U * 1024U) {
        return ESP_ERR_INVALID_ARG;
    }

    gfxlink_surface_slot_t *slot = allocate_surface_slot(link);
    uint32_t handle = allocate_handle(link);
    if (!slot || handle == 0U) {
        return ESP_ERR_NO_MEM;
    }

    grape_texture_desc_t texture_desc = {
        .width = width,
        .height = height,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_DEFAULT,
    };

    grape_texture_t *texture = NULL;
    esp_err_t ret = grape_texture_create(link->grape, &texture_desc, &texture);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *pixels = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    if (!pixels || stride < width) {
        grape_texture_destroy(texture);
        return ESP_FAIL;
    }
    for (uint32_t y = 0; y < height; ++y) {
        memset(pixels + y * stride, 255, width);
    }

    ret = grape_texture_invalidate(texture);
    if (ret != ESP_OK) {
        grape_texture_destroy(texture);
        return ret;
    }

    grape_surface_t *surface = NULL;
    ret = grape_surface_create(link->grape, texture, &surface);
    if (ret != ESP_OK) {
        grape_texture_destroy(texture);
        return ret;
    }

    grape_color_t color = {
        .r = request->r,
        .g = request->g,
        .b = request->b,
        .a = request->a,
    };
    ret = grape_surface_set_tint(surface, color);
    if (ret == ESP_OK) {
        ret = grape_surface_set_position(
            surface,
            float_from_le_bits(request->x_bits),
            float_from_le_bits(request->y_bits)
        );
    }
    if (ret != ESP_OK) {
        grape_surface_destroy(surface);
        grape_texture_destroy(texture);
        return ret;
    }

    slot->handle = handle;
    slot->surface = surface;
    slot->owned_texture = texture;
    *out_handle = handle;
    return ESP_OK;
}

static esp_err_t set_surface_position(grape_gfxlink_t *link,
                                      const gfxlink_set_surface_position_request_t *request)
{
    uint32_t handle = from_le32(request->handle);
    gfxlink_surface_slot_t *slot = find_surface_slot(link, handle);
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }
    return grape_surface_set_position(
        slot->surface,
        float_from_le_bits(request->x_bits),
        float_from_le_bits(request->y_bits)
    );
}

static esp_err_t set_surface_color(grape_gfxlink_t *link,
                                   const gfxlink_set_surface_color_request_t *request)
{
    uint32_t handle = from_le32(request->handle);
    gfxlink_surface_slot_t *slot = find_surface_slot(link, handle);
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }
    grape_color_t color = {
        .r = request->r,
        .g = request->g,
        .b = request->b,
        .a = request->a,
    };
    return grape_surface_set_tint(slot->surface, color);
}

static esp_err_t destroy_surface(grape_gfxlink_t *link,
                                 const gfxlink_destroy_surface_request_t *request)
{
    uint32_t handle = from_le32(request->handle);
    gfxlink_surface_slot_t *slot = find_surface_slot(link, handle);
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = grape_surface_destroy(slot->surface);
    if (ret != ESP_OK) {
        return ret;
    }
    slot->surface = NULL;

    ret = slot->owned_texture ? grape_texture_destroy(slot->owned_texture) : ESP_OK;
    if (ret != ESP_OK) {
        return ret;
    }
    slot->owned_texture = NULL;
    slot->handle = 0U;
    return ESP_OK;
}


static esp_err_t create_surface(grape_gfxlink_t *link,
                                const gfxlink_create_surface_request_t *request,
                                uint32_t *out_handle)
{
    if (!out_handle) {
        return ESP_ERR_INVALID_ARG;
    }

    gfxlink_texture_slot_t *texture_slot =
        find_texture_slot(link, from_le32(request->texture_handle));
    if (!texture_slot) {
        return ESP_ERR_NOT_FOUND;
    }

    gfxlink_surface_slot_t *slot = allocate_surface_slot(link);
    uint32_t handle = allocate_handle(link);
    if (!slot || handle == 0U) {
        return ESP_ERR_NO_MEM;
    }

    grape_surface_t *surface = NULL;
    esp_err_t ret = grape_surface_create(link->grape, texture_slot->texture, &surface);
    if (ret != ESP_OK) {
        return ret;
    }

    slot->handle = handle;
    slot->surface = surface;
    slot->owned_texture = NULL;
    *out_handle = handle;
    return ESP_OK;
}

static esp_err_t set_surface_texture(grape_gfxlink_t *link,
                                     const gfxlink_set_surface_texture_request_t *request)
{
    gfxlink_surface_slot_t *surface_slot =
        find_surface_slot(link, from_le32(request->handle));
    gfxlink_texture_slot_t *texture_slot =
        find_texture_slot(link, from_le32(request->texture_handle));
    if (!surface_slot || !texture_slot) {
        return ESP_ERR_NOT_FOUND;
    }
    if (surface_slot->owned_texture) {
        return ESP_ERR_INVALID_STATE;
    }
    return grape_surface_set_texture(surface_slot->surface, texture_slot->texture);
}

static esp_err_t set_surface_transform(grape_gfxlink_t *link,
                                       const gfxlink_set_surface_transform_request_t *request)
{
    gfxlink_surface_slot_t *slot = find_surface_slot(link, from_le32(request->handle));
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }
    grape_transform_t transform = {
        .x = float_from_le_bits(request->x_bits),
        .y = float_from_le_bits(request->y_bits),
        .scale_x = float_from_le_bits(request->scale_x_bits),
        .scale_y = float_from_le_bits(request->scale_y_bits),
        .rotation = float_from_le_bits(request->rotation_bits),
        .origin_x = float_from_le_bits(request->origin_x_bits),
        .origin_y = float_from_le_bits(request->origin_y_bits),
    };
    return grape_surface_set_transform(slot->surface, &transform);
}

static esp_err_t set_surface_scale(grape_gfxlink_t *link,
                                   const gfxlink_set_surface_scale_request_t *request)
{
    gfxlink_surface_slot_t *slot = find_surface_slot(link, from_le32(request->handle));
    if (!slot) return ESP_ERR_NOT_FOUND;
    return grape_surface_set_scale(slot->surface,
                                   float_from_le_bits(request->scale_x_bits),
                                   float_from_le_bits(request->scale_y_bits));
}

static esp_err_t set_surface_rotation(grape_gfxlink_t *link,
                                      const gfxlink_set_surface_rotation_request_t *request)
{
    gfxlink_surface_slot_t *slot = find_surface_slot(link, from_le32(request->handle));
    if (!slot) return ESP_ERR_NOT_FOUND;
    return grape_surface_set_rotation(slot->surface,
                                      float_from_le_bits(request->rotation_bits));
}

static esp_err_t set_surface_origin(grape_gfxlink_t *link,
                                    const gfxlink_set_surface_origin_request_t *request)
{
    gfxlink_surface_slot_t *slot = find_surface_slot(link, from_le32(request->handle));
    if (!slot) return ESP_ERR_NOT_FOUND;
    return grape_surface_set_origin(slot->surface,
                                    float_from_le_bits(request->origin_x_bits),
                                    float_from_le_bits(request->origin_y_bits));
}

static esp_err_t set_surface_z(grape_gfxlink_t *link,
                               const gfxlink_set_surface_z_request_t *request)
{
    gfxlink_surface_slot_t *slot = find_surface_slot(link, from_le32(request->handle));
    if (!slot) return ESP_ERR_NOT_FOUND;
    int32_t z = (int32_t)from_le32((uint32_t)request->z);
    return grape_surface_set_z(slot->surface, z);
}

static esp_err_t set_surface_opacity(grape_gfxlink_t *link,
                                     const gfxlink_set_surface_opacity_request_t *request)
{
    gfxlink_surface_slot_t *slot = find_surface_slot(link, from_le32(request->handle));
    if (!slot) return ESP_ERR_NOT_FOUND;
    return grape_surface_set_opacity(slot->surface, request->opacity);
}

static esp_err_t set_surface_visible(grape_gfxlink_t *link,
                                     const gfxlink_set_surface_visible_request_t *request)
{
    gfxlink_surface_slot_t *slot = find_surface_slot(link, from_le32(request->handle));
    if (!slot || request->visible > 1U) {
        return slot ? ESP_ERR_INVALID_ARG : ESP_ERR_NOT_FOUND;
    }
    return grape_surface_set_visible(slot->surface, request->visible != 0U);
}

static esp_err_t create_texture(grape_gfxlink_t *link,
                                const gfxlink_texture_create_request_t *request,
                                uint32_t *out_handle,
                                uint32_t *out_stride,
                                uint32_t *out_size)
{
    if (!out_handle || !out_stride || !out_size) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t width = from_le32(request->width);
    uint32_t height = from_le32(request->height);
    uint32_t format = from_le32(request->format);
    uint32_t resource_handle = from_le32(request->resource_handle);
    size_t bpp = pixel_bytes(format);
    if (width == 0U || height == 0U || bpp == 0U ||
        width > SIZE_MAX / bpp) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t row_bytes = (size_t)width * bpp;
    if (height > SIZE_MAX / row_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t total_size = row_bytes * height;
    if (total_size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    gfxlink_resource_slot_t *resource = NULL;
    if (resource_handle != 0U) {
        resource = find_resource_slot(link, resource_handle);
        if (!resource || !resource->committed) {
            return ESP_ERR_NOT_FOUND;
        }
        if (resource->kind != GFXLINK_RESOURCE_TEXTURE || resource->total_size != total_size) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    gfxlink_texture_slot_t *slot = allocate_texture_slot(link);
    uint32_t handle = allocate_handle(link);
    if (!slot || handle == 0U) {
        return ESP_ERR_NO_MEM;
    }

    grape_texture_desc_t desc = {
        .width = width,
        .height = height,
        .format = (grape_pixel_format_t)format,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    grape_texture_t *texture = NULL;
    esp_err_t ret = grape_texture_create(link->grape, &desc, &texture);
    if (ret != ESP_OK) {
        return ret;
    }

    if (resource) {
        uint8_t *dst = grape_texture_pixels(texture);
        size_t stride = grape_texture_stride(texture);
        if (!dst || stride < row_bytes) {
            grape_texture_destroy(texture);
            return ESP_FAIL;
        }
        for (uint32_t y = 0U; y < height; ++y) {
            memcpy(dst + (size_t)y * stride,
                   resource->data + (size_t)y * row_bytes,
                   row_bytes);
        }
        ret = grape_texture_invalidate(texture);
        if (ret != ESP_OK) {
            grape_texture_destroy(texture);
            return ret;
        }
    }

    slot->handle = handle;
    slot->texture = texture;
    *out_handle = handle;
    *out_stride = (uint32_t)grape_texture_stride(texture);
    *out_size = (uint32_t)total_size;
    return ESP_OK;
}

static esp_err_t update_texture(grape_gfxlink_t *link,
                                const gfxlink_texture_update_request_t *request)
{
    gfxlink_texture_slot_t *texture_slot =
        find_texture_slot(link, from_le32(request->texture_handle));
    gfxlink_resource_slot_t *resource =
        find_resource_slot(link, from_le32(request->resource_handle));
    if (!texture_slot || !resource || !resource->committed) {
        return ESP_ERR_NOT_FOUND;
    }
    if (resource->kind != GFXLINK_RESOURCE_TEXTURE) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t x = from_le32(request->x);
    uint32_t y = from_le32(request->y);
    uint32_t width = from_le32(request->width);
    uint32_t height = from_le32(request->height);
    uint32_t texture_width = grape_texture_width(texture_slot->texture);
    uint32_t texture_height = grape_texture_height(texture_slot->texture);
    uint32_t format = (uint32_t)grape_texture_format(texture_slot->texture);
    size_t bpp = pixel_bytes(format);
    if (width == 0U || height == 0U || bpp == 0U ||
        x > texture_width || y > texture_height ||
        width > texture_width - x || height > texture_height - y ||
        width > SIZE_MAX / bpp) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t row_bytes = (size_t)width * bpp;
    if (height > SIZE_MAX / row_bytes ||
        resource->total_size != row_bytes * height) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *dst = grape_texture_pixels(texture_slot->texture);
    size_t stride = grape_texture_stride(texture_slot->texture);
    if (!dst || stride < (size_t)(x + width) * bpp) {
        return ESP_FAIL;
    }
    for (uint32_t row = 0U; row < height; ++row) {
        memcpy(dst + (size_t)(y + row) * stride + (size_t)x * bpp,
               resource->data + (size_t)row * row_bytes,
               row_bytes);
    }
    return grape_texture_invalidate(texture_slot->texture);
}

static bool texture_damage_touches(const gfxlink_texture_damage_t *a,
                                   const gfxlink_texture_damage_t *b)
{
    uint64_t ax2 = (uint64_t)a->x + a->width;
    uint64_t ay2 = (uint64_t)a->y + a->height;
    uint64_t bx2 = (uint64_t)b->x + b->width;
    uint64_t by2 = (uint64_t)b->y + b->height;

    return a->texture_handle == b->texture_handle &&
           (uint64_t)a->x <= bx2 && (uint64_t)b->x <= ax2 &&
           (uint64_t)a->y <= by2 && (uint64_t)b->y <= ay2;
}

static void texture_damage_union(gfxlink_texture_damage_t *dst,
                                 const gfxlink_texture_damage_t *src)
{
    uint32_t x1 = dst->x < src->x ? dst->x : src->x;
    uint32_t y1 = dst->y < src->y ? dst->y : src->y;
    uint64_t dst_x2 = (uint64_t)dst->x + dst->width;
    uint64_t dst_y2 = (uint64_t)dst->y + dst->height;
    uint64_t src_x2 = (uint64_t)src->x + src->width;
    uint64_t src_y2 = (uint64_t)src->y + src->height;
    uint32_t x2 = (uint32_t)(dst_x2 > src_x2 ? dst_x2 : src_x2);
    uint32_t y2 = (uint32_t)(dst_y2 > src_y2 ? dst_y2 : src_y2);

    dst->x = x1;
    dst->y = y1;
    dst->width = x2 - x1;
    dst->height = y2 - y1;
}

static gfxlink_status_t record_fast_damage(grape_gfxlink_t *link,
                                           const gfxlink_texture_damage_t *damage)
{
    for (uint32_t i = 0U; i < link->fast_damage_count; ++i) {
        if (texture_damage_touches(&link->fast_damage[i], damage)) {
            texture_damage_union(&link->fast_damage[i], damage);
            return GFXLINK_STATUS_OK;
        }
    }

    if (link->fast_damage_count >= GFXLINK_FAST_DAMAGE_MAX) {
        return GFXLINK_STATUS_BUSY;
    }

    link->fast_damage[link->fast_damage_count++] = *damage;
    return GFXLINK_STATUS_OK;
}

static gfxlink_status_t write_texture_rect(grape_gfxlink_t *link,
                                           const uint8_t *payload,
                                           uint32_t payload_size)
{
    if (!payload || payload_size <= sizeof(gfxlink_texture_write_rect_request_t)) {
        return GFXLINK_STATUS_INVALID_ARGUMENT;
    }

    gfxlink_texture_write_rect_request_t request;
    memcpy(&request, payload, sizeof(request));

    uint32_t texture_handle = from_le32(request.texture_handle);
    uint32_t x = from_le32(request.x);
    uint32_t y = from_le32(request.y);
    uint32_t width = from_le32(request.width);
    uint32_t height = from_le32(request.height);
    uint32_t data_offset = from_le32(request.data_offset);
    uint32_t data_size = from_le32(request.data_size);
    const uint8_t *data = payload + sizeof(request);

    if (data_size == 0U ||
        data_size != payload_size - sizeof(request) ||
        data_size > GFXLINK_TEXTURE_WRITE_RECT_CHUNK_SIZE) {
        return GFXLINK_STATUS_INVALID_ARGUMENT;
    }

    gfxlink_texture_slot_t *slot = find_texture_slot(link, texture_handle);
    if (!slot) {
        return GFXLINK_STATUS_NOT_FOUND;
    }

    grape_texture_t *texture = slot->texture;
    uint32_t texture_width = grape_texture_width(texture);
    uint32_t texture_height = grape_texture_height(texture);
    size_t bpp = pixel_bytes((uint32_t)grape_texture_format(texture));
    if (width == 0U || height == 0U || bpp == 0U ||
        x > texture_width || y > texture_height ||
        width > texture_width - x || height > texture_height - y ||
        width > SIZE_MAX / bpp) {
        return GFXLINK_STATUS_INVALID_ARGUMENT;
    }

    size_t row_bytes = (size_t)width * bpp;
    if (height > SIZE_MAX / row_bytes) {
        return GFXLINK_STATUS_INVALID_ARGUMENT;
    }
    size_t total_size = row_bytes * height;
    if ((uint64_t)data_offset + data_size > total_size) {
        return GFXLINK_STATUS_INVALID_ARGUMENT;
    }

    uint8_t *dst = grape_texture_pixels(texture);
    size_t stride = grape_texture_stride(texture);
    if (!dst || stride < (size_t)(x + width) * bpp) {
        return GFXLINK_STATUS_INTERNAL;
    }

    size_t logical_offset = data_offset;
    size_t remaining = data_size;
    size_t source_offset = 0U;
    while (remaining > 0U) {
        size_t row = logical_offset / row_bytes;
        size_t column_bytes = logical_offset % row_bytes;
        size_t copy_size = row_bytes - column_bytes;
        if (copy_size > remaining) {
            copy_size = remaining;
        }

        memcpy(dst + (size_t)(y + row) * stride + (size_t)x * bpp + column_bytes,
               data + source_offset,
               copy_size);
        logical_offset += copy_size;
        source_offset += copy_size;
        remaining -= copy_size;
    }

    if ((size_t)data_offset + data_size == total_size) {
        gfxlink_texture_damage_t damage = {
            .texture_handle = texture_handle,
            .x = x,
            .y = y,
            .width = width,
            .height = height,
        };
        return record_fast_damage(link, &damage);
    }

    return GFXLINK_STATUS_OK;
}

static esp_err_t destroy_texture(grape_gfxlink_t *link,
                                 const gfxlink_texture_handle_request_t *request)
{
    gfxlink_texture_slot_t *slot = find_texture_slot(link, from_le32(request->handle));
    if (!slot) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = grape_texture_destroy(slot->texture);
    if (ret != ESP_OK) {
        return ret;
    }
    slot->texture = NULL;
    slot->handle = 0U;
    return ESP_OK;
}


static esp_err_t create_path(grape_gfxlink_t *link,
                             const gfxlink_path_create_request_t *request,
                             uint32_t *out_handle)
{
    if (!out_handle) return ESP_ERR_INVALID_ARG;

    uint32_t resource_handle = from_le32(request->resource_handle);
    uint32_t command_count = from_le32(request->command_count);
    if (command_count == 0U ||
        command_count > GFXLINK_MAX_RESOURCE_SIZE / sizeof(gfxlink_path_command_t)) {
        return ESP_ERR_INVALID_ARG;
    }

    gfxlink_resource_slot_t *resource = find_resource_slot(link, resource_handle);
    if (!resource || !resource->committed) return ESP_ERR_NOT_FOUND;
    if (resource->kind != GFXLINK_RESOURCE_VECTOR ||
        resource->total_size != command_count * sizeof(gfxlink_path_command_t)) {
        return ESP_ERR_INVALID_SIZE;
    }

    gfxlink_path_slot_t *slot = allocate_path_slot(link);
    uint32_t handle = allocate_handle(link);
    if (!slot || handle == 0U) return ESP_ERR_NO_MEM;

    grape_path_t *path = NULL;
    esp_err_t ret = grape_path_create(&path);
    if (ret != ESP_OK) return ret;

    for (uint32_t i = 0U; i < command_count && ret == ESP_OK; ++i) {
        gfxlink_path_command_t command;
        memcpy(&command,
               resource->data + (size_t)i * sizeof(command),
               sizeof(command));

        if (from_le16(command.reserved) != 0U ||
            (command.flags & ~(GFXLINK_PATH_FLAG_LARGE_ARC |
                               GFXLINK_PATH_FLAG_SWEEP)) != 0U) {
            ret = ESP_ERR_INVALID_ARG;
            break;
        }

        float v[GFXLINK_PATH_COMMAND_VALUE_COUNT];
        for (uint32_t j = 0U; j < GFXLINK_PATH_COMMAND_VALUE_COUNT; ++j) {
            v[j] = float_from_le_bits(command.values[j]);
        }

        switch (command.type) {
            case GFXLINK_PATH_MOVE_TO:
                if (command.flags != 0U) ret = ESP_ERR_INVALID_ARG;
                else ret = grape_path_move_to(path, v[0], v[1]);
                break;
            case GFXLINK_PATH_LINE_TO:
                if (command.flags != 0U) ret = ESP_ERR_INVALID_ARG;
                else ret = grape_path_line_to(path, v[0], v[1]);
                break;
            case GFXLINK_PATH_QUAD_TO:
                if (command.flags != 0U) ret = ESP_ERR_INVALID_ARG;
                else ret = grape_path_quad_to(path, v[0], v[1], v[2], v[3]);
                break;
            case GFXLINK_PATH_CUBIC_TO:
                if (command.flags != 0U) ret = ESP_ERR_INVALID_ARG;
                else ret = grape_path_cubic_to(
                    path, v[0], v[1], v[2], v[3], v[4], v[5]);
                break;
            case GFXLINK_PATH_ARC_TO:
                ret = grape_path_arc_to(
                    path,
                    v[0], v[1], v[2],
                    (command.flags & GFXLINK_PATH_FLAG_LARGE_ARC) != 0U,
                    (command.flags & GFXLINK_PATH_FLAG_SWEEP) != 0U,
                    v[3], v[4]
                );
                break;
            case GFXLINK_PATH_CLOSE:
                if (command.flags != 0U) ret = ESP_ERR_INVALID_ARG;
                else ret = grape_path_close(path);
                break;
            default:
                ret = ESP_ERR_INVALID_ARG;
                break;
        }
    }

    if (ret != ESP_OK) {
        grape_path_destroy(path);
        return ret;
    }

    slot->handle = handle;
    slot->path = path;
    *out_handle = handle;
    return ESP_OK;
}

static esp_err_t rasterize_path(grape_gfxlink_t *link,
                                const gfxlink_path_rasterize_request_t *request,
                                gfxlink_raster_texture_response_t *out_result)
{
    if (!out_result) return ESP_ERR_INVALID_ARG;
    gfxlink_path_slot_t *slot =
        find_path_slot(link, from_le32(request->path_handle));
    if (!slot) return ESP_ERR_NOT_FOUND;

    grape_path_rasterize_config_t config = GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT();
    config.pixels_per_unit = float_from_le_bits(request->pixels_per_unit_bits);
    config.samples_per_axis = request->samples_per_axis;
    config.padding_pixels = from_le32(request->padding_pixels);
    config.memory = GRAPE_MEMORY_DEFAULT;

    grape_path_raster_t raster = {0};
    esp_err_t ret = grape_path_rasterize_a8(
        link->grape, slot->path, &config, &raster);
    if (ret != ESP_OK) return ret;

    uint32_t texture_handle = 0U;
    ret = adopt_texture(link, raster.texture, &texture_handle);
    if (ret != ESP_OK) {
        grape_texture_destroy(raster.texture);
        return ret;
    }

    *out_result = (gfxlink_raster_texture_response_t){
        .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
        .texture_handle = to_le32(texture_handle),
        .width = to_le32(grape_texture_width(raster.texture)),
        .height = to_le32(grape_texture_height(raster.texture)),
        .stride = to_le32((uint32_t)grape_texture_stride(raster.texture)),
        .origin_x_bits = float_to_le_bits(raster.path_origin_x),
        .origin_y_bits = float_to_le_bits(raster.path_origin_y),
        .pixels_per_unit_bits = float_to_le_bits(raster.pixels_per_unit),
    };
    return ESP_OK;
}

static esp_err_t destroy_path(grape_gfxlink_t *link,
                              const gfxlink_path_handle_request_t *request)
{
    gfxlink_path_slot_t *slot = find_path_slot(link, from_le32(request->handle));
    if (!slot) return ESP_ERR_NOT_FOUND;
    esp_err_t ret = grape_path_destroy(slot->path);
    if (ret != ESP_OK) return ret;
    memset(slot, 0, sizeof(*slot));
    return ESP_OK;
}

static esp_err_t create_svg(grape_gfxlink_t *link,
                            const gfxlink_svg_create_request_t *request,
                            gfxlink_svg_create_response_t *out_result)
{
    if (!out_result) return ESP_ERR_INVALID_ARG;
    gfxlink_resource_slot_t *resource =
        find_resource_slot(link, from_le32(request->resource_handle));
    if (!resource || !resource->committed) return ESP_ERR_NOT_FOUND;
    if (resource->kind != GFXLINK_RESOURCE_SVG || resource->total_size < 2U ||
        resource->data[resource->total_size - 1U] != '\0' ||
        memchr(resource->data, '\0', resource->total_size - 1U) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gfxlink_svg_slot_t *slot = allocate_svg_slot(link);
    uint32_t handle = allocate_handle(link);
    if (!slot || handle == 0U) return ESP_ERR_NO_MEM;

    grape_svg_document_config_t config = GRAPE_SVG_DOCUMENT_CONFIG_DEFAULT();
    config.x = float_from_le_bits(request->x_bits);
    config.y = float_from_le_bits(request->y_bits);
    config.width = float_from_le_bits(request->width_bits);
    config.height = float_from_le_bits(request->height_bits);
    config.samples_per_axis = request->samples_per_axis;
    config.padding_pixels = from_le32(request->padding_pixels);
    config.memory = GRAPE_MEMORY_DEFAULT;
    config.z_base = (int32_t)from_le32((uint32_t)request->z_base);

    grape_svg_document_t *document = NULL;
    esp_err_t ret = grape_svg_document_create(
        link->grape, (const char *)resource->data, &config, &document);
    if (ret != ESP_OK) return ret;

    size_t layer_count = grape_svg_document_layer_count(document);
    const grape_svg_view_box_t *view_box = grape_svg_document_view_box(document);
    if (!view_box || layer_count > UINT32_MAX) {
        grape_svg_document_destroy(document);
        return ESP_ERR_INVALID_SIZE;
    }

    slot->handle = handle;
    slot->document = document;
    *out_result = (gfxlink_svg_create_response_t){
        .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
        .handle = to_le32(handle),
        .layer_count = to_le32((uint32_t)layer_count),
        .view_box_min_x_bits = float_to_le_bits(view_box->min_x),
        .view_box_min_y_bits = float_to_le_bits(view_box->min_y),
        .view_box_width_bits = float_to_le_bits(view_box->width),
        .view_box_height_bits = float_to_le_bits(view_box->height),
    };
    return ESP_OK;
}

static esp_err_t destroy_svg(grape_gfxlink_t *link,
                             const gfxlink_svg_handle_request_t *request)
{
    gfxlink_svg_slot_t *slot = find_svg_slot(link, from_le32(request->handle));
    if (!slot) return ESP_ERR_NOT_FOUND;
    esp_err_t ret = grape_svg_document_destroy(slot->document);
    if (ret != ESP_OK) return ret;
    memset(slot, 0, sizeof(*slot));
    return ESP_OK;
}

static esp_err_t create_font(grape_gfxlink_t *link,
                             const gfxlink_font_create_request_t *request,
                             gfxlink_font_create_response_t *out_result)
{
    if (!out_result) return ESP_ERR_INVALID_ARG;
    gfxlink_resource_slot_t *resource =
        find_resource_slot(link, from_le32(request->resource_handle));
    if (!resource || !resource->committed) return ESP_ERR_NOT_FOUND;
    if (resource->kind != GFXLINK_RESOURCE_FONT) return ESP_ERR_INVALID_ARG;

    gfxlink_font_slot_t *slot = allocate_font_slot(link);
    uint32_t handle = allocate_handle(link);
    if (!slot || handle == 0U) return ESP_ERR_NO_MEM;

    grape_font_t *font = NULL;
    esp_err_t ret = grape_font_load_memory(
        resource->data, resource->total_size, &font);
    if (ret != ESP_OK) return ret;

    uint8_t *backing_data = resource->data;
    uint32_t backing_size = resource->total_size;
    memset(resource, 0, sizeof(*resource));

    slot->handle = handle;
    slot->font = font;
    slot->backing_data = backing_data;
    slot->backing_size = backing_size;

    *out_result = (gfxlink_font_create_response_t){
        .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
        .handle = to_le32(handle),
        .units_per_em = to_le32(grape_font_units_per_em(font)),
        .glyph_count = to_le32(grape_font_glyph_count(font)),
        .cmap_format = to_le32(grape_font_cmap_format(font)),
        .ascender = (int32_t)to_le32((uint32_t)(int32_t)grape_font_ascender(font)),
        .descender = (int32_t)to_le32((uint32_t)(int32_t)grape_font_descender(font)),
        .line_gap = (int32_t)to_le32((uint32_t)(int32_t)grape_font_line_gap(font)),
    };
    return ESP_OK;
}

static esp_err_t destroy_font(grape_gfxlink_t *link,
                              const gfxlink_font_handle_request_t *request)
{
    gfxlink_font_slot_t *slot = find_font_slot(link, from_le32(request->handle));
    if (!slot) return ESP_ERR_NOT_FOUND;

    if (link->glyph_cache) {
        esp_err_t ret = grape_glyph_cache_clear(link->glyph_cache);
        if (ret != ESP_OK) return ret;
    }

    esp_err_t ret = grape_font_destroy(slot->font);
    if (ret != ESP_OK) return ret;
    free(slot->backing_data);
    memset(slot, 0, sizeof(*slot));
    return ESP_OK;
}

static esp_err_t ensure_glyph_cache(grape_gfxlink_t *link)
{
    if (link->glyph_cache) return ESP_OK;
    grape_glyph_cache_config_t config = GRAPE_GLYPH_CACHE_CONFIG_DEFAULT();
    return grape_glyph_cache_create(link->grape, &config, &link->glyph_cache);
}

static bool utf8_decode_one(const uint8_t *data,
                            size_t size,
                            size_t *io_offset,
                            uint32_t *out_codepoint)
{
    size_t i = *io_offset;
    if (i >= size) return false;

    uint8_t b0 = data[i++];
    uint32_t codepoint;
    uint32_t minimum;
    uint32_t continuation_count;

    if (b0 < 0x80U) {
        codepoint = b0;
        minimum = 0U;
        continuation_count = 0U;
    } else if ((b0 & 0xE0U) == 0xC0U) {
        codepoint = b0 & 0x1FU;
        minimum = 0x80U;
        continuation_count = 1U;
    } else if ((b0 & 0xF0U) == 0xE0U) {
        codepoint = b0 & 0x0FU;
        minimum = 0x800U;
        continuation_count = 2U;
    } else if ((b0 & 0xF8U) == 0xF0U) {
        codepoint = b0 & 0x07U;
        minimum = 0x10000U;
        continuation_count = 3U;
    } else {
        return false;
    }

    if (i + continuation_count > size) return false;
    for (uint32_t n = 0U; n < continuation_count; ++n) {
        uint8_t b = data[i++];
        if ((b & 0xC0U) != 0x80U) return false;
        codepoint = (codepoint << 6U) | (b & 0x3FU);
    }

    if (continuation_count != 0U && codepoint < minimum) return false;
    if (codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
        return false;
    }

    *io_offset = i;
    *out_codepoint = codepoint;
    return true;
}

static esp_err_t decode_utf8_resource(const gfxlink_resource_slot_t *resource,
                                      uint32_t **out_codepoints,
                                      size_t *out_count)
{
    if (!resource || !out_codepoints || !out_count ||
        resource->kind != GFXLINK_RESOURCE_TEXT ||
        !resource->committed || resource->total_size == 0U ||
        resource->data[resource->total_size - 1U] != '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t text_size = resource->total_size - 1U;
    if (memchr(resource->data, '\0', text_size) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0U;
    size_t count = 0U;
    while (offset < text_size) {
        uint32_t codepoint = 0U;
        if (!utf8_decode_one(resource->data, text_size, &offset, &codepoint)) {
            return ESP_ERR_INVALID_ARG;
        }
        count++;
    }

    uint32_t *codepoints = NULL;
    if (count > 0U) {
        if (count > SIZE_MAX / sizeof(*codepoints)) return ESP_ERR_INVALID_SIZE;
        codepoints = malloc(count * sizeof(*codepoints));
        if (!codepoints) return ESP_ERR_NO_MEM;

        offset = 0U;
        size_t index = 0U;
        while (offset < text_size) {
            if (!utf8_decode_one(resource->data, text_size,
                                 &offset, &codepoints[index++])) {
                free(codepoints);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    *out_codepoints = codepoints;
    *out_count = count;
    return ESP_OK;
}

static esp_err_t rasterize_text(grape_gfxlink_t *link,
                                const gfxlink_text_rasterize_request_t *request,
                                gfxlink_text_rasterize_response_t *out_result)
{
    if (!out_result) return ESP_ERR_INVALID_ARG;

    gfxlink_font_slot_t *font_slot =
        find_font_slot(link, from_le32(request->font_handle));
    gfxlink_resource_slot_t *text_resource =
        find_resource_slot(link, from_le32(request->text_resource_handle));
    if (!font_slot || !text_resource) return ESP_ERR_NOT_FOUND;

    uint32_t *codepoints = NULL;
    size_t codepoint_count = 0U;
    esp_err_t ret = decode_utf8_resource(
        text_resource, &codepoints, &codepoint_count);
    if (ret != ESP_OK) return ret;

    ret = ensure_glyph_cache(link);
    if (ret != ESP_OK) {
        free(codepoints);
        return ret;
    }

    grape_path_rasterize_config_t config = GRAPE_PATH_RASTERIZE_CONFIG_DEFAULT();
    config.pixels_per_unit = float_from_le_bits(request->pixels_per_unit_bits);
    config.samples_per_axis = request->samples_per_axis;
    config.padding_pixels = from_le32(request->padding_pixels);
    config.memory = GRAPE_MEMORY_DEFAULT;

    grape_text_raster_t raster = {0};
    ret = grape_text_rasterize_codepoints_a8(
        link->glyph_cache,
        font_slot->font,
        codepoints,
        codepoint_count,
        &config,
        &raster
    );
    free(codepoints);
    if (ret != ESP_OK) return ret;

    if (raster.glyph_count > UINT32_MAX) {
        grape_texture_destroy(raster.texture);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t texture_handle = 0U;
    ret = adopt_texture(link, raster.texture, &texture_handle);
    if (ret != ESP_OK) {
        grape_texture_destroy(raster.texture);
        return ret;
    }

    *out_result = (gfxlink_text_rasterize_response_t){
        .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
        .texture_handle = to_le32(texture_handle),
        .width = to_le32(grape_texture_width(raster.texture)),
        .height = to_le32(grape_texture_height(raster.texture)),
        .stride = to_le32((uint32_t)grape_texture_stride(raster.texture)),
        .origin_x_bits = float_to_le_bits(raster.text_origin_x),
        .origin_y_bits = float_to_le_bits(raster.text_origin_y),
        .pixels_per_unit_bits = float_to_le_bits(raster.pixels_per_unit),
        .advance_width_bits = float_to_le_bits(raster.advance_width),
        .glyph_count = to_le32((uint32_t)raster.glyph_count),
    };
    return ESP_OK;
}

static void execute_renderer_request(grape_gfxlink_t *link,
                                     const gfxlink_renderer_request_t *request,
                                     gfxlink_renderer_response_t *response)
{
    memset(response, 0, sizeof(*response));
    response->opcode = request->opcode;
    response->sequence = request->sequence;

    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;

    switch (request->opcode) {
        case GFXLINK_OP_PRESENT:
            if (request->payload_size != 0U) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            }
            ret = ESP_OK;
            for (uint32_t i = 0U; i < request->fast_damage_count; ++i) {
                const gfxlink_texture_damage_t *damage = &request->fast_damage[i];
                gfxlink_texture_slot_t *slot =
                    find_texture_slot(link, damage->texture_handle);
                if (!slot) {
                    ret = ESP_ERR_NOT_FOUND;
                    break;
                }
                ret = grape_texture_invalidate_rect(
                    slot->texture,
                    damage->x,
                    damage->y,
                    damage->width,
                    damage->height
                );
                if (ret != ESP_OK) {
                    break;
                }
            }
            if (ret == ESP_OK) {
                ret = grape_present(link->grape);
            }
            break;

        case GFXLINK_OP_CREATE_SOLID_SURFACE:
            if (request->payload_size != sizeof(gfxlink_create_solid_surface_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_create_solid_surface_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                uint32_t handle = 0;
                ret = create_solid_surface(link, &payload, &handle);
                if (ret == ESP_OK) {
                    gfxlink_create_surface_response_t result = {
                        .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
                        .handle = to_le32(handle),
                    };
                    memcpy(response->payload, &result, sizeof(result));
                    response->payload_size = sizeof(result);
                }
            }
            break;

        case GFXLINK_OP_SET_SURFACE_POSITION:
            if (request->payload_size != sizeof(gfxlink_set_surface_position_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_position_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_position(link, &payload);
            }
            break;


        case GFXLINK_OP_CREATE_SURFACE:
            if (request->payload_size != sizeof(gfxlink_create_surface_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_create_surface_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                uint32_t handle = 0U;
                ret = create_surface(link, &payload, &handle);
                if (ret == ESP_OK) {
                    gfxlink_create_surface_response_t result = {
                        .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
                        .handle = to_le32(handle),
                    };
                    memcpy(response->payload, &result, sizeof(result));
                    response->payload_size = sizeof(result);
                }
            }
            break;

        case GFXLINK_OP_SET_SURFACE_TEXTURE:
            if (request->payload_size != sizeof(gfxlink_set_surface_texture_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_texture_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_texture(link, &payload);
            }
            break;

        case GFXLINK_OP_SET_SURFACE_TRANSFORM:
            if (request->payload_size != sizeof(gfxlink_set_surface_transform_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_transform_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_transform(link, &payload);
            }
            break;

        case GFXLINK_OP_SET_SURFACE_SCALE:
            if (request->payload_size != sizeof(gfxlink_set_surface_scale_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_scale_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_scale(link, &payload);
            }
            break;

        case GFXLINK_OP_SET_SURFACE_ROTATION:
            if (request->payload_size != sizeof(gfxlink_set_surface_rotation_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_rotation_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_rotation(link, &payload);
            }
            break;

        case GFXLINK_OP_SET_SURFACE_ORIGIN:
            if (request->payload_size != sizeof(gfxlink_set_surface_origin_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_origin_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_origin(link, &payload);
            }
            break;

        case GFXLINK_OP_SET_SURFACE_Z:
            if (request->payload_size != sizeof(gfxlink_set_surface_z_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_z_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_z(link, &payload);
            }
            break;

        case GFXLINK_OP_SET_SURFACE_OPACITY:
            if (request->payload_size != sizeof(gfxlink_set_surface_opacity_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_opacity_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_opacity(link, &payload);
            }
            break;

        case GFXLINK_OP_SET_SURFACE_VISIBLE:
            if (request->payload_size != sizeof(gfxlink_set_surface_visible_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_visible_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_visible(link, &payload);
            }
            break;

        case GFXLINK_OP_TEXTURE_CREATE:
            if (request->payload_size != sizeof(gfxlink_texture_create_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_texture_create_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                uint32_t handle = 0U, stride = 0U, size = 0U;
                ret = create_texture(link, &payload, &handle, &stride, &size);
                if (ret == ESP_OK) {
                    gfxlink_texture_create_response_t result = {
                        .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
                        .handle = to_le32(handle),
                        .stride = to_le32(stride),
                        .size = to_le32(size),
                    };
                    memcpy(response->payload, &result, sizeof(result));
                    response->payload_size = sizeof(result);
                }
            }
            break;

        case GFXLINK_OP_TEXTURE_UPDATE:
            if (request->payload_size != sizeof(gfxlink_texture_update_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_texture_update_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = update_texture(link, &payload);
            }
            break;

        case GFXLINK_OP_TEXTURE_DESTROY:
            if (request->payload_size != sizeof(gfxlink_texture_handle_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_texture_handle_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = destroy_texture(link, &payload);
            }
            break;

        case GFXLINK_OP_PATH_CREATE:
            if (request->payload_size != sizeof(gfxlink_path_create_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_path_create_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                uint32_t handle = 0U;
                ret = create_path(link, &payload, &handle);
                if (ret == ESP_OK) {
                    gfxlink_create_surface_response_t result = {
                        .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
                        .handle = to_le32(handle),
                    };
                    memcpy(response->payload, &result, sizeof(result));
                    response->payload_size = sizeof(result);
                }
            }
            break;

        case GFXLINK_OP_PATH_RASTERIZE:
            if (request->payload_size != sizeof(gfxlink_path_rasterize_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_path_rasterize_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                gfxlink_raster_texture_response_t result;
                ret = rasterize_path(link, &payload, &result);
                if (ret == ESP_OK) {
                    memcpy(response->payload, &result, sizeof(result));
                    response->payload_size = sizeof(result);
                }
            }
            break;

        case GFXLINK_OP_PATH_DESTROY:
            if (request->payload_size != sizeof(gfxlink_path_handle_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_path_handle_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = destroy_path(link, &payload);
            }
            break;

        case GFXLINK_OP_SVG_CREATE:
            if (request->payload_size != sizeof(gfxlink_svg_create_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_svg_create_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                gfxlink_svg_create_response_t result;
                ret = create_svg(link, &payload, &result);
                if (ret == ESP_OK) {
                    memcpy(response->payload, &result, sizeof(result));
                    response->payload_size = sizeof(result);
                }
            }
            break;

        case GFXLINK_OP_SVG_DESTROY:
            if (request->payload_size != sizeof(gfxlink_svg_handle_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_svg_handle_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = destroy_svg(link, &payload);
            }
            break;

        case GFXLINK_OP_FONT_CREATE:
            if (request->payload_size != sizeof(gfxlink_font_create_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_font_create_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                gfxlink_font_create_response_t result;
                ret = create_font(link, &payload, &result);
                if (ret == ESP_OK) {
                    memcpy(response->payload, &result, sizeof(result));
                    response->payload_size = sizeof(result);
                }
            }
            break;

        case GFXLINK_OP_FONT_DESTROY:
            if (request->payload_size != sizeof(gfxlink_font_handle_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_font_handle_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = destroy_font(link, &payload);
            }
            break;

        case GFXLINK_OP_TEXT_RASTERIZE:
            if (request->payload_size != sizeof(gfxlink_text_rasterize_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_text_rasterize_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                gfxlink_text_rasterize_response_t result;
                ret = rasterize_text(link, &payload, &result);
                if (ret == ESP_OK) {
                    memcpy(response->payload, &result, sizeof(result));
                    response->payload_size = sizeof(result);
                }
            }
            break;

        case GFXLINK_OP_SET_SURFACE_COLOR:
            if (request->payload_size != sizeof(gfxlink_set_surface_color_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_set_surface_color_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = set_surface_color(link, &payload);
            }
            break;

        case GFXLINK_OP_DESTROY_SURFACE:
            if (request->payload_size != sizeof(gfxlink_destroy_surface_request_t)) {
                set_status_response(response, GFXLINK_STATUS_INVALID_ARGUMENT);
                return;
            } else {
                gfxlink_destroy_surface_request_t payload;
                memcpy(&payload, request->payload, sizeof(payload));
                ret = destroy_surface(link, &payload);
            }
            break;

        default:
            set_status_response(response, GFXLINK_STATUS_UNSUPPORTED);
            return;
    }

    if (response->payload_size == 0U || ret != ESP_OK) {
        set_status_response(response, status_from_esp_err(ret));
    }
}

static esp_err_t wait_vendor_tx_available(uint32_t *out_available)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(GFXLINK_TX_WAIT_MS);
    while (1) {
        uint32_t available = tud_vendor_write_available();
        if (available > 0U) {
            *out_available = available;
            return ESP_OK;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

static esp_err_t send_bytes(const uint8_t *data, uint32_t size)
{
    uint32_t offset = 0U;
    while (offset < size) {
        uint32_t available = 0U;
        esp_err_t ret = wait_vendor_tx_available(&available);
        if (ret != ESP_OK) {
            return ret;
        }

        uint32_t remaining = size - offset;
        uint32_t chunk = remaining < available ? remaining : available;
        uint32_t written = tud_vendor_write(data + offset, chunk);
        if (written != chunk) {
            return ESP_FAIL;
        }
        offset += written;
    }
    return ESP_OK;
}

static esp_err_t send_response(uint8_t opcode,
                               uint32_t sequence,
                               const void *payload,
                               uint32_t payload_size)
{
    if (payload_size > GFXLINK_MAX_PAYLOAD || (payload_size > 0U && !payload)) {
        return ESP_ERR_INVALID_ARG;
    }

    gfxlink_header_t header = {
        .magic = to_le32(GFXLINK_MAGIC),
        .version = GFXLINK_PROTOCOL_VERSION,
        .opcode = opcode,
        .flags = to_le16(GFXLINK_FLAG_RESPONSE),
        .sequence = to_le32(sequence),
        .payload_size = to_le32(payload_size),
    };

    esp_err_t ret = send_bytes((const uint8_t *)&header, sizeof(header));
    if (ret != ESP_OK) {
        return ret;
    }
    if (payload_size > 0U) {
        ret = send_bytes(payload, payload_size);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    tud_vendor_write_flush();
    return ESP_OK;
}

static void send_status(uint8_t opcode, uint32_t sequence, gfxlink_status_t status)
{
    gfxlink_status_response_t response = {
        .status = (int32_t)to_le32((uint32_t)(int32_t)status),
    };
    esp_err_t ret = send_response(opcode, sequence, &response, sizeof(response));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send status response: %s", esp_err_to_name(ret));
    }
}

static gfxlink_status_t resource_create(grape_gfxlink_t *link,
                                        const gfxlink_resource_create_request_t *request,
                                        uint32_t *out_handle,
                                        uint32_t *out_chunk_count)
{
    uint32_t kind = from_le32(request->kind);
    uint32_t total_size = from_le32(request->total_size);
    uint32_t flags = from_le32(request->flags);

    if (!out_handle || !out_chunk_count || flags != 0U ||
        kind > GFXLINK_RESOURCE_TEXT ||
        total_size == 0U || total_size > GFXLINK_MAX_RESOURCE_SIZE) {
        return GFXLINK_STATUS_INVALID_ARGUMENT;
    }

    gfxlink_resource_slot_t *slot = allocate_resource_slot(link);
    uint32_t handle = allocate_handle(link);
    if (!slot || handle == 0U) {
        return GFXLINK_STATUS_NO_MEMORY;
    }

    uint8_t *data = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) {
        data = heap_caps_malloc(total_size, MALLOC_CAP_8BIT);
    }
    if (!data) {
        return GFXLINK_STATUS_NO_MEMORY;
    }

    memset(slot, 0, sizeof(*slot));
    slot->handle = handle;
    slot->kind = kind;
    slot->total_size = total_size;
    slot->chunk_count = resource_chunk_count(total_size);
    slot->data = data;
    *out_handle = handle;
    *out_chunk_count = slot->chunk_count;
    return GFXLINK_STATUS_OK;
}

static void resource_write(grape_gfxlink_t *link,
                           const uint8_t *payload,
                           uint32_t payload_size)
{
    if (!payload || payload_size <= sizeof(gfxlink_resource_write_request_t)) {
        return;
    }

    gfxlink_resource_write_request_t request;
    memcpy(&request, payload, sizeof(request));
    uint32_t handle = from_le32(request.handle);
    uint32_t chunk_index = from_le32(request.chunk_index);
    uint32_t data_size = from_le32(request.data_size);
    uint32_t expected_crc32 = from_le32(request.crc32);

    gfxlink_resource_slot_t *slot = find_resource_slot(link, handle);
    if (!slot || slot->committed || chunk_index >= slot->chunk_count) {
        return;
    }

    uint32_t expected_size = resource_chunk_size(slot, chunk_index);
    uint32_t wire_size = payload_size - sizeof(request);
    if (data_size != wire_size || data_size != expected_size) {
        bitmap_clear(slot->received_bitmap, chunk_index);
        bitmap_set(slot->corrupt_bitmap, chunk_index);
        return;
    }

    const uint8_t *data = payload + sizeof(request);
    uint32_t actual_crc32 = crc32_ieee(data, data_size);
    if (actual_crc32 != expected_crc32) {
        bitmap_clear(slot->received_bitmap, chunk_index);
        bitmap_set(slot->corrupt_bitmap, chunk_index);
        return;
    }

    uint32_t offset = chunk_index * GFXLINK_RESOURCE_CHUNK_SIZE;
    memcpy(slot->data + offset, data, data_size);
    bitmap_set(slot->received_bitmap, chunk_index);
    bitmap_clear(slot->corrupt_bitmap, chunk_index);
}

static void resource_commit(grape_gfxlink_t *link,
                            const gfxlink_resource_commit_request_t *request,
                            gfxlink_resource_commit_response_t *response)
{
    memset(response, 0, sizeof(*response));
    uint32_t handle = from_le32(request->handle);
    uint32_t expected_crc32 = from_le32(request->expected_crc32);
    gfxlink_resource_slot_t *slot = find_resource_slot(link, handle);
    if (!slot) {
        response->status = (int32_t)to_le32((uint32_t)(int32_t)GFXLINK_STATUS_NOT_FOUND);
        return;
    }

    response->chunk_count = to_le32(slot->chunk_count);
    if (slot->committed) {
        response->status = (int32_t)to_le32((uint32_t)(int32_t)GFXLINK_STATUS_OK);
        response->resource_crc32 = to_le32(slot->committed_crc32);
        return;
    }

    bool incomplete = false;
    for (uint32_t i = 0U; i < slot->chunk_count; ++i) {
        if (bitmap_test(slot->corrupt_bitmap, i)) {
            bitmap_set(response->corrupt_bitmap, i);
            incomplete = true;
        } else if (!bitmap_test(slot->received_bitmap, i)) {
            bitmap_set(response->missing_bitmap, i);
            incomplete = true;
        }
    }

    if (incomplete) {
        response->status = (int32_t)to_le32((uint32_t)(int32_t)GFXLINK_STATUS_INCOMPLETE);
        return;
    }

    uint32_t actual_crc32 = crc32_ieee(slot->data, slot->total_size);
    response->resource_crc32 = to_le32(actual_crc32);
    if (actual_crc32 != expected_crc32) {
        response->status = (int32_t)to_le32((uint32_t)(int32_t)GFXLINK_STATUS_CHECKSUM_MISMATCH);
        return;
    }

    slot->committed = true;
    slot->committed_crc32 = actual_crc32;
    response->status = (int32_t)to_le32((uint32_t)(int32_t)GFXLINK_STATUS_OK);
}

static gfxlink_status_t resource_destroy(grape_gfxlink_t *link, uint32_t handle)
{
    gfxlink_resource_slot_t *slot = find_resource_slot(link, handle);
    if (!slot) {
        return GFXLINK_STATUS_NOT_FOUND;
    }

    free(slot->data);
    memset(slot, 0, sizeof(*slot));
    return GFXLINK_STATUS_OK;
}

static bool is_renderer_opcode(uint8_t opcode)
{
    switch (opcode) {
        case GFXLINK_OP_PRESENT:
        case GFXLINK_OP_CREATE_SOLID_SURFACE:
        case GFXLINK_OP_SET_SURFACE_POSITION:
        case GFXLINK_OP_SET_SURFACE_COLOR:
        case GFXLINK_OP_DESTROY_SURFACE:
        case GFXLINK_OP_CREATE_SURFACE:
        case GFXLINK_OP_SET_SURFACE_TEXTURE:
        case GFXLINK_OP_SET_SURFACE_TRANSFORM:
        case GFXLINK_OP_SET_SURFACE_SCALE:
        case GFXLINK_OP_SET_SURFACE_ROTATION:
        case GFXLINK_OP_SET_SURFACE_ORIGIN:
        case GFXLINK_OP_SET_SURFACE_Z:
        case GFXLINK_OP_SET_SURFACE_OPACITY:
        case GFXLINK_OP_SET_SURFACE_VISIBLE:
        case GFXLINK_OP_TEXTURE_CREATE:
        case GFXLINK_OP_TEXTURE_UPDATE:
        case GFXLINK_OP_TEXTURE_DESTROY:
        case GFXLINK_OP_PATH_CREATE:
        case GFXLINK_OP_PATH_RASTERIZE:
        case GFXLINK_OP_PATH_DESTROY:
        case GFXLINK_OP_SVG_CREATE:
        case GFXLINK_OP_SVG_DESTROY:
        case GFXLINK_OP_FONT_CREATE:
        case GFXLINK_OP_FONT_DESTROY:
        case GFXLINK_OP_TEXT_RASTERIZE:
            return true;
        default:
            return false;
    }
}

static void dispatch_packet(grape_gfxlink_t *link,
                            const gfxlink_header_t *header,
                            const uint8_t *payload)
{
    uint32_t sequence = from_le32(header->sequence);
    uint32_t payload_size = from_le32(header->payload_size);

    if (header->opcode == GFXLINK_OP_HELLO) {
        if (payload_size != 0U) {
            send_status(header->opcode, sequence, GFXLINK_STATUS_INVALID_ARGUMENT);
            return;
        }
        gfxlink_hello_response_t response = {
            .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
            .protocol_version = GFXLINK_PROTOCOL_VERSION,
            .reserved = {0, 0, 0},
            .capabilities = to_le32(
                GFXLINK_CAP_SOLID_SURFACE |
                GFXLINK_CAP_SURFACE_POSITION |
                GFXLINK_CAP_SURFACE_COLOR |
                GFXLINK_CAP_SURFACE_DESTROY |
                GFXLINK_CAP_EXPLICIT_PRESENT |
                GFXLINK_CAP_RESOURCE_STREAM |
                GFXLINK_CAP_RELIABLE_RESOURCE_STREAM |
                GFXLINK_CAP_TEXTURES |
                GFXLINK_CAP_TEXTURE_UPDATE |
                GFXLINK_CAP_SURFACE_TEXTURE |
                GFXLINK_CAP_SURFACE_FULL_CONTROL |
                GFXLINK_CAP_VECTOR_PATHS |
                GFXLINK_CAP_SVG |
                GFXLINK_CAP_FONTS |
                GFXLINK_CAP_TEXT |
                GFXLINK_CAP_TEXTURE_WRITE_RECT
            ),
            .max_payload = to_le32(GFXLINK_MAX_PAYLOAD),
            .max_resource_size = to_le32(GFXLINK_MAX_RESOURCE_SIZE),
        };
        if (send_response(header->opcode, sequence, &response, sizeof(response)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send HELLO response");
        }
        return;
    }

    if (header->opcode == GFXLINK_OP_GET_INFO) {
        if (payload_size != 0U) {
            send_status(header->opcode, sequence, GFXLINK_STATUS_INVALID_ARGUMENT);
            return;
        }
        const grape_display_info_t *display = grape_get_display_info(link->grape);
        if (!display) {
            send_status(header->opcode, sequence, GFXLINK_STATUS_INTERNAL);
            return;
        }
        gfxlink_info_response_t response = {
            .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
            .display_width = to_le32(display->width),
            .display_height = to_le32(display->height),
            .pixel_format = to_le32((uint32_t)display->format),
            .max_surfaces = to_le32(GFXLINK_MAX_SURFACES),
            .max_resources = to_le32(GFXLINK_MAX_RESOURCES),
            .max_textures = to_le32(GFXLINK_MAX_TEXTURES),
            .max_paths = to_le32(GFXLINK_MAX_PATHS),
            .max_svg_documents = to_le32(GFXLINK_MAX_SVG_DOCUMENTS),
            .max_fonts = to_le32(GFXLINK_MAX_FONTS),
        };
        if (send_response(header->opcode, sequence, &response, sizeof(response)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send INFO response");
        }
        return;
    }

    if (header->opcode == GFXLINK_OP_RESOURCE_CREATE) {
        if (payload_size != sizeof(gfxlink_resource_create_request_t)) {
            send_status(header->opcode, sequence, GFXLINK_STATUS_INVALID_ARGUMENT);
            return;
        }
        gfxlink_resource_create_request_t request;
        memcpy(&request, payload, sizeof(request));
        uint32_t handle = 0U;
        uint32_t chunk_count = 0U;
        gfxlink_status_t status = resource_create(link, &request, &handle, &chunk_count);
        if (status != GFXLINK_STATUS_OK) {
            send_status(header->opcode, sequence, status);
            return;
        }
        gfxlink_resource_create_response_t response = {
            .status = (int32_t)to_le32((uint32_t)GFXLINK_STATUS_OK),
            .handle = to_le32(handle),
            .chunk_size = to_le32(GFXLINK_RESOURCE_CHUNK_SIZE),
            .chunk_count = to_le32(chunk_count),
        };
        if (send_response(header->opcode, sequence, &response, sizeof(response)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send resource create response");
        }
        return;
    }

    if (header->opcode == GFXLINK_OP_RESOURCE_WRITE) {
        if ((from_le16(header->flags) & GFXLINK_FLAG_NO_RESPONSE) != 0U) {
            resource_write(link, payload, payload_size);
        }
        return;
    }

    if (header->opcode == GFXLINK_OP_RESOURCE_COMMIT) {
        if (payload_size != sizeof(gfxlink_resource_commit_request_t)) {
            send_status(header->opcode, sequence, GFXLINK_STATUS_INVALID_ARGUMENT);
            return;
        }
        gfxlink_resource_commit_request_t request;
        memcpy(&request, payload, sizeof(request));
        gfxlink_resource_commit_response_t response;
        resource_commit(link, &request, &response);
        if (send_response(header->opcode, sequence, &response, sizeof(response)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send resource commit response");
        }
        return;
    }

    if (header->opcode == GFXLINK_OP_RESOURCE_DESTROY) {
        if (payload_size != sizeof(gfxlink_resource_handle_request_t)) {
            send_status(header->opcode, sequence, GFXLINK_STATUS_INVALID_ARGUMENT);
            return;
        }
        gfxlink_resource_handle_request_t request;
        memcpy(&request, payload, sizeof(request));
        send_status(header->opcode, sequence,
                    resource_destroy(link, from_le32(request.handle)));
        return;
    }

    if (header->opcode == GFXLINK_OP_TEXTURE_WRITE_RECT) {
        gfxlink_status_t status = link->fast_write_status;
        if (status == GFXLINK_STATUS_OK) {
            status = write_texture_rect(link, payload, payload_size);
        }

        if ((from_le16(header->flags) & GFXLINK_FLAG_NO_RESPONSE) != 0U) {
            if (status != GFXLINK_STATUS_OK &&
                link->fast_write_status == GFXLINK_STATUS_OK) {
                link->fast_write_status = status;
            }
        } else {
            send_status(header->opcode, sequence, status);
        }
        return;
    }

    if (header->opcode == GFXLINK_OP_PRESENT &&
        link->fast_write_status != GFXLINK_STATUS_OK) {
        gfxlink_status_t status = link->fast_write_status;
        link->fast_write_status = GFXLINK_STATUS_OK;
        link->fast_damage_count = 0U;
        send_status(header->opcode, sequence, status);
        return;
    }

    if (!is_renderer_opcode(header->opcode)) {
        send_status(header->opcode, sequence, GFXLINK_STATUS_UNSUPPORTED);
        return;
    }
    if (payload_size > GFXLINK_RENDERER_MAX_PAYLOAD) {
        send_status(header->opcode, sequence, GFXLINK_STATUS_INVALID_ARGUMENT);
        return;
    }

    gfxlink_renderer_request_t request = {
        .opcode = header->opcode,
        .sequence = sequence,
        .payload_size = payload_size,
    };
    if (payload_size > 0U) {
        memcpy(request.payload, payload, payload_size);
    }
    if (header->opcode == GFXLINK_OP_PRESENT && link->fast_damage_count > 0U) {
        request.fast_damage_count = (uint8_t)link->fast_damage_count;
        memcpy(request.fast_damage,
               link->fast_damage,
               link->fast_damage_count * sizeof(link->fast_damage[0]));
        link->fast_damage_count = 0U;
    }

    if (xQueueSend(link->renderer_requests, &request, pdMS_TO_TICKS(250)) != pdTRUE) {
        send_status(header->opcode, sequence, GFXLINK_STATUS_BUSY);
        return;
    }

    gfxlink_renderer_response_t response;
    if (xQueueReceive(link->renderer_responses, &response, pdMS_TO_TICKS(5000)) != pdTRUE) {
        send_status(header->opcode, sequence, GFXLINK_STATUS_INTERNAL);
        return;
    }
    if (response.opcode != header->opcode || response.sequence != sequence) {
        send_status(header->opcode, sequence, GFXLINK_STATUS_INTERNAL);
        return;
    }

    if (send_response(response.opcode, response.sequence,
                      response.payload, response.payload_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send renderer response");
    }
}

static bool valid_header(const gfxlink_header_t *header)
{
    uint16_t flags = from_le16(header->flags);
    return from_le32(header->magic) == GFXLINK_MAGIC &&
           header->version == GFXLINK_PROTOCOL_VERSION &&
           (flags & GFXLINK_FLAG_RESPONSE) == 0U &&
           (flags & ~(GFXLINK_FLAG_NO_RESPONSE)) == 0U &&
           from_le32(header->payload_size) <= GFXLINK_MAX_PAYLOAD;
}

static void gfxlink_task(void *arg)
{
    grape_gfxlink_t *link = arg;
    const size_t frame_capacity = sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD;
    uint8_t *frame = heap_caps_malloc(frame_capacity, MALLOC_CAP_8BIT);
    if (!frame) {
        ESP_LOGE(TAG, "Unable to allocate GFXLINK frame buffer");
        vTaskDelete(NULL);
        return;
    }

    size_t frame_size = 0U;
    size_t expected_size = 0U;

    while (1) {
        while (tud_vendor_available() > 0U) {
            size_t target = expected_size ? expected_size : sizeof(gfxlink_header_t);
            size_t remaining = target - frame_size;
            uint32_t available = tud_vendor_available();
            uint32_t to_read = remaining < available ? (uint32_t)remaining : available;
            uint32_t received = tud_vendor_read(frame + frame_size, to_read);
            if (received == 0U) {
                break;
            }
            frame_size += received;

            if (frame_size == sizeof(gfxlink_header_t) && expected_size == 0U) {
                gfxlink_header_t header;
                memcpy(&header, frame, sizeof(header));
                if (!valid_header(&header)) {
                    memmove(frame, frame + 1U, sizeof(gfxlink_header_t) - 1U);
                    frame_size = sizeof(gfxlink_header_t) - 1U;
                    continue;
                }
                expected_size = sizeof(gfxlink_header_t) + from_le32(header.payload_size);
            }

            if (expected_size != 0U && frame_size == expected_size) {
                gfxlink_header_t header;
                memcpy(&header, frame, sizeof(header));
                dispatch_packet(link, &header, frame + sizeof(header));
                frame_size = 0U;
                expected_size = 0U;
            }
        }

        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    (void)buffer;
    (void)bufsize;

    if (itf != GFXLINK_USB_INTERFACE || !s_active_link || !s_active_link->task) {
        return;
    }

    xTaskNotifyGive(s_active_link->task);
}

esp_err_t grape_gfxlink_start(grape_context_t *grape, grape_gfxlink_t **out_link)
{
    if (!grape || !out_link || s_active_link) {
        return ESP_ERR_INVALID_ARG;
    }

    grape_gfxlink_t *link = calloc(1, sizeof(*link));
    if (!link) {
        return ESP_ERR_NO_MEM;
    }
    link->grape = grape;
    link->next_handle = 1U;

    link->renderer_requests = xQueueCreate(GFXLINK_QUEUE_DEPTH, sizeof(gfxlink_renderer_request_t));
    link->renderer_responses = xQueueCreate(GFXLINK_QUEUE_DEPTH, sizeof(gfxlink_renderer_response_t));
    if (!link->renderer_requests || !link->renderer_responses) {
        if (link->renderer_requests) vQueueDelete(link->renderer_requests);
        if (link->renderer_responses) vQueueDelete(link->renderer_responses);
        free(link);
        return ESP_ERR_NO_MEM;
    }

    s_active_link = link;

    tinyusb_config_t usb_config = TINYUSB_DEFAULT_CONFIG();
    usb_config.port = TINYUSB_PORT_HIGH_SPEED_0;
    usb_config.task = TINYUSB_TASK_CUSTOM(
        GFXLINK_TINYUSB_TASK_STACK_SIZE,
        GFXLINK_TINYUSB_TASK_PRIORITY,
        GFXLINK_CPU_CORE
    );
    usb_config.descriptor.device = &s_device_descriptor;
    usb_config.descriptor.qualifier = &s_device_qualifier;
    usb_config.descriptor.string = s_string_descriptors;
    usb_config.descriptor.string_count = sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0]);
    usb_config.descriptor.full_speed_config = s_fs_configuration_descriptor;
    usb_config.descriptor.high_speed_config = s_hs_configuration_descriptor;

    esp_err_t ret = tinyusb_driver_install(&usb_config);
    if (ret != ESP_OK) {
        s_active_link = NULL;
        vQueueDelete(link->renderer_requests);
        vQueueDelete(link->renderer_responses);
        free(link);
        return ret;
    }

    BaseType_t created = xTaskCreatePinnedToCore(
        gfxlink_task,
        "gfxlink",
        GFXLINK_TASK_STACK_SIZE,
        link,
        GFXLINK_TASK_PRIORITY,
        &link->task,
        GFXLINK_CPU_CORE
    );
    if (created != pdPASS) {
        tinyusb_driver_uninstall();
        s_active_link = NULL;
        vQueueDelete(link->renderer_requests);
        vQueueDelete(link->renderer_responses);
        free(link);
        return ESP_ERR_NO_MEM;
    }

    *out_link = link;
    ESP_LOGI(TAG,
             "GFXLINK v%u started on USB HS; max payload=%u, resources=%u, fast texture writes enabled",
             GFXLINK_PROTOCOL_VERSION,
             GFXLINK_MAX_PAYLOAD,
             GFXLINK_MAX_RESOURCES);
    return ESP_OK;
}

esp_err_t grape_gfxlink_process(grape_gfxlink_t *link, TickType_t timeout_ticks)
{
    if (!link) {
        return ESP_ERR_INVALID_ARG;
    }

    gfxlink_renderer_request_t request;
    if (xQueueReceive(link->renderer_requests, &request, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    gfxlink_renderer_response_t response;
    execute_renderer_request(link, &request, &response);
    if (xQueueSend(link->renderer_responses, &response, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
