/*
 * Web Server - Camera Status & MJPEG Stream
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations for C++ types used in callbacks */
typedef struct web_server_config {
    /* Camera pipeline node pointer (WhoFrameCapNode*) - for frame peek */
    void *frame_cap_node;
    /* Face detection pointer (WhoDetect*) - for status queries */
    void *detector;
    /* Pointer to vending_active flag */
    volatile bool *vending_active;
    /* Camera resolution */
    uint16_t cam_width;
    uint16_t cam_height;
} web_server_config_t;

/**
 * @brief Start the HTTP server
 * @param config  Configuration with camera/detection references
 * @return ESP_OK on success
 */
esp_err_t web_server_start(const web_server_config_t *config);

/**
 * @brief Stop the HTTP server
 * @return ESP_OK on success
 */
esp_err_t web_server_stop(void);

#ifdef __cplusplus
}
#endif
