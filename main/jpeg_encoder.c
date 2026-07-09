/*
 * JPEG Encoder Wrapper — Software JPEG Encoder (esp_new_jpeg)
 *
 * Uses the esp_new_jpeg managed component for RGB565 → JPEG conversion.
 * Uses a persistent PSRAM output buffer to avoid heap fragmentation during streaming.
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "jpeg_encoder.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "jpeg_encoder";

/* Persistent output buffer in PSRAM — 300KB handles 800×1280 @ quality 75 */
#define JPEG_OUT_BUF_SIZE  (300 * 1024)

static uint8_t *s_out_buf = NULL;

esp_err_t jpeg_encoder_init(void)
{
    if (s_out_buf) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_out_buf = (uint8_t *)heap_caps_malloc(JPEG_OUT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_out_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG output buffer (%d bytes)", JPEG_OUT_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "JPEG encoder ready (software, esp_new_jpeg, buf=%dKB)", JPEG_OUT_BUF_SIZE / 1024);
    return ESP_OK;
}

/*
 * Encode RGB565 frame to JPEG.
 *
 * Returns pointer to an internal static buffer — caller must NOT free it.
 * The buffer is valid until the next call to rgb565_to_jpeg().
 */
esp_err_t rgb565_to_jpeg(const uint8_t *rgb565, int width, int height,
                         int quality, uint8_t **jpeg_buf, size_t *jpeg_len)
{
    if (!rgb565 || !jpeg_buf || !jpeg_len || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_out_buf) {
        ESP_LOGE(TAG, "Encoder not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Configure encoder for this frame */
    jpeg_enc_config_t enc_cfg = {
        .width = width,
        .height = height,
        .src_type = JPEG_PIXEL_FORMAT_RGB565_LE,
        .subsampling = JPEG_SUBSAMPLE_420,
        .quality = (uint8_t)quality,
        .rotate = JPEG_ROTATE_0D,
        .task_enable = false,
        .hfm_task_priority = 0,
        .hfm_task_core = 0,
    };

    jpeg_enc_handle_t encoder = NULL;
    jpeg_error_t err = jpeg_enc_open(&enc_cfg, &encoder);
    if (err != JPEG_ERR_OK || !encoder) {
        ESP_LOGE(TAG, "Failed to open JPEG encoder: %d", err);
        return ESP_FAIL;
    }

    int out_size = 0;
    int inbuf_size = width * height * 2;  /* RGB565 = 2 bytes/pixel */

    err = jpeg_enc_process(encoder, rgb565, inbuf_size,
                           s_out_buf, JPEG_OUT_BUF_SIZE, &out_size);

    jpeg_enc_close(encoder);

    if (err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %d (in=%d, out_buf=%d)", err, inbuf_size, JPEG_OUT_BUF_SIZE);
        return ESP_FAIL;
    }

    if (out_size <= 0 || out_size > JPEG_OUT_BUF_SIZE) {
        ESP_LOGE(TAG, "JPEG output size invalid: %d", out_size);
        return ESP_FAIL;
    }

    *jpeg_buf = s_out_buf;
    *jpeg_len = (size_t)out_size;

    return ESP_OK;
}

void jpeg_encoder_deinit(void)
{
    if (s_out_buf) {
        heap_caps_free(s_out_buf);
        s_out_buf = NULL;
    }
    ESP_LOGI(TAG, "JPEG encoder deinitialized");
}
