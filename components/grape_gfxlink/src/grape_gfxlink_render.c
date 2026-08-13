#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "grape/grape_gfxlink_internal.h"

typedef struct {
    uint32_t handle;
    grape_surface_t *surface;
    grape_texture_t *texture;
} gfxlink_surface_slot_t;

static gfxlink_surface_slot_t s_surfaces[GFXLINK_MAX_SURFACES];
static uint32_t s_next_handle = 1u;

static float float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static gfxlink_surface_slot_t *find_surface(uint32_t handle)
{
    for (size_t i = 0; i < GFXLINK_MAX_SURFACES; ++i) {
        if (s_surfaces[i].handle == handle) return &s_surfaces[i];
    }
    return NULL;
}

static gfxlink_surface_slot_t *find_free_slot(void)
{
    for (size_t i = 0; i < GFXLINK_MAX_SURFACES; ++i) {
        if (s_surfaces[i].handle == 0u) return &s_surfaces[i];
    }
    return NULL;
}

static uint32_t allocate_handle(void)
{
    for (;;) {
        uint32_t handle = s_next_handle++;
        if (handle != 0u && !find_surface(handle)) return handle;
    }
}

static int32_t status_from_esp(esp_err_t error)
{
    if (error == ESP_OK) return GFXLINK_STATUS_OK;
    if (error == ESP_ERR_INVALID_ARG) return GFXLINK_STATUS_INVALID_ARGUMENT;
    if (error == ESP_ERR_NO_MEM) return GFXLINK_STATUS_NO_MEMORY;
    return GFXLINK_STATUS_INTERNAL;
}

void gfxlink_set_status(gfxlink_packet_t *packet, int32_t status)
{
    gfxlink_status_response_t response = { .status = status };
    memcpy(packet->response, &response, sizeof(response));
    packet->response_size = sizeof(response);
}

static void process_create(gfxlink_packet_t *packet)
{
    if (packet->header.payload_size != sizeof(gfxlink_create_solid_surface_request_t)) {
        gfxlink_set_status(packet, GFXLINK_STATUS_INVALID_ARGUMENT);
        return;
    }

    gfxlink_create_solid_surface_request_t request;
    memcpy(&request, packet->payload, sizeof(request));
    float x = float_from_bits(request.x_bits);
    float y = float_from_bits(request.y_bits);
    if (!request.width || !request.height || request.width > g_gfxlink_display_width ||
        request.height > g_gfxlink_display_height || !isfinite(x) || !isfinite(y)) {
        gfxlink_set_status(packet, GFXLINK_STATUS_INVALID_ARGUMENT);
        return;
    }

    gfxlink_surface_slot_t *slot = find_free_slot();
    if (!slot) {
        gfxlink_set_status(packet, GFXLINK_STATUS_BUSY);
        return;
    }

    grape_texture_desc_t desc = {
        .width = request.width,
        .height = request.height,
        .format = GRAPE_PIXEL_FORMAT_A8,
        .memory = GRAPE_MEMORY_DEFAULT,
    };
    grape_texture_t *texture = NULL;
    grape_surface_t *surface = NULL;
    esp_err_t ret = grape_texture_create(g_gfxlink_grape, &desc, &texture);
    if (ret == ESP_OK) {
        uint8_t *pixels = grape_texture_pixels(texture);
        size_t stride = grape_texture_stride(texture);
        for (uint32_t row = 0; row < request.height; ++row) {
            memset(pixels + row * stride, 255, request.width);
        }
        ret = grape_texture_invalidate(texture);
    }
    if (ret == ESP_OK) ret = grape_surface_create(g_gfxlink_grape, texture, &surface);
    if (ret == ESP_OK) {
        ret = grape_surface_set_tint(surface, (grape_color_t){
            .r = request.r, .g = request.g, .b = request.b, .a = request.a,
        });
    }
    if (ret == ESP_OK) ret = grape_surface_set_position(surface, x, y);

    if (ret != ESP_OK) {
        if (surface) grape_surface_destroy(surface);
        if (texture) grape_texture_destroy(texture);
        gfxlink_set_status(packet, status_from_esp(ret));
        return;
    }

    uint32_t handle = allocate_handle();
    *slot = (gfxlink_surface_slot_t){ .handle = handle, .surface = surface, .texture = texture };
    gfxlink_create_surface_response_t response = {
        .status = GFXLINK_STATUS_OK,
        .handle = handle,
    };
    memcpy(packet->response, &response, sizeof(response));
    packet->response_size = sizeof(response);
}

