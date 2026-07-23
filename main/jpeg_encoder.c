/*
 * JPEG Encoder Wrapper — Software JPEG Encoder (esp_new_jpeg)
 *
 * Optimizations:
 * - Persistent encoder handle (no open/close per frame)
 * - Pre-allocated PSRAM buffers
 * - Supports encoding at reduced resolution for web streaming
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

/* Persistent output buffer in PSRAM */
#define JPEG_OUT_BUF_SIZE  (300 * 1024)

static uint8_t *s_out_buf = NULL;

/* Persistent encoder handles for different resolutions */
static jpeg_enc_handle_t s_enc_800 = NULL;   /* Full 800×800 */
static jpeg_enc_handle_t s_enc_400 = NULL;   /* Half 400×400 (for web stream) */

/* Downscale buffer (400×400 RGB565 = 320KB) */
#define DOWNSCALE_SIZE  400
static uint8_t *s_downscale_buf = NULL;

esp_err_t jpeg_encoder_init(void)
{
    if (s_out_buf) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* Allocate output buffer (PSRAM) */
    s_out_buf = (uint8_t *)heap_caps_aligned_calloc(64, 1, JPEG_OUT_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_out_buf) {
        ESP_LOGE(TAG, "Failed to allocate JPEG output buffer (%d bytes)", JPEG_OUT_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    /* Allocate downscale buffer (PSRAM, 400×400×2 = 320KB) — lazy init */
    s_downscale_buf = NULL;  /* Will be allocated on first use */

    /* Open persistent encoder for full resolution */
    jpeg_enc_config_t cfg_800 = {
        .width = 800,
        .height = 800,
        .src_type = JPEG_PIXEL_FORMAT_RGB565_LE,
        .subsampling = JPEG_SUBSAMPLE_420,
        .quality = 40,
        .rotate = JPEG_ROTATE_0D,
        .task_enable = false,
    };
    if (jpeg_enc_open(&cfg_800, &s_enc_800) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "Failed to open 800x800 encoder");
    }

    /* Open persistent encoder for half resolution (web stream) */
    if (s_downscale_buf) {
        jpeg_enc_config_t cfg_400 = {
            .width = DOWNSCALE_SIZE,
            .height = DOWNSCALE_SIZE,
            .src_type = JPEG_PIXEL_FORMAT_RGB565_LE,
            .subsampling = JPEG_SUBSAMPLE_420,
            .quality = 40,
            .rotate = JPEG_ROTATE_0D,
            .task_enable = false,
        };
        if (jpeg_enc_open(&cfg_400, &s_enc_400) != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "Failed to open 400x400 encoder");
        }
    }

    ESP_LOGI(TAG, "JPEG encoder ready (persistent, 800=%s, 400=%s, buf=%dKB)",
             s_enc_800 ? "OK" : "FAIL",
             s_enc_400 ? "OK" : "N/A",
             JPEG_OUT_BUF_SIZE / 1024);
    return ESP_OK;
}

/**
 * @brief 2× nearest-neighbor downscale (800×800 → 400×400)
 *
 * Takes every other pixel from even rows.
 * Processes 32-bit words (2 pixels) for speed.
 */
static void downscale_2x_rgb565(const uint8_t *src, int src_w, int src_h,
                                 uint8_t *dst, int dst_w, int dst_h)
{
    const uint16_t *src16 = (const uint16_t *)src;
    uint32_t *dst32 = (uint32_t *)dst;

    for (int y = 0; y < dst_h; y++) {
        int src_y = y * 2;
        const uint16_t *row = src16 + src_y * src_w;
        for (int x = 0; x < dst_w; x += 2) {
            /* Pack 2 pixels into one 32-bit word */
            uint16_t p0 = row[x * 2];
            uint16_t p1 = row[x * 2 + 2];
            dst32[(y * dst_w + x) / 2] = ((uint32_t)p1 << 16) | p0;
        }
    }
}

/*
 * Encode RGB565 frame to JPEG at full resolution.
 * Returns pointer to internal static buffer — caller must NOT free it.
 */
