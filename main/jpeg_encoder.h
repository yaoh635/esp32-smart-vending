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
 * @brief Encode an RGB565 frame to JPEG at full resolution
 *
 * Uses persistent encoder handle (no open/close per frame).
 * Returns pointer to internal static buffer — caller must NOT free it.
 */
esp_err_t rgb565_to_jpeg(const uint8_t *rgb565, int width, int height,
                         int quality, uint8_t **jpeg_buf, size_t *jpeg_len);

/**
 * @brief Encode an RGB565 frame to JPEG at half resolution (for web streaming)
 *
 * Downsamples 800×800 → 400×400 before encoding.
 * Uses persistent encoder handle — much faster than full resolution.
 * Returns pointer to internal static buffer — caller must NOT free it.
 */
esp_err_t rgb565_to_jpeg_half(const uint8_t *rgb565, int width, int height,
                              uint8_t **jpeg_buf, size_t *jpeg_len);

/**
 * @brief Release the hardware JPEG encoder
 */
void jpeg_encoder_deinit(void);

#ifdef __cplusplus
}
#endif