static void process_position(gfxlink_packet_t *packet)
{
    if (packet->header.payload_size != sizeof(gfxlink_set_surface_position_request_t)) {
        gfxlink_set_status(packet, GFXLINK_STATUS_INVALID_ARGUMENT);
        return;
    }
    gfxlink_set_surface_position_request_t request;
    memcpy(&request, packet->payload, sizeof(request));
    gfxlink_surface_slot_t *slot = find_surface(request.handle);
    if (!slot) {
        gfxlink_set_status(packet, GFXLINK_STATUS_NOT_FOUND);
        return;
    }
    float x = float_from_bits(request.x_bits);
    float y = float_from_bits(request.y_bits);
    if (!isfinite(x) || !isfinite(y)) {
        gfxlink_set_status(packet, GFXLINK_STATUS_INVALID_ARGUMENT);
        return;
    }
    gfxlink_set_status(packet, status_from_esp(grape_surface_set_position(slot->surface, x, y)));
}

static void process_color(gfxlink_packet_t *packet)
{
    if (packet->header.payload_size != sizeof(gfxlink_set_surface_color_request_t)) {
        gfxlink_set_status(packet, GFXLINK_STATUS_INVALID_ARGUMENT);
        return;
    }
    gfxlink_set_surface_color_request_t request;
    memcpy(&request, packet->payload, sizeof(request));
    gfxlink_surface_slot_t *slot = find_surface(request.handle);
    if (!slot) {
        gfxlink_set_status(packet, GFXLINK_STATUS_NOT_FOUND);
        return;
    }
    gfxlink_set_status(packet, status_from_esp(grape_surface_set_tint(slot->surface, (grape_color_t){
        .r = request.r, .g = request.g, .b = request.b, .a = request.a,
    })));
}

static void process_destroy(gfxlink_packet_t *packet)
{
    if (packet->header.payload_size != sizeof(gfxlink_destroy_surface_request_t)) {
        gfxlink_set_status(packet, GFXLINK_STATUS_INVALID_ARGUMENT);
        return;
    }
    gfxlink_destroy_surface_request_t request;
    memcpy(&request, packet->payload, sizeof(request));
    gfxlink_surface_slot_t *slot = find_surface(request.handle);
    if (!slot) {
        gfxlink_set_status(packet, GFXLINK_STATUS_NOT_FOUND);
        return;
    }
    esp_err_t ret = grape_surface_destroy(slot->surface);
    if (ret == ESP_OK) ret = grape_texture_destroy(slot->texture);
    if (ret == ESP_OK) memset(slot, 0, sizeof(*slot));
    gfxlink_set_status(packet, status_from_esp(ret));
}

void gfxlink_process_render_packet(gfxlink_packet_t *packet)
{
    switch ((gfxlink_opcode_t)packet->header.opcode) {
    case GFXLINK_OP_CREATE_SOLID_SURFACE: process_create(packet); break;
    case GFXLINK_OP_SET_SURFACE_POSITION: process_position(packet); break;
    case GFXLINK_OP_SET_SURFACE_COLOR: process_color(packet); break;
    case GFXLINK_OP_DESTROY_SURFACE: process_destroy(packet); break;
    default: gfxlink_set_status(packet, GFXLINK_STATUS_UNSUPPORTED); break;
    }
}

esp_err_t grape_gfxlink_process(grape_context_t *grape)
{
    if (!grape || grape != g_gfxlink_grape || !g_gfxlink_render_queue) return ESP_ERR_INVALID_STATE;
    gfxlink_packet_t *packet = NULL;
    while (xQueueReceive(g_gfxlink_render_queue, &packet, 0) == pdTRUE) {
        if (!packet) continue;
        gfxlink_process_render_packet(packet);
        xTaskNotifyGive(packet->waiter);
    }
    return ESP_OK;
}
