/*
 * Face ID Manager Component Implementation
 * Manages face ID registration and purchase logging on SD card
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "face_id_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_default_configs.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "face_id_mgr";

/* SD card handle */
static sdmmc_card_t *s_card = NULL;
static bool s_initialized = false;
static int s_next_id = 1;
static int s_current_user_id = -1;

/* SDMMC pin configuration (matching sdkconfig) */
#define SDMMC_PIN_CLK   43
#define SDMMC_PIN_CMD   44
#define SDMMC_PIN_D0    39
#define SDMMC_PIN_D1    40
#define SDMMC_PIN_D2    41
#define SDMMC_PIN_D3    42

esp_err_t face_id_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card...");

    /* Mount config */
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    /* Host config */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    esp_err_t ret;

    /* LDO power control for SD card */
#if CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO
    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LDO power: %s", esp_err_to_name(ret));
        return ret;
    }
    host.pwr_ctrl_handle = pwr_ctrl_handle;
    ESP_LOGI(TAG, "SD LDO power initialized (channel %d)", CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID);
#endif

    /* Card power reset (pulse GPIO 45) */
#if CONFIG_EXAMPLE_PIN_CARD_POWER_RESET
    gpio_config_t rst_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << CONFIG_EXAMPLE_PIN_CARD_POWER_RESET,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(CONFIG_EXAMPLE_PIN_CARD_POWER_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(CONFIG_EXAMPLE_PIN_CARD_POWER_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Card power reset done (GPIO %d)", CONFIG_EXAMPLE_PIN_CARD_POWER_RESET);
#endif

    /* Slot config */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 4;
    slot_config.clk = SDMMC_PIN_CLK;
    slot_config.cmd = SDMMC_PIN_CMD;
    slot_config.d0  = SDMMC_PIN_D0;
    slot_config.d1  = SDMMC_PIN_D1;
    slot_config.d2  = SDMMC_PIN_D2;
    slot_config.d3  = SDMMC_PIN_D3;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* Mount SD card */
    ret = esp_vfs_fat_sdmmc_mount(FACE_ID_MOUNT_POINT, &host,
                                             &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", FACE_ID_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);

    /* Load existing face database to determine next ID */
    FILE *f = fopen(FACE_DB_PATH, "r");
    if (f) {
        char line[128];
        int max_id = 0;
        /* Skip header */
        if (fgets(line, sizeof(line), f)) {
            while (fgets(line, sizeof(line), f)) {
                int id = 0;
                if (sscanf(line, "%d,", &id) == 1) {
                    if (id > max_id) max_id = id;
                }
            }
        }
        s_next_id = max_id + 1;
        fclose(f);
        ESP_LOGI(TAG, "Loaded face DB, next ID = %d", s_next_id);
    } else {
        /* Create new face DB with header */
        f = fopen(FACE_DB_PATH, "w");
        if (f) {
            fprintf(f, "id,name,enroll_time_us\n");
            fclose(f);
            ESP_LOGI(TAG, "Created new face database");
        }
    }

    /* Create purchase log with header if not exists */
    struct stat st;
    if (stat(PURCHASE_LOG_PATH, &st) != 0) {
        f = fopen(PURCHASE_LOG_PATH, "w");
        if (f) {
            fprintf(f, "timestamp_us,face_id,product_name,price_text\n");
            fclose(f);
            ESP_LOGI(TAG, "Created new purchase log");
        }
    }

    s_initialized = true;
    return ESP_OK;
}

int face_id_register(const char *name)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return -1;
    }

    int id = s_next_id++;

    char actual_name[FACE_ID_MAX_NAME_LEN];
    if (name) {
        strncpy(actual_name, name, sizeof(actual_name) - 1);
        actual_name[sizeof(actual_name) - 1] = '\0';
    } else {
        snprintf(actual_name, sizeof(actual_name), "User_%d", id);
    }

    int64_t now = esp_timer_get_time();

    FILE *f = fopen(FACE_DB_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open face DB for writing");
        s_next_id--;
        return -1;
    }

    fprintf(f, "%d,%s,%lld\n", id, actual_name, (long long)now);
    fclose(f);

    ESP_LOGI(TAG, "Registered face ID %d (%s)", id, actual_name);
    return id;
}

int face_id_find_or_create(int recognized_id)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return -1;
    }

    /* Search for existing ID in database */
    FILE *f = fopen(FACE_DB_PATH, "r");
    if (f) {
        char line[128];
        /* Skip header */
        if (fgets(line, sizeof(line), f)) {
            while (fgets(line, sizeof(line), f)) {
                int id = 0;
                if (sscanf(line, "%d,", &id) == 1) {
                    if (id == recognized_id) {
                        fclose(f);
                        ESP_LOGI(TAG, "Found existing face ID %d", recognized_id);
                        return recognized_id;
                    }
                }
            }
        }
        fclose(f);
    }

    /* Not found, register as new user */
    ESP_LOGI(TAG, "Face ID %d not found, registering new user", recognized_id);
    return face_id_register(NULL);
}

esp_err_t face_id_log_purchase(int face_id, const char *product_name,
                               const char *price_text)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    int64_t now = esp_timer_get_time();

    FILE *f = fopen(PURCHASE_LOG_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open purchase log for writing");
        return ESP_FAIL;
    }

    int n = fprintf(f, "%lld,%d,%s,%s\n", (long long)now, face_id,
            product_name ? product_name : "unknown",
            price_text ? price_text : "$0.00");
    int fc = fclose(f);

    ESP_LOGI(TAG, "Logged purchase: ID=%d product=%s price=%s (fprintf=%d, fclose=%d)",
             face_id, product_name, price_text, n, fc);
    return ESP_OK;
}

void face_id_purchase_callback(const char *product_name,
                               const char *price_text,
                               void *user_data)
{
    (void)user_data;
    int user_id = s_current_user_id;
    if (user_id < 0) {
        user_id = 0; /* Unknown user */
    }
    face_id_log_purchase(user_id, product_name, price_text);
}

void face_id_set_current_user(int id)
{
    s_current_user_id = id;
    ESP_LOGI(TAG, "Current user set to ID %d", id);
}

int face_id_get_current_user(void)
{
    return s_current_user_id;
}

int face_id_get_count(void)
{
    if (!s_initialized) return 0;

    int count = 0;
    FILE *f = fopen(FACE_DB_PATH, "r");
    if (f) {
        char line[128];
        /* Skip header */
        if (fgets(line, sizeof(line), f)) {
            while (fgets(line, sizeof(line), f)) {
                if (strlen(line) > 1) count++;
            }
        }
        fclose(f);
    }
    return count;
}
