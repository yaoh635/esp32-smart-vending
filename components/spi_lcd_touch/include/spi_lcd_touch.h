/*
 * SPI LCD Touch Component
 * Reusable SPI LCD + Touch + LVGL integration for ESP-IDF
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "lvgl.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SPI LCD 触摸屏系统配置结构体
 *        所有引脚号和显示参数均可配置。
 *        用户可通过 Kconfig 覆盖默认值，也可直接填充此结构体。
 */
typedef struct {
    /* SPI 总线引脚 */
    int sclk_gpio;          /**< SPI 时钟引脚 */
    int mosi_gpio;          /**< SPI 主出从入引脚 */
    int miso_gpio;          /**< SPI 主入从出引脚 */

    /* LCD 面板引脚 */
    int lcd_dc_gpio;        /**< LCD 数据/命令选择引脚 */
    int lcd_cs_gpio;        /**< LCD 片选引脚 */
    int lcd_rst_gpio;       /**< LCD 复位引脚 */
    int lcd_bk_light_gpio;  /**< LCD 背光控制引脚 */

    /* 触摸控制器引脚 */
    int touch_cs_gpio;      /**< 触摸芯片片选引脚 */
    int touch_int_gpio;     /**< 触摸中断引脚 */

    /* LCD 显示参数 */
    uint32_t pixel_clock_hz; /**< 像素时钟频率 (Hz) */
    uint16_t h_res;          /**< 水平分辨率 (像素) */
    uint16_t v_res;          /**< 垂直分辨率 (像素) */

    /* LVGL 参数 */
    uint16_t draw_buf_lines; /**< 绘图缓冲区行数 */
    uint16_t tick_period_ms; /**< LVGL 心跳周期 (毫秒) */
    uint32_t task_stack_size; /**< LVGL 任务栈大小 (字节) */
    BaseType_t task_priority; /**< LVGL 任务优先级 */

    /* 触摸功能标志 */
    bool touch_enabled;      /**< 是否启用触摸功能 */
    bool mirror_y;           /**< 是否镜像 Y 轴坐标 */
} spi_lcd_touch_config_t;

/**
 * @brief 从 Kconfig 获取默认配置
 *        用户可调用此函数获取默认配置，然后按需修改个别字段。
 */
spi_lcd_touch_config_t spi_lcd_touch_get_default_config(void);

/**
 * @brief 初始化 SPI 总线、LCD 面板、触摸控制器和 LVGL
 *        此函数不会启动 LVGL 任务，请在所有 LVGL UI 设置完成后
 *        调用 spi_lcd_touch_start_task() 启动。
 *
 * @param config  配置结构体指针（传 NULL 则使用 Kconfig 默认值）
 * @param out_display  输出参数，接收 LVGL 显示句柄
 * @return 成功返回 ESP_OK
 */
esp_err_t spi_lcd_touch_init(const spi_lcd_touch_config_t *config,
                             lv_display_t **out_display);

/**
 * @brief 启动 LVGL 任务
 *        必须在所有 LVGL UI 设置完成之后调用，
 *        以确保 UI 创建和 LVGL 渲染之间不会产生竞态条件。
 */
void spi_lcd_touch_start_task(void);

/**
 * @brief 获取 LVGL API 互斥锁
 *        在非 LVGL 任务中调用任何 LVGL API 之前必须先获取此锁。
 */
void spi_lcd_touch_lock(void);

/**
 * @brief 释放 LVGL API 互斥锁
 */
void spi_lcd_touch_unlock(void);

/**
 * @brief 释放 SPI 总线资源，允许重新初始化
 *        在售货机循环结束后、下次初始化前调用。
 */
void spi_lcd_touch_deinit(void);

#ifdef __cplusplus
}
#endif
