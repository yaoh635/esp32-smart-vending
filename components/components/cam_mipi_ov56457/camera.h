/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_cam_ctlr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize camera subsystem (MIPI-CSI + ISP + sensor)
 *
 * Performs: LDO power, PSRAM buffer allocation, I2C bus, sensor detect & format,
 *          CSI controller (bypass), ISP processor with demosaic.
 *
 * @param[out] cam_handle  Camera controller handle for esp_cam_ctlr_receive()
 * @param[out] trans       Pre-filled transaction (buffer & length) for receive loop
 * @return ESP_OK on success
 */
esp_err_t camera_init(esp_cam_ctlr_handle_t *cam_handle, esp_cam_ctlr_trans_t *trans);

/**
 * @brief Start camera capture (must call after camera_init and display task created)
 */
esp_err_t camera_start(esp_cam_ctlr_handle_t cam_handle);

/**
 * @brief 释放 ISP 资源，深度睡眠前必须调用
 */
void camera_deinit(void);

/* Accessors for display task */
void     *camera_get_frame_buffer(int index);
size_t    camera_get_frame_buffer_size(void);
SemaphoreHandle_t camera_get_frame_ready_sem(void);
volatile int *camera_get_display_buf_idx_ptr(void);

/* Sensor handle for register access */
#include "esp_cam_sensor.h"
esp_cam_sensor_device_t *camera_get_sensor_handle(void);

/* I2C bus handle for sharing with audio codecs */
#include "driver/i2c_master.h"
i2c_master_bus_handle_t camera_get_i2c_bus_handle(void);

#define CAM_H_RES   CONFIG_EXAMPLE_MIPI_CSI_DISP_HRES
#define CAM_V_RES   CONFIG_EXAMPLE_MIPI_CSI_DISP_VRES

#ifdef __cplusplus
}
#endif
