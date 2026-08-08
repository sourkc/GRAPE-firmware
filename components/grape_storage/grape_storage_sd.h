#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRAPE_STORAGE_SD_MOUNT_POINT "/sdcard"

#define GRAPE_STORAGE_SD_SLOT_WIDTH  4
#define GRAPE_STORAGE_SD_SLOT_CLK    43
#define GRAPE_STORAGE_SD_SLOT_CMD    44
#define GRAPE_STORAGE_SD_SLOT_D0     39
#define GRAPE_STORAGE_SD_SLOT_D1     40
#define GRAPE_STORAGE_SD_SLOT_D2     41
#define GRAPE_STORAGE_SD_SLOT_D3     42

#define GRAPE_STORAGE_SD_LDO_CHANNEL 4

esp_err_t grape_storage_sd_mount(void);
esp_err_t grape_storage_sd_unmount(void);
bool grape_storage_sd_is_mounted(void);

#ifdef __cplusplus
}
#endif
