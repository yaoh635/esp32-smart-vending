/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file camera.c
 * @brief 摄像头驱动模块 - MIPI CSI + ISP 全流水线 + AWB 自动白平衡
 *
 * 本模块负责:
 * 1. 初始化 MIPI CSI 摄像头传感器 (OV5647, RAW8 800×800)
 * 2. 配置双缓冲帧管理（CSI ping-pong，无 LCD 显示）
 * 3. ISP 流水线: BLC → BF → LSC → Demosaic → WBG → CCM → Gamma → Sharpen → Color
 * 4. AWB 自动白平衡: 硬件统计 + WBG 增益调节
 * 5. 提供帧数据访问接口（供 web server + 人脸检测使用）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/isp_demosaic.h"
#include "driver/isp_ccm.h"
#include "driver/isp_color.h"
#include "driver/isp_sharpen.h"
#include "driver/isp_bf.h"
#include "driver/isp_gamma.h"
#include "driver/isp_wbg.h"
#include "driver/isp_awb.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "esp_sccb_intf.h"
#include "esp_sccb_i2c.h"
#include "example_sensor_init_config.h"
#include "example_config.h"
#include "camera.h"

static const char *TAG = "camera";

/* Gamma=0.6 伽马校正函数：y = 256 * (x/256)^0.6，适度提亮暗部 */
static uint32_t gamma_brighten(uint32_t x)
{
    if (x >= 256) return 256;
    return (uint32_t)(256.0 * pow((double)x / 256.0, 0.6));
}

/* ==================== 双缓冲机制 ==================== */
/* 使用两个缓冲区交替存储帧数据，实现 CSI ping-pong（无 LCD 显示） */
static void *s_frame_buffers[2] = {NULL, NULL};
static volatile int s_active_buf_idx = 0;
static SemaphoreHandle_t s_frame_ready_sem = NULL;      /* 帧就绪信号量（内部同步用） */
static esp_cam_sensor_device_t *s_cam_sensor = NULL;
static isp_proc_handle_t s_isp_proc = NULL;
static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;

/* AWB 控制器 */
static isp_awb_ctlr_t s_awb_ctlr = NULL;
static QueueHandle_t s_awb_queue = NULL;
static TaskHandle_t s_awb_task_handle = NULL;

/* 帧缓冲区大小: 800×800×2 bytes (RGB565) */
#define FRAME_BUFFER_SIZE  (CAM_H_RES * CAM_V_RES * EXAMPLE_RGB565_BYTES_PER_PIXEL)

/* AWB 参数 */
#define AWB_GAIN_NORM           256
#define AWB_P_GAIN              0.5f
#define AWB_GAIN_UPDATE_COUNT   5

/* ==================== 回调函数 ==================== */

static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    trans->buffer = s_frame_buffers[s_active_buf_idx];
    trans->buflen = FRAME_BUFFER_SIZE;
    return false;
}

static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    /* 切换到另一个缓冲区 */
    s_active_buf_idx = 1 - s_active_buf_idx;

    /* 通知帧就绪 */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_ready_sem, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return false;
}

/* ==================== 访问接口 ==================== */

void *camera_get_frame_buffer(int index)           { return s_frame_buffers[index]; }
size_t camera_get_frame_buffer_size(void)          { return FRAME_BUFFER_SIZE; }
SemaphoreHandle_t camera_get_frame_ready_sem(void) { return s_frame_ready_sem; }
volatile int *camera_get_display_buf_idx_ptr(void) { return &s_active_buf_idx; }

/* 获取最新完成的帧缓冲区（刚写完的那个） */
void *camera_get_latest_frame(void)
{
    /* s_active_buf_idx 已切换，所以 1-s_active_buf_idx 是刚完成的帧 */
    return s_frame_buffers[1 - s_active_buf_idx];
}

/* ==================== ISP 辅助模块 ==================== */

#if CONFIG_ESP32P4_REV_MIN_FULL >= 100
/**
 * @brief 配置 LSC（镜头阴影校正）模块
 */
