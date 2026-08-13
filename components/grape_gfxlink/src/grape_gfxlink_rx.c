#include <stdlib.h>
#include <string.h>
#include "grape/grape_gfxlink_internal.h"
#include "grape/grape_gfxlink_worker.h"
#include "tusb.h"

#define RX_CAPACITY (sizeof(gfxlink_header_t) + GFXLINK_MAX_PAYLOAD)
static uint8_t s_rx[RX_CAPACITY];
static size_t s_rx_size;

static void consume_frame(const uint8_t *frame)
{
    gfxlink_header_t header;
    memcpy(&header, frame, sizeof(header));
    gfxlink_packet_t *packet = calloc(1, sizeof(*packet));
    if (!packet) return;
    packet->header = header;
    if (header.payload_size) memcpy(packet->payload, frame + sizeof(header), header.payload_size);
    if (xQueueSend(g_gfxlink_dispatch_queue, &packet, 0) != pdTRUE) free(packet);
}

static void parse_frames(void)
{
    while (s_rx_size >= sizeof(gfxlink_header_t)) {
        gfxlink_header_t header;
        memcpy(&header, s_rx, sizeof(header));
        if (header.magic != GFXLINK_MAGIC || header.version != GFXLINK_PROTOCOL_VERSION ||
            header.payload_size > GFXLINK_MAX_PAYLOAD) {
            memmove(s_rx, s_rx + 1, --s_rx_size);
            continue;
        }
        size_t frame_size = sizeof(header) + header.payload_size;
        if (s_rx_size < frame_size) return;
        consume_frame(s_rx);
        s_rx_size -= frame_size;
        if (s_rx_size) memmove(s_rx, s_rx + frame_size, s_rx_size);
    }
}

void tud_vendor_rx_cb(uint8_t itf, uint8_t const *buffer, uint16_t buffer_size)
{
    (void)buffer;
    (void)buffer_size;
    if (itf != GFXLINK_USB_INTERFACE) return;

    uint8_t chunk[64];
    while (tud_vendor_available()) {
        uint32_t count = tud_vendor_read(chunk, sizeof(chunk));
        if (!count) break;
        if (count > sizeof(s_rx) - s_rx_size) s_rx_size = 0;
        if (count > sizeof(s_rx) - s_rx_size) continue;
        memcpy(s_rx + s_rx_size, chunk, count);
        s_rx_size += count;
        parse_frames();
    }
}
