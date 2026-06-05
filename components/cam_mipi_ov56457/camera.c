/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file camera.c
 * @brief 摄像头驱动模块 - 实现MIPI CSI摄像头初始化、双缓冲帧管理和ISP图像处理
 *
 * 本模块负责:
 * 1. 初始化MIPI CSI摄像头传感器
 * 2. 配置双缓冲帧管理机制
 * 3. 设置ISP进行RAW8到RGB565的颜色空间转换
 * 4. 提供帧数据访问接口
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/isp_demosaic.h"
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

/* ==================== 双缓冲机制 ==================== */
/* 使用两个缓冲区交替存储帧数据，实现无缝帧切换 */
static void *s_frame_buffers[2] = {NULL, NULL};        /* 帧缓冲区数组 */
static volatile int s_active_buf_idx = 0;               /* 当前正在接收数据的缓冲区索引 */
static volatile int s_display_buf_idx = -1;             /* 当前可供显示的缓冲区索引 */
static SemaphoreHandle_t s_frame_ready_sem = NULL;      /* 帧就绪信号量，用于通知显示任务 */

/* 帧缓冲区大小计算: 水平分辨率 × 垂直分辨率 × 每像素字节数 */
#define FRAME_BUFFER_SIZE  (CAM_H_RES * CAM_V_RES * EXAMPLE_RGB565_BYTES_PER_PIXEL)

/* ==================== 回调函数 ==================== */

/**
 * @brief 获取新帧缓冲区回调 - 当CSI准备好接收新帧时调用
 * @param handle CSI控制器句柄
 * @param trans 传输描述符，需要填充缓冲区地址和长度
 * @param user_data 用户数据(未使用)
 * @return false 表示不需要上下文切换
 */
static bool s_camera_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    /* 将当前活动缓冲区提供给CSI控制器 */
    trans->buffer = s_frame_buffers[s_active_buf_idx];
    trans->buflen = FRAME_BUFFER_SIZE;
    return false;
}

/**
 * @brief 帧传输完成回调 - 当一帧数据接收完成后调用
 * @param handle CSI控制器句柄
 * @param trans 传输描述符
 * @param user_data 用户数据(未使用)
 * @return false 表示不需要上下文切换
 *
 * 此函数在ISR中执行，完成以下操作:
 * 1. 将当前缓冲区标记为可显示
 * 2. 切换到另一个缓冲区用于下一帧
 * 3. 通知显示任务有新帧可用
 */
static bool s_camera_get_finished_trans(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    /* 将当前缓冲区设为显示缓冲区 */
    s_display_buf_idx = s_active_buf_idx;
    /* 切换到另一个缓冲区(0->1 或 1->0) */
    s_active_buf_idx = 1 - s_active_buf_idx;

    /* 从ISR中释放信号量，通知显示任务 */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_ready_sem, &xHigherPriorityTaskWoken);
    /* 如果有更高优先级任务被唤醒，触发上下文切换 */
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    return false;
}

/* ==================== 访问接口 ==================== */

/**
 * @brief 获取指定索引的帧缓冲区指针
 * @param index 缓冲区索引(0或1)
 * @return 缓冲区指针
 */
void *camera_get_frame_buffer(int index)           { return s_frame_buffers[index]; }

/**
 * @brief 获取帧缓冲区大小
 * @return 缓冲区大小(字节)
 */
size_t camera_get_frame_buffer_size(void)          { return FRAME_BUFFER_SIZE; }

/**
 * @brief 获取帧就绪信号量句柄
 * @return 信号量句柄，用于等待新帧到达
 */
SemaphoreHandle_t camera_get_frame_ready_sem(void) { return s_frame_ready_sem; }

/**
 * @brief 获取显示缓冲区索引指针
 * @return 指向当前显示缓冲区索引的指针
 *
 * 注意: 返回指针而非值，因为索引会在ISR中更新
 */
volatile int *camera_get_display_buf_idx_ptr(void) { return &s_display_buf_idx; }

/* ==================== 初始化函数 ==================== */

/**
 * @brief 初始化摄像头系统
 * @param cam_handle 输出参数，返回CSI控制器句柄
 * @param trans 输出参数，返回初始传输描述符
 * @return ESP_OK 成功，其他值表示失败
 *
 * 初始化流程:
 * 1. 配置MIPI LDO电源
 * 2. 创建双缓冲区(PSRAM中)
 * 3. 初始化I2C总线
 * 4. 检测并初始化摄像头传感器
 * 5. 配置CSI控制器
 * 6. 初始化ISP进行颜色转换
 */
