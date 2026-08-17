#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>

#include "demo_internal.h"
#include "esp_log.h"
#include "grape_storage_sd.h"

static const char *TAG = "GRAPE_DEMO";

static void log_saved_file(const char *label, const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) == 0) {
        ESP_LOGI(TAG, "%s screenshot: %s (%ld bytes)", label, path, (long)st.st_size);
    } else {
        ESP_LOGI(TAG, "%s screenshot: %s", label, path);
    }
}

esp_err_t grape_demo_screenshot_run(grape_context_t *grape)
{
    /* Reuse the AA diagnostic scene so the lossless capture is immediately
     * useful for inspecting the renderer's current edge/filtering behavior. */
    esp_err_t ret = grape_demo_aa_run(grape);
    if (ret != ESP_OK) {
        return ret;
    }

    bool mounted_here = !grape_storage_sd_is_mounted();
    if (mounted_here) {
        ret = grape_storage_sd_mount();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    grape_screenshot_desc_t png = GRAPE_SCREENSHOT_DESC_DEFAULT();
    png.format = GRAPE_SCREENSHOT_FORMAT_PNG;
    png.png_compression_level = 3;

    ret = grape_screenshot_save(grape, &png, "/sdcard/grape_aa.png");
    if (ret == ESP_OK) {
        log_saved_file("PNG", "/sdcard/grape_aa.png");
    }

    if (ret == ESP_OK) {
        grape_screenshot_desc_t jpeg = GRAPE_SCREENSHOT_DESC_DEFAULT();
        jpeg.format = GRAPE_SCREENSHOT_FORMAT_JPEG;
        jpeg.jpeg_quality = 95U;
        jpeg.jpeg_subsampling = GRAPE_SCREENSHOT_JPEG_YUV444;

        ret = grape_screenshot_save(grape, &jpeg, "/sdcard/grape_aa.jpg");
        if (ret == ESP_OK) {
            log_saved_file("JPEG", "/sdcard/grape_aa.jpg");
        }
    }

    if (mounted_here) {
        esp_err_t unmount_ret = grape_storage_sd_unmount();
        if (ret == ESP_OK) {
            ret = unmount_ret;
        }
    }

    return ret;
}
