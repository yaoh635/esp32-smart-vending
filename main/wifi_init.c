/*
 * WiFi Initialization - ESP-Hosted + STA Mode
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_hosted.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"

#include "wifi_init.h"

static const char *TAG = "wifi_init";

/* WiFi event group bits */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif = NULL;
static int s_retry_num = 0;
static char s_ip_str[16] = {0};

#define MAXIMUM_RETRY  CONFIG_APP_WIFI_MAX_RETRY

/* ── WiFi + IP event handler ── */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retry connecting to AP (%d/%d)", s_retry_num, MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "Failed to connect to AP after %d retries", MAXIMUM_RETRY);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_str);
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/* ── ESP-Hosted event handler ── */
static void hosted_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    if (event_base == ESP_HOSTED_EVENT) {
        if (event_id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
            ESP_LOGI(TAG, "ESP-Hosted transport UP");
        } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN) {
            ESP_LOGW(TAG, "ESP-Hosted transport DOWN");
        }
    }
}

esp_err_t wifi_init(void)
{
    esp_err_t ret;

    /* ── 1. NVS flash (required by WiFi) ── */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "NVS flash init failed");

    /* ── 2. TCP/IP + event loop ── */
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Event loop create failed");

    /* ── 3. ESP-Hosted init (connects to ESP32-C6 slave) ── */
    ESP_LOGI(TAG, "Initializing ESP-Hosted...");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, &hosted_event_handler, NULL, NULL),
        TAG, "Hosted event handler register failed");

    esp_hosted_init();
    esp_hosted_connect_to_slave();
    ESP_LOGI(TAG, "ESP-Hosted transport ready");

    /* ── 4. WiFi STA init ── */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_ERROR(!s_sta_netif ? ESP_FAIL : ESP_OK, TAG, "Create STA netif failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "WiFi init failed");

    /* Register event handlers */
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL),
        TAG, "WiFi event handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL),
        TAG, "IP event handler register failed");

    /* Configure WiFi */
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    /* Copy SSID and password from Kconfig */
    strncpy((char *)wifi_config.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wifi_config.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "WiFi set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "WiFi set config failed");

    /* ── 5. Start WiFi and wait for connection ── */
    s_wifi_event_group = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "WiFi start failed");
    ESP_LOGI(TAG, "Connecting to AP: %s", CONFIG_APP_WIFI_SSID);

    /* Wait up to 15 seconds for WiFi connection (don't block forever) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to %s, IP: %s", CONFIG_APP_WIFI_SSID, s_ip_str);
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to %s after %d retries", CONFIG_APP_WIFI_SSID, MAXIMUM_RETRY);
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout (15s)");
    }
    return ESP_FAIL;
}

esp_err_t wifi_get_ip_str(char *buf, size_t len)
{
    if (s_ip_str[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(buf, s_ip_str, len);
    return ESP_OK;
}
