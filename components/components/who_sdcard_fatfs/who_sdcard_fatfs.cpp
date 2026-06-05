#include "who_sdcard_fatfs.hpp"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"

static const char *TAG = "sdcard_fatfs";
static sdmmc_card_t *s_card = nullptr;

esp_err_t fatfs_sd_mount(void)
{
    ESP_LOGI(TAG, "Initializing SD card");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = true;
    mount_config.max_files = 5;
    mount_config.allocation_unit_size = 16 * 1024;

    ESP_LOGI(TAG, "Mounting filesystem");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        CONFIG_SDCARD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount (%s)", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", CONFIG_SDCARD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t fatfs_sd_unmount(void)
{
    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(CONFIG_SDCARD_MOUNT_POINT, s_card);
    s_card = nullptr;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SD card unmounted");
    }
    return ret;
}