esp_err_t rgb565_to_jpeg(const uint8_t *rgb565, int width, int height,
                         int quality, uint8_t **jpeg_buf, size_t *jpeg_len)
{
    if (!rgb565 || !jpeg_buf || !jpeg_len || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    jpeg_enc_handle_t enc = s_enc_800;

    /* If quality changed, need to reopen encoder */
    static int s_last_quality = 40;
    if (quality != s_last_quality && enc) {
        jpeg_enc_close(enc);
        s_enc_800 = NULL;
        jpeg_enc_config_t cfg = {
            .width = width, .height = height,
            .src_type = JPEG_PIXEL_FORMAT_RGB565_LE,
            .subsampling = JPEG_SUBSAMPLE_420,
            .quality = (uint8_t)quality,
            .rotate = JPEG_ROTATE_0D,
            .task_enable = false,
        };
        if (jpeg_enc_open(&cfg, &s_enc_800) == JPEG_ERR_OK) {
            enc = s_enc_800;
            s_last_quality = quality;
        }
    }

    if (!enc) {
        return ESP_ERR_INVALID_STATE;
    }

    int out_size = 0;
    int inbuf_size = width * height * 2;

    jpeg_error_t err = jpeg_enc_process(enc, rgb565, inbuf_size,
                                         s_out_buf, JPEG_OUT_BUF_SIZE, &out_size);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG encode failed: %d", err);
        return ESP_FAIL;
    }

    *jpeg_buf = s_out_buf;
    *jpeg_len = (size_t)out_size;
    return ESP_OK;
}

/*
 * Encode RGB565 frame to JPEG at half resolution (for web streaming).
 * Downsamples 800×800 → 400×400 before encoding.
 * Returns pointer to internal static buffer — caller must NOT free it.
 */
esp_err_t rgb565_to_jpeg_half(const uint8_t *rgb565, int width, int height,
                              uint8_t **jpeg_buf, size_t *jpeg_len)
{
    if (!rgb565 || !jpeg_buf || !jpeg_len) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Lazy init: allocate downscale buffer and 400x400 encoder on first use */
    if (!s_downscale_buf) {
        s_downscale_buf = (uint8_t *)heap_caps_aligned_calloc(64, 1,
                            DOWNSCALE_SIZE * DOWNSCALE_SIZE * 2, MALLOC_CAP_SPIRAM);
        if (s_downscale_buf) {
            ESP_LOGI(TAG, "Downscale buffer allocated (320KB)");
        }
    }
    if (!s_enc_400 && s_downscale_buf) {
        jpeg_enc_config_t cfg = {
            .width = DOWNSCALE_SIZE, .height = DOWNSCALE_SIZE,
            .src_type = JPEG_PIXEL_FORMAT_RGB565_LE,
            .subsampling = JPEG_SUBSAMPLE_420,
            .quality = 40, .rotate = JPEG_ROTATE_0D,
            .task_enable = false,
        };
        if (jpeg_enc_open(&cfg, &s_enc_400) == JPEG_ERR_OK) {
            ESP_LOGI(TAG, "400x400 encoder opened");
        }
    }

    if (!s_enc_400 || !s_downscale_buf) {
        /* Fallback to full resolution */
        return rgb565_to_jpeg(rgb565, width, height, 40, jpeg_buf, jpeg_len);
    }

    /* Downscale 800×800 → 400×400 */
    downscale_2x_rgb565(rgb565, width, height,
                         s_downscale_buf, DOWNSCALE_SIZE, DOWNSCALE_SIZE);

    int out_size = 0;
    int inbuf_size = DOWNSCALE_SIZE * DOWNSCALE_SIZE * 2;

    jpeg_error_t err = jpeg_enc_process(s_enc_400, s_downscale_buf, inbuf_size,
                                         s_out_buf, JPEG_OUT_BUF_SIZE, &out_size);
    if (err != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "JPEG encode half failed: %d", err);
        return ESP_FAIL;
    }

    *jpeg_buf = s_out_buf;
    *jpeg_len = (size_t)out_size;
    return ESP_OK;
}

void jpeg_encoder_deinit(void)
{
    if (s_enc_800) {
        jpeg_enc_close(s_enc_800);
        s_enc_800 = NULL;
    }
    if (s_enc_400) {
        jpeg_enc_close(s_enc_400);
        s_enc_400 = NULL;
    }
    if (s_out_buf) {
        heap_caps_free(s_out_buf);
        s_out_buf = NULL;
    }
    if (s_downscale_buf) {
        heap_caps_free(s_downscale_buf);
        s_downscale_buf = NULL;
    }
    ESP_LOGI(TAG, "JPEG encoder deinitialized");
}
