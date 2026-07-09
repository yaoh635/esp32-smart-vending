/*
 * JPEG Encoder Wrapper — ESP32-P4 Hardware JPEG Encoder
 *
 * Wraps the esp_driver_jpeg hardware encoder for RGB565 → JPEG conversion.
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the hardware JPEG encoder
 * @return ESP_OK on success
 */
esp_err_t jpeg_encoder_init(void);

/**
 * @brief Encode an RGB565 frame to JPEG using hardware encoder
 *
 * @param[in]  rgb565    Pointer to RGB565 frame data
 * @param[in]  width     Frame width in pixels
 * @param[in]  height    Frame height in pixels
 * @param[in]  quality   JPEG quality (1-100, higher = better quality + larger file)
 * @param[out] jpeg_buf  Pointer to output JPEG buffer (caller must free with free())
 * @param[out] jpeg_len  Pointer to output JPEG size in bytes
 * @return ESP_OK on success
 */
esp_err_t rgb565_to_jpeg(const uint8_t *rgb565, int width, int height,
                         int quality, uint8_t **jpeg_buf, size_t *jpeg_len);

/**
 * @brief Release the hardware JPEG encoder
 */
void jpeg_encoder_deinit(void);

#ifdef __cplusplus
}
#endif