esp_err_t camera_init(esp_cam_ctlr_handle_t *cam_handle, esp_cam_ctlr_trans_t *trans)
{
    esp_err_t ret;

    /* ========== 步骤1: 配置MIPI PHY LDO电源 ========== */
    /* MIPI接口需要专用的低压差稳压器供电 */
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = CONFIG_EXAMPLE_USED_LDO_CHAN_ID,      /* LDO通道ID */
        .voltage_mv = CONFIG_EXAMPLE_USED_LDO_VOLTAGE_MV, /* 输出电压(mV) */
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy), TAG, "LDO初始化失败");

    /* ========== 步骤2: 创建帧就绪信号量 ========== */
    s_frame_ready_sem = xSemaphoreCreateBinary();
    assert(s_frame_ready_sem);

    /* ========== 步骤3: 分配双缓冲区(PSRAM) ========== */
    /* 使用64字节对齐，确保DMA访问效率 */
    for (int i = 0; i < 2; i++) {
        s_frame_buffers[i] = heap_caps_aligned_calloc(64, 1, FRAME_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        assert(s_frame_buffers[i]);
        /* 清零缓冲区 */
        memset(s_frame_buffers[i], 0x00, FRAME_BUFFER_SIZE);
        /* 同步缓存到内存(Cache -> Memory) */
        esp_cache_msync(s_frame_buffers[i], FRAME_BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    ESP_LOGI(TAG, "双缓冲区已分配: %p %p (每个%zu字节)",
             s_frame_buffers[0], s_frame_buffers[1], FRAME_BUFFER_SIZE);

    /* 初始化传输描述符 */
    trans->buffer = s_frame_buffers[0];
    trans->buflen = FRAME_BUFFER_SIZE;

    /* ========== 步骤4: 初始化I2C总线(SCCB) ========== */
    /* SCCB是摄像头传感器的控制总线，基于I2C协议 */
    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,                    /* 时钟源 */
        .sda_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO,      /* SDA引脚 */
        .scl_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO,      /* SCL引脚 */
        .i2c_port = I2C_NUM_0,                                /* I2C端口号 */
        .flags.enable_internal_pullup = true,                 /* 启用内部上拉电阻 */
    };
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_bus_conf, &i2c_bus_handle), TAG, "I2C初始化失败");

    /* ========== 步骤5: 检测摄像头传感器 ========== */
    /* 遍历所有已注册的传感器驱动，尝试检测连接的摄像头 */
    esp_cam_sensor_config_t cam_config = {
        .reset_pin = -1,    /* 复位引脚(-1表示未使用) */
        .pwdn_pin = -1,     /* 电源控制引脚(-1表示未使用) */
        .xclk_pin = -1,     /* 外部时钟引脚(-1表示未使用) */
    };
    esp_cam_sensor_device_t *cam = NULL;
    /* 遍历传感器检测函数数组 */
    for (esp_cam_sensor_detect_fn_t *p = &__esp_cam_sensor_detect_fn_array_start;
         p < &__esp_cam_sensor_detect_fn_array_end; ++p) {
        /* 配置I2C设备参数 */
        sccb_i2c_config_t i2c_config = {
            .scl_speed_hz = EXAMPLE_CAM_SCCB_FREQ,    /* I2C时钟频率 */
            .device_address = p->sccb_addr,            /* 传感器I2C地址 */
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,    /* 7位地址模式 */
        };
        ESP_RETURN_ON_ERROR(sccb_new_i2c_io(i2c_bus_handle, &i2c_config, &cam_config.sccb_handle),
                            TAG, "SCCB IO创建失败");
        cam_config.sensor_port = p->port;
        /* 尝试检测传感器 */
        cam = (*(p->detect))(&cam_config);
        if (cam) {
            ESP_LOGI(TAG, "检测到摄像头传感器");
            break;
        }
        /* 检测失败，释放I2C IO */
        ESP_ERROR_CHECK(esp_sccb_del_i2c_io(cam_config.sccb_handle));
    }
    assert(cam);  /* 确保检测到摄像头 */

    /* ========== 步骤6: 设置摄像头输出格式 ========== */
    /* 查询传感器支持的所有格式 */
    esp_cam_sensor_format_array_t cam_fmt_array = {0};
    esp_cam_sensor_query_format(cam, &cam_fmt_array);
    esp_cam_sensor_format_t *cam_cur_fmt = NULL;
    /* 查找目标格式(由EXAMPLE_CAM_FORMAT宏定义) */
    for (int i = 0; i < cam_fmt_array.count; i++) {
        ESP_LOGI(TAG, "支持格式[%d]: %s", i, cam_fmt_array.format_array[i].name);
        if (!strcmp(cam_fmt_array.format_array[i].name, EXAMPLE_CAM_FORMAT)) {
            cam_cur_fmt = (esp_cam_sensor_format_t *)&cam_fmt_array.format_array[i];
        }
    }
    assert(cam_cur_fmt);  /* 确保找到目标格式 */
    /* 应用选定的格式 */
    ESP_RETURN_ON_ERROR(esp_cam_sensor_set_format(cam, cam_cur_fmt), TAG, "设置格式失败");
    ESP_LOGI(TAG, "当前格式: %s", cam_cur_fmt->name);

    /* ========== 步骤7: 启动传感器数据流 ========== */
    int enable_flag = 1;
    ESP_RETURN_ON_ERROR(esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_STREAM, &enable_flag),
                        TAG, "传感器数据流启动失败");

    /* ========== 步骤8: 初始化CSI控制器 ========== */
    /* CSI(Camera Serial Interface)用于接收MIPI摄像头数据 */
    /* 注意: v1.0芯片CSI仅支持bypass模式，无法进行格式转换 */
    esp_cam_ctlr_csi_config_t csi_config = {
        .ctlr_id = 0,                                    /* 控制器ID */
        .h_res = CAM_H_RES,                              /* 水平分辨率 */
        .v_res = CAM_V_RES,                              /* 垂直分辨率 */
        .lane_bit_rate_mbps = EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS,  /* 数据速率(Mbps) */
        .input_data_color_type = CAM_CTLR_COLOR_RAW8,    /* 输入数据格式: RAW8 */
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,   /* 输出数据格式: RAW8(bypass) */
        .data_lane_num = 2,                              /* 数据通道数 */
        .byte_swap_en = false,                           /* 禁用字节交换 */
        .queue_items = 1,                                /* 传输队列深度 */
    };
    ret = esp_cam_new_csi_ctlr(&csi_config, cam_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI初始化失败[%d]", ret);
        return ret;
    }

    /* 注册回调函数 */
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_get_new_trans = s_camera_get_new_vb,         /* 新帧请求回调 */
        .on_trans_finished = s_camera_get_finished_trans, /* 帧完成回调 */
    };
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(*cam_handle, &cbs, trans),
                        TAG, "注册回调失败");

    /* 使能CSI控制器 */
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(*cam_handle), TAG, "CSI使能失败");

    /* ========== 步骤9: 初始化ISP图像信号处理器 ========== */
    /* ISP负责将RAW8格式转换为RGB565格式 */
    static isp_proc_handle_t s_isp_proc = NULL;  /* 全局保存，用于 deinit */
    esp_isp_processor_cfg_t isp_config = {
        .clk_hz = 80 * 1000 * 1000,                      /* ISP时钟: 80MHz */
        .input_data_source = ISP_INPUT_DATA_SOURCE_CSI,   /* 数据源: CSI */
        .input_data_color_type = ISP_COLOR_RAW8,          /* 输入格式: RAW8 */
        .output_data_color_type = ISP_COLOR_RGB565,       /* 输出格式: RGB565 */
        .has_line_start_packet = false,                    /* 无行起始包 */
        .has_line_end_packet = false,                      /* 无行结束包 */
        .h_res = CAM_H_RES,                               /* 水平分辨率 */
        .v_res = CAM_V_RES,                               /* 垂直分辨率 */
        .bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG,      /* Bayer模式排列顺序 */
    };
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_config, &s_isp_proc), TAG, "ISP创建失败");
    ESP_RETURN_ON_ERROR(esp_isp_enable(s_isp_proc), TAG, "ISP使能失败");

    /* ========== 步骤10: 配置去马赛克(Demosaic)模块 ========== */
    /* 去马赛克是将Bayer格式转换为全彩色图像的关键步骤 */
    /* 当CSI输出RAW8格式时必须启用(v1.0芯片) */
    esp_isp_demosaic_config_t demosaic_config = {
        .grad_ratio = {
            .integer = 2,    /* 梯度比整数部分 */
            .decimal = 5,    /* 梯度比小数部分(实际值2.5) */
        },
    };
    ESP_RETURN_ON_ERROR(esp_isp_demosaic_configure(s_isp_proc, &demosaic_config), TAG, "去马赛克配置失败");
    ESP_RETURN_ON_ERROR(esp_isp_demosaic_enable(s_isp_proc), TAG, "去马赛克使能失败");

    ESP_LOGI(TAG, "摄像头初始化完成 (%dx%d RAW8 -> RGB565)", CAM_H_RES, CAM_V_RES);
    return ESP_OK;
}

/**
 * @brief 启动摄像头数据采集
 * @param cam_handle CSI控制器句柄(由camera_init返回)
 * @return ESP_OK 成功，其他值表示失败
 *
 * 调用此函数后，摄像头开始输出帧数据，
 * 帧数据通过双缓冲机制和信号量传递给显示任务
 */
esp_err_t camera_start(esp_cam_ctlr_handle_t cam_handle)
{
    return esp_cam_ctlr_start(cam_handle);
}

/**
 * @brief 释放 ISP 资源，深度睡眠前必须调用
 */
void camera_deinit(void)
{
    if (s_isp_proc) {
        ESP_LOGI(TAG, "Disabling ISP...");
        esp_isp_demosaic_disable(s_isp_proc);
        esp_isp_disable(s_isp_proc);
        esp_isp_del_processor(s_isp_proc);
        s_isp_proc = NULL;
        ESP_LOGI(TAG, "ISP disabled and deleted");
    }
}
