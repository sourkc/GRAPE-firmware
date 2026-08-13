#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#include "grape/gfxlink_protocol.h"
#include "grape/grape_gfxlink.h"

#define GFXLINK_MAX_SURFACES 64U
#define GFXLINK_RX_STREAM_SIZE 1024U
#define GFXLINK_QUEUE_DEPTH 4U
#define GFXLINK_TASK_STACK_SIZE 4096U
#define GFXLINK_TASK_PRIORITY 4U
#define GFXLINK_TINYUSB_TASK_STACK_SIZE 4096U
#define GFXLINK_TINYUSB_TASK_PRIORITY 5U
#define GFXLINK_CPU_CORE 1
#define GFXLINK_USB_PACKET_SIZE 64U
#define GFXLINK_CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const char *TAG = "grape_gfxlink";

typedef struct {
    grape_surface_t *surface;
    grape_texture_t *texture;
} gfxlink_surface_slot_t;

typedef struct {
    uint8_t opcode;
    uint32_t sequence;
    uint32_t payload_size;
    uint8_t payload[GFXLINK_MAX_PAYLOAD];
} gfxlink_renderer_request_t;

typedef struct {
    uint8_t opcode;
    uint32_t sequence;
    uint32_t payload_size;
    uint8_t payload[GFXLINK_MAX_PAYLOAD];
} gfxlink_renderer_response_t;

