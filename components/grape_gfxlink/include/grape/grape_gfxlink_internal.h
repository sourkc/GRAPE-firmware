#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "grape/gfxlink_protocol.h"
#include "grape/grape.h"

#define GFXLINK_MAX_SURFACES 32u
#define GFXLINK_QUEUE_DEPTH 8u
#define GFXLINK_CPU 1

typedef struct {
    gfxlink_header_t header;
    uint8_t payload[GFXLINK_MAX_PAYLOAD];
    uint8_t response[64];
    uint32_t response_size;
    TaskHandle_t waiter;
} gfxlink_packet_t;

extern grape_context_t *g_gfxlink_grape;
extern QueueHandle_t g_gfxlink_render_queue;
extern uint32_t g_gfxlink_display_width;
extern uint32_t g_gfxlink_display_height;
extern uint32_t g_gfxlink_display_format;

void gfxlink_set_status(gfxlink_packet_t *packet, int32_t status);
void gfxlink_process_render_packet(gfxlink_packet_t *packet);