static esp_err_t camera_config_lsc(isp_proc_handle_t isp_proc)
{
    esp_isp_lsc_gain_array_t gain_array = {};
    esp_isp_lsc_config_t lsc_config = {
        .gain_array = &gain_array,
    };

    size_t gain_size = 0;
    esp_err_t ret = esp_isp_lsc_allocate_gain_array(isp_proc, &gain_array, &gain_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LSC gain alloc failed: %d", ret);
        return ret;
    }

    /* 默认增益：轻微补偿边缘衰减 */
    isp_lsc_gain_t gain_val = { .decimal = 204, .integer = 0 };
    for (int i = 0; i < gain_size; i++) {
        gain_array.gain_r[i].val = gain_val.val;
        gain_array.gain_gr[i].val = gain_val.val;
        gain_array.gain_gb[i].val = gain_val.val;
        gain_array.gain_b[i].val = gain_val.val;
    }

    ret = esp_isp_lsc_configure(isp_proc, &lsc_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LSC configure failed: %d", ret);
        return ret;
    }
    ret = esp_isp_lsc_enable(isp_proc);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LSC enabled (edge compensation)");
    }
    return ret;
}
#endif /* rev >= 100 */

#if CONFIG_ESP32P4_REV_MIN_FULL >= 300
/**
 * @brief 配置 BLC（黑电平校正）模块
 */
static esp_err_t camera_config_blc(isp_proc_handle_t isp_proc)
{
    esp_isp_blc_config_t blc_config = {
        .window = {
            .top_left = { .x = 0, .y = 0 },
            .btm_right = { .x = CAM_H_RES, .y = CAM_V_RES },
        },
        .filter_enable = true,
        .filter_threshold = {
            .top_left_chan_thresh = 128,
            .top_right_chan_thresh = 128,
            .bottom_left_chan_thresh = 128,
            .bottom_right_chan_thresh = 128,
        },
        .stretch = {
            .top_left_chan_stretch_en = true,
            .top_right_chan_stretch_en = true,
            .bottom_left_chan_stretch_en = true,
            .bottom_right_chan_stretch_en = true,
        },
    };

    esp_err_t ret = esp_isp_blc_configure(isp_proc, &blc_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLC configure failed: %d", ret);
        return ret;
    }
    ret = esp_isp_blc_enable(isp_proc);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLC enable failed: %d", ret);
        return ret;
    }

    esp_isp_blc_offset_t blc_offset = {
        .top_left_chan_offset = 20,
        .top_right_chan_offset = 20,
        .bottom_left_chan_offset = 20,
        .bottom_right_chan_offset = 20,
    };
    ret = esp_isp_blc_set_correction_offset(isp_proc, &blc_offset);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BLC enabled (offset=20)");
    }
    return ret;
}
#endif /* rev >= 300 */

/* ==================== AWB 自动白平衡 ==================== */

/**
 * @brief AWB 统计回调（ISR 上下文）
 */
static bool IRAM_ATTR s_awb_statistics_callback(isp_awb_ctlr_t awb_ctlr,
                                                 const esp_isp_awb_evt_data_t *edata,
                                                 void *user_data)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_awb_queue != NULL) {
        if (xQueueSendFromISR(s_awb_queue, &edata->awb_result, &xHigherPriorityTaskWoken) != pdTRUE) {
            return false;
        }
    }
    return xHigherPriorityTaskWoken == pdTRUE;
}

/**
 * @brief PI 控制器平滑更新白平衡增益
 */
static void s_awb_pi_update(isp_wbg_gain_t target, isp_wbg_gain_t current, isp_wbg_gain_t *out)
{
    float err_r = (float)target.gain_r - (float)current.gain_r;
    float err_b = (float)target.gain_b - (float)current.gain_b;

    out->gain_r = (uint32_t)((float)current.gain_r + AWB_P_GAIN * err_r + 0.5f);
    out->gain_g = AWB_GAIN_NORM;
    out->gain_b = (uint32_t)((float)current.gain_b + AWB_P_GAIN * err_b + 0.5f);
}