struct grape_gfxlink {
    grape_context_t *grape;
    StreamBufferHandle_t rx_stream;
    QueueHandle_t renderer_requests;
    QueueHandle_t renderer_responses;
    TaskHandle_t task;
    volatile bool rx_overflow;
    gfxlink_surface_slot_t surfaces[GFXLINK_MAX_SURFACES];
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
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
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
        GFXLINK_USB_PACKET_SIZE
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

static gfxlink_surface_slot_t *find_surface_slot(grape_gfxlink_t *link, uint32_t handle)
{
    if (handle == 0U || handle > GFXLINK_MAX_SURFACES) {
        return NULL;
    }
    gfxlink_surface_slot_t *slot = &link->surfaces[handle - 1U];
    return slot->surface ? slot : NULL;
}

static uint32_t allocate_surface_slot(grape_gfxlink_t *link)
{
    for (uint32_t i = 0; i < GFXLINK_MAX_SURFACES; ++i) {
        if (!link->surfaces[i].surface && !link->surfaces[i].texture) {
            return i + 1U;
        }
    }
    return 0U;
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

    uint32_t handle = allocate_surface_slot(link);
    if (handle == 0U) {
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

    gfxlink_surface_slot_t *slot = &link->surfaces[handle - 1U];
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
    bool needs_present = false;

    switch (request->opcode) {
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
                    needs_present = true;
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
                needs_present = ret == ESP_OK;
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
                needs_present = ret == ESP_OK;
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
                needs_present = ret == ESP_OK;
            }
            break;

        default:
            set_status_response(response, GFXLINK_STATUS_UNSUPPORTED);
            return;
    }

    if (ret == ESP_OK && needs_present) {
        ret = grape_present(link->grape);
    }

    if (response->payload_size == 0U || ret != ESP_OK) {
        set_status_response(response, status_from_esp_err(ret));
    }
}

static esp_err_t send_response(uint8_t opcode,
                               uint32_t sequence,
                               const void *payload,
                               uint32_t payload_size)
{
    if (payload_size > GFXLINK_MAX_PAYLOAD || (payload_size > 0U && !payload)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t packet[sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD];
    gfxlink_header_t header = {
        .magic = to_le32(GFXLINK_MAGIC),
        .version = GFXLINK_PROTOCOL_VERSION,
        .opcode = opcode,
        .flags = to_le16(GFXLINK_FLAG_RESPONSE),
        .sequence = to_le32(sequence),
        .payload_size = to_le32(payload_size),
    };

    memcpy(packet, &header, sizeof(header));
    if (payload_size > 0U) {
        memcpy(packet + sizeof(header), payload, payload_size);
    }

    uint32_t total = sizeof(header) + payload_size;
    uint32_t written = tud_vendor_write(packet, total);
    if (written != total) {
        return ESP_ERR_NO_MEM;
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
                GFXLINK_CAP_SURFACE_DESTROY
            ),
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
        };
        if (send_response(header->opcode, sequence, &response, sizeof(response)) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to send INFO response");
        }
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
    return from_le32(header->magic) == GFXLINK_MAGIC &&
           header->version == GFXLINK_PROTOCOL_VERSION &&
           (from_le16(header->flags) & GFXLINK_FLAG_RESPONSE) == 0U &&
           from_le32(header->payload_size) <= GFXLINK_MAX_PAYLOAD;
}

static void gfxlink_task(void *arg)
{
    grape_gfxlink_t *link = arg;
    uint8_t frame[sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD];
    size_t frame_size = 0U;
    size_t expected_size = 0U;

    while (1) {
        if (link->rx_overflow) {
            link->rx_overflow = false;
            frame_size = 0U;
            expected_size = 0U;
            xStreamBufferReset(link->rx_stream);
            ESP_LOGW(TAG, "RX stream overflow; parser reset");
        }

        uint8_t chunk[128];
        size_t received = xStreamBufferReceive(
            link->rx_stream,
            chunk,
            sizeof(chunk),
            portMAX_DELAY
        );
        size_t offset = 0U;

        while (offset < received) {
            size_t target = expected_size ? expected_size : sizeof(gfxlink_header_t);
            size_t remaining = target - frame_size;
            size_t available = received - offset;
            size_t copy = remaining < available ? remaining : available;
            memcpy(frame + frame_size, chunk + offset, copy);
            frame_size += copy;
            offset += copy;

            if (frame_size == sizeof(gfxlink_header_t) && expected_size == 0U) {
                gfxlink_header_t header;
                memcpy(&header, frame, sizeof(header));
                if (!valid_header(&header)) {
                    ESP_LOGW(TAG, "Invalid GFXLINK header; parser reset");
                    frame_size = 0U;
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
    }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t bufsize)
{
    if (itf != GFXLINK_USB_INTERFACE || !s_active_link || !buffer || bufsize == 0U) {
        return;
    }

    size_t sent = xStreamBufferSend(s_active_link->rx_stream, buffer, bufsize, 0);
    if (sent != bufsize) {
        s_active_link->rx_overflow = true;
    }
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

    link->rx_stream = xStreamBufferCreate(GFXLINK_RX_STREAM_SIZE, 1);
    link->renderer_requests = xQueueCreate(GFXLINK_QUEUE_DEPTH, sizeof(gfxlink_renderer_request_t));
    link->renderer_responses = xQueueCreate(GFXLINK_QUEUE_DEPTH, sizeof(gfxlink_renderer_response_t));
    if (!link->rx_stream || !link->renderer_requests || !link->renderer_responses) {
        if (link->rx_stream) vStreamBufferDelete(link->rx_stream);
        if (link->renderer_requests) vQueueDelete(link->renderer_requests);
        if (link->renderer_responses) vQueueDelete(link->renderer_responses);
        free(link);
        return ESP_ERR_NO_MEM;
    }

    s_active_link = link;

    tinyusb_config_t usb_config = TINYUSB_CONFIG_FULL_SPEED(NULL, NULL);
    usb_config.task = TINYUSB_TASK_CUSTOM(
        GFXLINK_TINYUSB_TASK_STACK_SIZE,
        GFXLINK_TINYUSB_TASK_PRIORITY,
        GFXLINK_CPU_CORE
    );
    usb_config.descriptor.device = &s_device_descriptor;
    usb_config.descriptor.string = s_string_descriptors;
    usb_config.descriptor.string_count = sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0]);
    usb_config.descriptor.full_speed_config = s_fs_configuration_descriptor;

    esp_err_t ret = tinyusb_driver_install(&usb_config);
    if (ret != ESP_OK) {
        s_active_link = NULL;
        vStreamBufferDelete(link->rx_stream);
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
        vStreamBufferDelete(link->rx_stream);
        vQueueDelete(link->renderer_requests);
        vQueueDelete(link->renderer_responses);
        free(link);
        return ESP_ERR_NO_MEM;
    }

    *out_link = link;
    ESP_LOGI(TAG, "GFXLINK v%u started on USB FS; TinyUSB and GFXLINK pinned to CPU%d",
             GFXLINK_PROTOCOL_VERSION, GFXLINK_CPU_CORE);
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
