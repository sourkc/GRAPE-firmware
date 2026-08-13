#include "esp_log.h"
#include "grape/grape_gfxlink.h"
#include "grape/grape_gfxlink_internal.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"

#define GFXLINK_WORKER_STACK 6144u
#define GFXLINK_WORKER_PRIORITY 4u
#define GFXLINK_USB_EP_SIZE 64u

static const char *TAG = "grape_gfxlink";

grape_context_t *g_gfxlink_grape;
QueueHandle_t g_gfxlink_render_queue;
QueueHandle_t g_gfxlink_dispatch_queue;
uint32_t g_gfxlink_display_width;
uint32_t g_gfxlink_display_height;
uint32_t g_gfxlink_display_format;

static TaskHandle_t s_worker_task;

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = GFXLINK_USB_VID,
    .idProduct = GFXLINK_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *s_string_descriptors[] = {
    (const char[]){0x09, 0x04},
    "GRAPE Project",
    "GRAPE GFXLINK FS",
    "0001",
    "GFXLINK",
};

#define GFXLINK_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)
static const uint8_t s_full_speed_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, GFXLINK_CONFIG_LEN, 0x00, 100),
    TUD_VENDOR_DESCRIPTOR(GFXLINK_USB_INTERFACE, 4, GFXLINK_USB_EP_OUT,
                          GFXLINK_USB_EP_IN, GFXLINK_USB_EP_SIZE),
};

esp_err_t grape_gfxlink_init(grape_context_t *grape)
{
    if (!grape || g_gfxlink_grape) return ESP_ERR_INVALID_ARG;
    const grape_display_info_t *display = grape_get_display_info(grape);
    if (!display) return ESP_ERR_INVALID_STATE;

    g_gfxlink_dispatch_queue = xQueueCreate(GFXLINK_QUEUE_DEPTH, sizeof(gfxlink_packet_t *));
    g_gfxlink_render_queue = xQueueCreate(GFXLINK_QUEUE_DEPTH, sizeof(gfxlink_packet_t *));
    if (!g_gfxlink_dispatch_queue || !g_gfxlink_render_queue) return ESP_ERR_NO_MEM;

    g_gfxlink_grape = grape;
    g_gfxlink_display_width = display->width;
    g_gfxlink_display_height = display->height;
    g_gfxlink_display_format = (uint32_t)display->format;

    if (xTaskCreatePinnedToCore(gfxlink_worker, "gfxlink", GFXLINK_WORKER_STACK, NULL,
                                GFXLINK_WORKER_PRIORITY, &s_worker_task, GFXLINK_CPU) != pdPASS) {
        g_gfxlink_grape = NULL;
        return ESP_ERR_NO_MEM;
    }

    tinyusb_config_t config = TINYUSB_CONFIG_FULL_SPEED(NULL, NULL);
    config.task.xCoreID = GFXLINK_CPU;
    config.descriptor.device = &s_device_descriptor;
    config.descriptor.string = s_string_descriptors;
    config.descriptor.string_count = sizeof(s_string_descriptors) / sizeof(s_string_descriptors[0]);
    config.descriptor.full_speed_config = s_full_speed_config;

    esp_err_t ret = tinyusb_driver_install(&config);
    if (ret != ESP_OK) {
        vTaskDelete(s_worker_task);
        s_worker_task = NULL;
        g_gfxlink_grape = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "GFXLINK v%u USB Full-Speed on CPU%d",
             GFXLINK_PROTOCOL_VERSION, GFXLINK_CPU);
    return ESP_OK;
}