/**
 * @brief 从 AWB 统计计算白平衡增益
 */
static bool s_awb_calc_gain(const isp_awb_stat_result_t *stat, isp_wbg_gain_t *gain)
{
    if (stat->white_patch_num == 0 || stat->sum_r == 0 || stat->sum_b == 0) {
        return false;
    }

    float gr = ((float)stat->sum_g / (float)stat->sum_r) * AWB_GAIN_NORM;
    float gb = ((float)stat->sum_g / (float)stat->sum_b) * AWB_GAIN_NORM;

    gain->gain_r = (uint32_t)(gr + 0.5f);
    gain->gain_g = AWB_GAIN_NORM;
    gain->gain_b = (uint32_t)(gb + 0.5f);
    return true;
}

/**
 * @brief AWB 处理任务
 */
static void s_awb_task(void *arg)
{
    isp_awb_stat_result_t stat;
    isp_wbg_gain_t current_gain = { .gain_r = AWB_GAIN_NORM, .gain_g = AWB_GAIN_NORM, .gain_b = AWB_GAIN_NORM };
    uint32_t count = 0;
    uint64_t sum_r = 0, sum_g = 0, sum_b = 0;

    ESP_LOGI(TAG, "AWB task started");

    while (1) {
        if (xQueueReceive(s_awb_queue, &stat, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        isp_wbg_gain_t gain;
        if (!s_awb_calc_gain(&stat, &gain)) {
            continue;
        }

        sum_r += gain.gain_r;
        sum_g += gain.gain_g;
        sum_b += gain.gain_b;
        count++;

        if (count >= AWB_GAIN_UPDATE_COUNT) {
            isp_wbg_gain_t target = {
                .gain_r = (uint32_t)(sum_r / count),
                .gain_g = (uint32_t)(sum_g / count),
                .gain_b = (uint32_t)(sum_b / count),
            };

            isp_wbg_gain_t new_gain;
            s_awb_pi_update(target, current_gain, &new_gain);

            esp_err_t ret = esp_isp_wbg_set_wb_gain(s_isp_proc, new_gain);
            if (ret == ESP_OK) {
                current_gain = new_gain;
                ESP_LOGD(TAG, "AWB: R=%lu G=%lu B=%lu",
                         new_gain.gain_r, new_gain.gain_g, new_gain.gain_b);
            }

            count = 0;
            sum_r = sum_g = sum_b = 0;
        }
    }
}

/**
 * @brief 初始化并启动 AWB
 */
static esp_err_t camera_init_awb(isp_proc_handle_t isp_proc)
{
    /* 配置 WBG 模块 */
    esp_isp_wbg_config_t wbg_config = {
        .flags = { .update_once_configured = true },
    };
    esp_err_t ret = esp_isp_wbg_configure(isp_proc, &wbg_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WBG configure failed: %d", ret);
        return ret;
    }
    ESP_RETURN_ON_ERROR(esp_isp_wbg_enable(isp_proc), TAG, "WBG enable failed");

    /* 设置初始增益（中性） */
    isp_wbg_gain_t init_gain = { .gain_r = AWB_GAIN_NORM, .gain_g = AWB_GAIN_NORM, .gain_b = AWB_GAIN_NORM };
    esp_isp_wbg_set_wb_gain(isp_proc, init_gain);

    /* 创建 AWB 控制器（统计用） */
    esp_isp_awb_config_t awb_config = {
        .sample_point = ISP_AWB_SAMPLE_POINT_AFTER_CCM,
        .window = {
            .top_left = { .x = CAM_H_RES * 0.2f, .y = CAM_V_RES * 0.2f },
            .btm_right = { .x = CAM_H_RES * 0.8f - 1, .y = CAM_V_RES * 0.8f - 1 },
        },
        .subwindow = {
            .top_left = { .x = CAM_H_RES * 0.2f, .y = CAM_V_RES * 0.2f },
            .btm_right = { .x = CAM_H_RES * 0.8f - 1, .y = CAM_V_RES * 0.8f - 1 },
        },
        .white_patch = {
            .luminance = { .min = 0, .max = 220 * 3 },
            .red_green_ratio = { .min = 0.5f, .max = 1.999f },
            .blue_green_ratio = { .min = 0.5f, .max = 1.999f },
        },
        .intr_priority = 0,
    };

    ret = esp_isp_new_awb_controller(isp_proc, &awb_config, &s_awb_ctlr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AWB controller create failed: %d", ret);
        return ret;
    }

    /* AWB 统计队列 */
    s_awb_queue = xQueueCreateWithCaps(1, sizeof(isp_awb_stat_result_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_awb_queue) {
        ESP_LOGW(TAG, "AWB queue create failed");
        esp_isp_del_awb_controller(s_awb_ctlr);
        s_awb_ctlr = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* 注册回调 */
    esp_isp_awb_cbs_t awb_cbs = { .on_statistics_done = s_awb_statistics_callback };
    ESP_GOTO_ON_ERROR(esp_isp_awb_register_event_callbacks(s_awb_ctlr, &awb_cbs, NULL), err, TAG, "AWB cb register failed");
    ESP_GOTO_ON_ERROR(esp_isp_awb_controller_enable(s_awb_ctlr), err, TAG, "AWB enable failed");
    ESP_GOTO_ON_ERROR(esp_isp_awb_controller_start_continuous_statistics(s_awb_ctlr), err, TAG, "AWB start failed");

    /* 创建 AWB 处理任务 */
    if (xTaskCreate(s_awb_task, "awb_task", 4096, NULL, 5, &s_awb_task_handle) != pdPASS) {
        ESP_LOGW(TAG, "AWB task create failed");
        goto err;
    }

    ESP_LOGI(TAG, "AWB enabled (WBG + statistics + PI controller)");
    return ESP_OK;

err:
    esp_isp_awb_controller_stop_continuous_statistics(s_awb_ctlr);
    esp_isp_awb_controller_disable(s_awb_ctlr);
    esp_isp_wbg_disable(isp_proc);
    if (s_awb_queue) {
        vQueueDeleteWithCaps(s_awb_queue);
        s_awb_queue = NULL;
    }
    esp_isp_del_awb_controller(s_awb_ctlr);
    s_awb_ctlr = NULL;
    return ret;
}

/* ==================== 初始化函数 ==================== */

esp_err_t camera_init(esp_cam_ctlr_handle_t *cam_handle, esp_cam_ctlr_trans_t *trans)
{
    esp_err_t ret;

    /* ========== 步骤1: MIPI PHY LDO 电源 ========== */
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = CONFIG_EXAMPLE_USED_LDO_CHAN_ID,
        .voltage_mv = CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy), TAG, "LDO init failed");

    /* ========== 步骤2: 帧就绪信号量 ========== */
    s_frame_ready_sem = xSemaphoreCreateBinary();
    assert(s_frame_ready_sem);

    /* ========== 步骤3: 双缓冲区 (PSRAM) ========== */
    for (int i = 0; i < 2; i++) {
        s_frame_buffers[i] = heap_caps_aligned_calloc(128, 1, FRAME_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        assert(s_frame_buffers[i]);
        memset(s_frame_buffers[i], 0x00, FRAME_BUFFER_SIZE);
        esp_cache_msync(s_frame_buffers[i], FRAME_BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    ESP_LOGI(TAG, "Dual buffers: %p %p (each %zu bytes)", s_frame_buffers[0], s_frame_buffers[1], FRAME_BUFFER_SIZE);

    trans->buffer = s_frame_buffers[0];
    trans->buflen = FRAME_BUFFER_SIZE;

    /* ========== 步骤4: I2C 总线 (SCCB) ========== */
    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO,
        .scl_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO,
        .i2c_port = I2C_NUM_0,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_conf, &i2c_bus_handle), TAG, "I2C init failed");
    s_i2c_bus_handle = i2c_bus_handle;

    /* ========== 步骤5: 检测摄像头传感器 ========== */
    esp_cam_sensor_config_t cam_config = {
        .reset_pin = -1,
        .pwdn_pin = -1,
        .xclk_pin = -1,
    };
    esp_cam_sensor_device_t *cam = NULL;
    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        sccb_i2c_config_t i2c_config = {
            .scl_speed_hz = EXAMPLE_CAM_SCCB_FREQ,
            .device_address = p->sccb_addr,
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        };
        ESP_RETURN_ON_ERROR(sccb_new_i2c_io(i2c_bus_handle, &i2c_config, &cam_config.sccb_handle),
                            TAG, "SCCB IO create failed");
        cam_config.sensor_port = p->port;
        cam = (*(p->detect))(&cam_config);
        if (cam) {
            ESP_LOGI(TAG, "Camera sensor detected");
            break;
        }
        ESP_ERROR_CHECK(esp_sccb_del_i2c_io(cam_config.sccb_handle));
    }
    assert(cam);
    s_cam_sensor = cam;

    /* ========== 步骤6: 设置摄像头输出格式 ========== */
    esp_cam_sensor_format_array_t cam_fmt_array = {0};
    esp_cam_sensor_query_format(cam, &cam_fmt_array);
    esp_cam_sensor_format_t *cam_cur_fmt = NULL;
    for (int i = 0; i < cam_fmt_array.count; i++) {
        ESP_LOGI(TAG, "Supported format[%d]: %s", i, cam_fmt_array.format_array[i].name);
        if (!strcmp(cam_fmt_array.format_array[i].name, EXAMPLE_CAM_FORMAT)) {
            cam_cur_fmt = (esp_cam_sensor_format_t *)&cam_fmt_array.format_array[i];
        }
    }
    assert(cam_cur_fmt);
    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(cam, cam_cur_fmt), TAG, "Set format failed");
    ESP_LOGI(TAG, "Current format: %s", cam_cur_fmt->name);

    /* ========== 步骤6.5: 调整传感器寄存器 - 适度提高画面亮度 ========== */
    esp_cam_sensor_reg_val_t ae_regs[] = {
        {.regaddr = 0x3A0F, .value = 0x68},  /* AE target high */
        {.regaddr = 0x3A10, .value = 0x60},  /* AE target low */
        {.regaddr = 0x3A1B, .value = 0x68},  /* AE target high fast */
        {.regaddr = 0x3A1E, .value = 0x60},  /* AE target low fast */
        {.regaddr = 0x3A11, .value = 0x70},  /* AE fast high threshold */
        {.regaddr = 0x3A1F, .value = 0x28},  /* AE fast low threshold */
        {.regaddr = 0x3A18, .value = 0x01},  /* gain ceiling high */
        {.regaddr = 0x3A19, .value = 0xC0},  /* gain ceiling low (24x) */
    };
    for (int i = 0; i < sizeof(ae_regs)/sizeof(ae_regs[0]); i++) {
        esp_err_t reg_ret = esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_REG, &ae_regs[i]);
        if (reg_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to write reg 0x%04X: %s", ae_regs[i].regaddr, esp_err_to_name(reg_ret));
        }
    }
    ESP_LOGI(TAG, "OV5647 AE target adjusted, gain ceiling 24x");

    /* ========== 步骤7: 启动传感器数据流 ========== */
    int enable_flag = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag),
                        TAG, "Sensor stream start failed");

    /* ========== 步骤8: CSI 控制器 ========== */
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,
        .h_res = CAM_H_RES,
        .v_res = CAM_V_RES,
        .lane_bit_rate_mbps = EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS,
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,
        .data_lane_num = 2,
        .byte_swap_en = false,
        .queue_items = 1,
    };
    ret = esp_cam_new_csi_ctlr(&csi_config, cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI init failed [%d]", ret);
        return ret;
    }

    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = s_camera_get_new_vb,
        .on_trans_finished = s_camera_get_finished_trans,
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(*cam_handle, &cbs, trans), TAG, "Register callbacks failed");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(*cam_handle), TAG, "CSI enable failed");

    /* ========== 步骤9: ISP 处理器 ========== */
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,
        .input_data_color_type = ISP_COLOR_RAW8,
        .output_data_color_type = ISP_COLOR_RGB565,
        .has_line_start_packet = false,
        .has_line_end_packet = false,
        .h_res = CAM_H_RES,
        .v_res = CAM_V_RES,
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG,
    };
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_config, &s_isp_proc), TAG, "ISP create failed");
    ESP_RETURN_ON_ERROR(esp_isp_enable(s_isp_proc), TAG, "ISP enable failed");

    /* ========== ISP 流水线模块（按硬件顺序） ========== */
    int isp_enhanced = 0;

    /* [1] BLC: 黑电平校正 */
