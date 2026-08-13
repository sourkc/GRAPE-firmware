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

static const char *TAG = "grape_gfxlink";

typedef struct {
    uint32_t handle;
    grape_surface_t *surface;
    grape_texture_t *texture;
} gfxlink_surface_slot_t;

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
    uint8_t opcode;
    uint32_t sequence;
    uint32_t payload_size;
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
    "GFXLINK-M1",
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
            !link->surfaces[i].texture) {
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
    slot->texture = texture;
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

    ret = grape_texture_destroy(slot->texture);
    if (ret != ESP_OK) {
        return ret;
    }
    slot->texture = NULL;
    slot->handle = 0U;
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
            ret = grape_present(link->grape);
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
        kind > GFXLINK_RESOURCE_SVG ||
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
                GFXLINK_CAP_RELIABLE_RESOURCE_STREAM
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
             "GFXLINK v%u M2.0a started on USB HS; max payload=%u, resources=%u",
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
