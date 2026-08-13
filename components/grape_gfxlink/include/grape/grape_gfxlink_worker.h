#pragma once

#include "freertos/queue.h"

extern QueueHandle_t g_gfxlink_dispatch_queue;
void gfxlink_worker(void *arg);