#if CONFIG_ESP32P4_REV_MIN_FULL >= 300
    if (camera_config_blc(s_isp_proc) == ESP_OK) {
        isp_enhanced++;
    }
#endif

    /* [2] BF: 双边滤波降噪 */
    esp_isp_bf_config_t bf_config = {
        .denoising_level = 2,
        .padding_mode = 0,
        .padding_data = 0,
        .bf_template = {{1,2,1},{2,4,2},{1,2,1}},
        .flags = {.update_once_configured = 1},
    };
    if (esp_isp_bf_configure(s_isp_proc, &bf_config) == ESP_OK &&
        esp_isp_bf_enable(s_isp_proc) == ESP_OK) {
        ESP_LOGI(TAG, "BF enabled (denoising_level=2)");
        isp_enhanced++;
    }

    /* [3] LSC: 镜头阴影校正 */
#if CONFIG_ESP32P4_REV_MIN_FULL >= 100
    if (camera_config_lsc(s_isp_proc) == ESP_OK) {
        isp_enhanced++;
    }
#endif

    /* [4] Demosaic: 去马赛克 (RAW8 → RGB) */
    esp_isp_demosaic_config_t demosaic_config = {
        .grad_ratio = { .integer = 2, .decimal = 5 },
    };
    ESP_RETURN_ON_ERROR(esp_isp_demosaic_configure(s_isp_proc, &demosaic_config), TAG, "Demosaic config failed");
    ESP_RETURN_ON_ERROR(esp_isp_demosaic_enable(s_isp_proc), TAG, "Demosaic enable failed");
    isp_enhanced++;

    /* [5] WBG + AWB: 白平衡（在 Demosaic 之后，CCM 之前） */
    if (camera_init_awb(s_isp_proc) == ESP_OK) {
        isp_enhanced++;
    } else {
        ESP_LOGW(TAG, "AWB not available, using fixed white balance");
    }

    /* [6] CCM: 颜色校正矩阵 */
    esp_isp_ccm_config_t ccm_config = {
        .matrix = {
            { 2.0000, -0.5459, -0.4541},
            {-0.4751,  1.7696, -0.2945},
            {-0.2002, -0.7998,  2.0000},
        },
        .saturation = true,
        .flags = {.update_once_configured = 1},
    };
    if (esp_isp_ccm_configure(s_isp_proc, &ccm_config) == ESP_OK &&
        esp_isp_ccm_enable(s_isp_proc) == ESP_OK) {
        ESP_LOGI(TAG, "CCM enabled (OV5647 matrix)");
        isp_enhanced++;
    }

    /* [7] Gamma: 伽马校正 */
    isp_gamma_curve_points_t gamma_pts;
    esp_isp_gamma_fill_curve_points(gamma_brighten, &gamma_pts);
    if (esp_isp_gamma_configure(s_isp_proc, COLOR_COMPONENT_R, &gamma_pts) == ESP_OK &&
        esp_isp_gamma_configure(s_isp_proc, COLOR_COMPONENT_G, &gamma_pts) == ESP_OK &&
        esp_isp_gamma_configure(s_isp_proc, COLOR_COMPONENT_B, &gamma_pts) == ESP_OK &&
        esp_isp_gamma_enable(s_isp_proc) == ESP_OK) {
        ESP_LOGI(TAG, "Gamma enabled (param=0.6)");
        isp_enhanced++;
    }

    /* [8] Sharpen: 锐化 */
    esp_isp_sharpen_config_t sharpen_config = {
        .h_freq_coeff = { .integer = 0, .decimal = 16 },  /* 16/32 = 0.5 */
        .m_freq_coeff = { .integer = 0, .decimal = 20 },  /* 20/32 = 0.625 */
        .h_thresh = 100,
        .l_thresh = 30,
        .padding_mode = 0,
        .padding_data = 0,
        .sharpen_template = {{1,2,1},{2,2,2},{1,2,1}},
        .flags = {.update_once_configured = 1},
    };
    if (esp_isp_sharpen_configure(s_isp_proc, &sharpen_config) == ESP_OK &&
        esp_isp_sharpen_enable(s_isp_proc) == ESP_OK) {
        ESP_LOGI(TAG, "Sharpen enabled (h=0.50 m=0.625)");
        isp_enhanced++;
    }

    /* [9] Color: 亮度/对比度/饱和度 */
    esp_isp_color_config_t color_config = {
        .color_brightness = 35,
        .color_contrast = { .integer = 0, .decimal = 108 },
        .color_saturation = { .integer = 0, .decimal = 102 },
        .color_hue = 0,
        .flags = {.update_once_configured = 1},
    };
    if (esp_isp_color_configure(s_isp_proc, &color_config) == ESP_OK &&
        esp_isp_color_enable(s_isp_proc) == ESP_OK) {
        ESP_LOGI(TAG, "Color enabled (bright=+35 contrast=0.85 sat=0.8)");
        isp_enhanced++;
    }

    ESP_LOGI(TAG, "Camera init done (%dx%d RAW8→RGB565, ISP modules: %d)", CAM_H_RES, CAM_V_RES, isp_enhanced);
    return ESP_OK;
}

