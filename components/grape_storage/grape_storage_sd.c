#include "grape_storage_sd.h"

#include <stdio.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char *TAG = "grape_storage";

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_power_control;

esp_err_t grape_storage_sd_mount(void)
{
    if (s_card) {
        return ESP_OK;
    }

    esp_err_t ret;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = GRAPE_STORAGE_SD_LDO_CHANNEL,
    };

    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_power_control);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SDMMC IO power: %s", esp_err_to_name(ret));
        return ret;
    }

    host.pwr_ctrl_handle = s_power_control;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = GRAPE_STORAGE_SD_SLOT_WIDTH;
    slot_config.clk = GRAPE_STORAGE_SD_SLOT_CLK;
    slot_config.cmd = GRAPE_STORAGE_SD_SLOT_CMD;
    slot_config.d0 = GRAPE_STORAGE_SD_SLOT_D0;
    slot_config.d1 = GRAPE_STORAGE_SD_SLOT_D1;
    slot_config.d2 = GRAPE_STORAGE_SD_SLOT_D2;
    slot_config.d3 = GRAPE_STORAGE_SD_SLOT_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    ESP_LOGI(
        TAG,
        "Mounting SD card at %s (4-bit SDMMC, CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d)",
        GRAPE_STORAGE_SD_MOUNT_POINT,
        GRAPE_STORAGE_SD_SLOT_CLK,
        GRAPE_STORAGE_SD_SLOT_CMD,
        GRAPE_STORAGE_SD_SLOT_D0,
        GRAPE_STORAGE_SD_SLOT_D1,
        GRAPE_STORAGE_SD_SLOT_D2,
        GRAPE_STORAGE_SD_SLOT_D3
    );

    ret = esp_vfs_fat_sdmmc_mount(
        GRAPE_STORAGE_SD_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));

        if (s_power_control) {
            esp_err_t power_ret = sd_pwr_ctrl_del_on_chip_ldo(s_power_control);
            if (power_ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to release SDMMC IO power: %s", esp_err_to_name(power_ret));
            }
            s_power_control = NULL;
        }

        s_card = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted");
    sdmmc_card_print_info(stdout, s_card);

    return ESP_OK;
}

esp_err_t grape_storage_sd_unmount(void)
{
    if (!s_card) {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(
        GRAPE_STORAGE_SD_MOUNT_POINT,
        s_card
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    s_card = NULL;

    if (s_power_control) {
        ret = sd_pwr_ctrl_del_on_chip_ldo(s_power_control);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to release SDMMC IO power: %s", esp_err_to_name(ret));
            return ret;
        }
        s_power_control = NULL;
    }

    ESP_LOGI(TAG, "SD card unmounted");
    return ESP_OK;
}

bool grape_storage_sd_is_mounted(void)
{
    return s_card != NULL;
}
