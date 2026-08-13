#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "grape/grape_gfxlink_internal.h"
#include "grape/grape_gfxlink_worker.h"
#include "tusb.h"

static bool is_render_opcode(uint8_t opcode)
{
    return opcode == GFXLINK_OP_CREATE_SOLID_SURFACE ||
           opcode == GFXLINK_OP_SET_SURFACE_POSITION ||
           opcode == GFXLINK_OP_SET_SURFACE_COLOR ||
           opcode == GFXLINK_OP_DESTROY_SURFACE;
}

static void build_hello(gfxlink_packet_t *packet)
{
    gfxlink_hello_response_t response = {
        .status = GFXLINK_STATUS_OK,
        .protocol_version = GFXLINK_PROTOCOL_VERSION,
        .capabilities = GFXLINK_CAP_SOLID_SURFACE |
                        GFXLINK_CAP_SURFACE_POSITION |
                        GFXLINK_CAP_SURFACE_COLOR |
                        GFXLINK_CAP_SURFACE_DESTROY,
    };
    memcpy(packet->response, &response, sizeof(response));
    packet->response_size = sizeof(response);
}

static void build_info(gfxlink_packet_t *packet)
{
    gfxlink_info_response_t response = {
        .status = GFXLINK_STATUS_OK,
        .display_width = g_gfxlink_display_width,
        .display_height = g_gfxlink_display_height,
        .pixel_format = g_gfxlink_display_format,
        .max_surfaces = GFXLINK_MAX_SURFACES,
    };
    memcpy(packet->response, &response, sizeof(response));
    packet->response_size = sizeof(response);
}

static void send_response(gfxlink_packet_t *packet)
{
    uint8_t frame[sizeof(gfxlink_header_t) + 64];
    gfxlink_header_t header = {
        .magic = GFXLINK_MAGIC,
        .version = GFXLINK_PROTOCOL_VERSION,
        .opcode = packet->header.opcode,
        .flags = GFXLINK_FLAG_RESPONSE,
        .sequence = packet->header.sequence,
        .payload_size = packet->response_size,
    };
    size_t frame_size = sizeof(header) + packet->response_size;
    memcpy(frame, &header, sizeof(header));
    if (packet->response_size) memcpy(frame + sizeof(header), packet->response, packet->response_size);

    if (!tud_vendor_mounted()) return;
    size_t offset = 0;
    TickType_t started = xTaskGetTickCount();
    while (offset < frame_size && xTaskGetTickCount() - started < pdMS_TO_TICKS(500)) {
        uint32_t written = tud_vendor_write(frame + offset, frame_size - offset);
        if (!written) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        offset += written;
    }
    tud_vendor_write_flush();
}

void gfxlink_worker(void *arg)
{
    (void)arg;
    while (true) {
        gfxlink_packet_t *packet = NULL;
        if (xQueueReceive(g_gfxlink_dispatch_queue, &packet, portMAX_DELAY) != pdTRUE || !packet) continue;

        if (packet->header.flags != 0u) {
            gfxlink_set_status(packet, GFXLINK_STATUS_INVALID_PACKET);
        } else if (packet->header.opcode == GFXLINK_OP_HELLO) {
            if (packet->header.payload_size == 0u) build_hello(packet);
            else gfxlink_set_status(packet, GFXLINK_STATUS_INVALID_ARGUMENT);
        } else if (packet->header.opcode == GFXLINK_OP_GET_INFO) {
            if (packet->header.payload_size == 0u) build_info(packet);
            else gfxlink_set_status(packet, GFXLINK_STATUS_INVALID_ARGUMENT);
        } else if (is_render_opcode(packet->header.opcode)) {
            packet->waiter = xTaskGetCurrentTaskHandle();
            if (xQueueSend(g_gfxlink_render_queue, &packet, pdMS_TO_TICKS(100)) != pdTRUE) {
                gfxlink_set_status(packet, GFXLINK_STATUS_BUSY);
            } else {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
        } else {
            gfxlink_set_status(packet, GFXLINK_STATUS_UNSUPPORTED);
        }

        send_response(packet);
        free(packet);
    }
}