esp_err_t camera_start(esp_cam_ctlr_handle_t cam_handle)
{
    return esp_cam_ctlr_start(cam_handle);
}

esp_cam_sensor_device_t *camera_get_sensor_handle(void)
{
    return s_cam_sensor;
}

i2c_master_bus_handle_t camera_get_i2c_bus_handle(void)
{
    return s_i2c_bus_handle;
}

/**
 * @brief 释放 ISP 资源（深度睡眠前调用）
 */
void camera_deinit(void)
{
    /* 停止 AWB */
    if (s_awb_task_handle) {
        vTaskDelete(s_awb_task_handle);
        s_awb_task_handle = NULL;
    }
    if (s_awb_ctlr) {
        esp_isp_awb_controller_stop_continuous_statistics(s_awb_ctlr);
        esp_isp_awb_controller_disable(s_awb_ctlr);
        esp_isp_del_awb_controller(s_awb_ctlr);
        s_awb_ctlr = NULL;
    }
    if (s_awb_queue) {
        vQueueDeleteWithCaps(s_awb_queue);
        s_awb_queue = NULL;
    }

    /* 释放 ISP */
    if (s_isp_proc) {
        ESP_LOGI(TAG, "Disabling ISP pipeline...");
        esp_isp_wbg_disable(s_isp_proc);
        esp_isp_gamma_disable(s_isp_proc);
        esp_isp_bf_disable(s_isp_proc);
        esp_isp_sharpen_disable(s_isp_proc);
        esp_isp_color_disable(s_isp_proc);
        esp_isp_ccm_disable(s_isp_proc);
        esp_isp_demosaic_disable(s_isp_proc);
#if CONFIG_ESP32P4_REV_MIN_FULL >= 100
        esp_isp_lsc_disable(s_isp_proc);
#endif
#if CONFIG_ESP32P4_REV_MIN_FULL >= 300
        esp_isp_blc_disable(s_isp_proc);
#endif
        esp_isp_disable(s_isp_proc);
        esp_isp_del_processor(s_isp_proc);
        s_isp_proc = NULL;
        ESP_LOGI(TAG, "ISP disabled and deleted");
    }
}
