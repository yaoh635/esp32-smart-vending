/*
 * Vending Machine UI Component
 * Reusable vending machine interface for ESP32 SPI LCD Touch
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "lvgl.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 电机控制默认 GPIO 引脚 */
#ifndef VENDING_MOTOR_GPIO
#define VENDING_MOTOR_GPIO  7
#endif

/* 舵机控制 GPIO 引脚（每个商品对应一个舵机） */
#ifndef VENDING_SERVO_COUNT
#define VENDING_SERVO_COUNT  4
#endif

/**
 * @brief 自动售货机状态枚举
 */
typedef enum {
    STATE_PRODUCT_SELECT,      /**< 商品选择界面 */
    STATE_ORDER_CONFIRM,       /**< 订单确认界面 */
    STATE_PAYMENT,             /**< 支付处理界面 */
    STATE_DISPENSING,          /**< 出货中界面 */
    STATE_DONE,                /**< 完成/感谢界面 */
    STATE_VOICE_LISTENING,     /**< 语音交互界面（预留） */
} vending_state_t;

/**
 * @brief 商品定义结构体
 */
typedef struct {
    const char *name;        /**< 商品名称 */
    const char *price_text;  /**< 价格文本（如 "$2.00"） */
    lv_color_t color;        /**< 商品卡片主题颜色 */
    const char *symbol;      /**< LVGL 图标符号 */
} product_t;

/* 默认商品数量 */
#define VENDING_PRODUCT_COUNT  4

/**
 * @brief 购买完成回调函数类型
 *        在出货完成时调用，用于记录购买信息。
 *
 * @param product_name  商品名称
 * @param price_text    价格文本
 * @param user_data     用户自定义数据
 */
typedef void (*vending_purchase_cb_t)(const char *product_name,
                                      const char *price_text,
                                      void *user_data);

/**
 * @brief 自动售货机 UI 配置结构体
 */
typedef struct {
    int motor_gpio;                  /**< 电机控制 GPIO 引脚（-1 表示禁用电机） */
    int servo_gpios[VENDING_SERVO_COUNT]; /**< 舵机 GPIO 引脚数组（每个商品对应一个舵机，-1 表示禁用） */
    const product_t *products;       /**< 商品目录数组（NULL 使用默认商品） */
    uint8_t product_count;           /**< 商品数量 */
    vending_purchase_cb_t purchase_cb; /**< 购买完成回调（NULL 不回调） */
    void *purchase_cb_user_data;     /**< 回调用户数据 */
} vending_machine_config_t;

/**
 * @brief 获取自动售货机默认配置
 *        返回包含默认电机 GPIO 和默认商品目录的配置。
 */
vending_machine_config_t vending_machine_get_default_config(void);

/**
 * @brief 初始化并启动自动售货机 UI
 *        在指定的 LVGL 显示屏上创建完整的售货机交互界面。
 *
 * @param display  LVGL 显示句柄（从 spi_lcd_touch_init 获取）
 * @param config   配置结构体（传 NULL 使用默认配置）
 * @return 成功返回 ESP_OK
 */
esp_err_t vending_machine_start(lv_display_t *display,
                                const vending_machine_config_t *config);

/**
 * @brief 获取当前售货机状态
 * @return 当前状态枚举值
 */
vending_state_t vending_machine_get_state(void);

/**
 * @brief 停止售货机 UI，禁用所有定时器回调
 *        在销毁 LCD 之前必须调用，防止定时器访问已销毁对象。
 */
void vending_machine_stop(void);

#ifdef __cplusplus
}
#endif
