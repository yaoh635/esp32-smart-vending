/*
 * WiFi Initialization - ESP-Hosted + STA Mode
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize WiFi subsystem and connect to AP
 *
 * Steps:
 * 1. Initialize esp_hosted transport (connect to ESP32-C6 slave)
 * 2. Initialize WiFi in STA mode
 * 3. Connect to configured AP
 * 4. Wait for IP address
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_init(void);

/**
 * @brief Get the current station IP address as string
 * @param buf   Output buffer
 * @param len   Buffer length
 * @return ESP_OK on success
 */
esp_err_t wifi_get_ip_str(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
