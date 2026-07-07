/*
 * Voice Control Component
 * ESP-SR based speech recognition for vending machine
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 语音命令 ID 定义
 *        与 my_commands_cn.txt 中的命令 ID 对应
 */
typedef enum {
    VOICE_CMD_COLA_1 = 1,      /* wo xiang yao ke le */
    VOICE_CMD_COLA_2 = 2,      /* wo yao ke le */
    VOICE_CMD_COLA_3 = 3,      /* ke le */
    VOICE_CMD_SPRITE_1 = 4,    /* wo xiang yao xue bi */
    VOICE_CMD_SPRITE_2 = 5,    /* wo yao xue bi */
    VOICE_CMD_SPRITE_3 = 6,    /* xue bi */
    VOICE_CMD_WATER_1 = 7,     /* wo xiang yao shui */
    VOICE_CMD_WATER_2 = 8,     /* wo yao shui */
    VOICE_CMD_WATER_3 = 9,     /* shui */
    VOICE_CMD_WATER_4 = 10,    /* wo xiang yao kuang quan shui */
    VOICE_CMD_WATER_5 = 11,    /* wo yao kuang quan shui */
    VOICE_CMD_WATER_6 = 12,    /* kuang quan shui */
    VOICE_CMD_CHIPS_1 = 13,    /* wo xiang yao shu pian */
    VOICE_CMD_CHIPS_2 = 14,    /* wo yao shu pian */
    VOICE_CMD_CHIPS_3 = 15,    /* shu pian */
} voice_cmd_id_t;

/**
 * @brief 语音命令回调函数类型
 *
 * @param cmd_id    检测到的命令 ID
 * @param product   对应的商品名称（如 "Cola", "Water"）
 * @param user_data 用户自定义数据
 */
typedef void (*voice_cmd_cb_t)(int cmd_id, const char *product, void *user_data);

/**
 * @brief 初始化语音控制组件
 *        初始化 ESP-SR 模型和音频硬件（I2S + ES8311 + ES7210）
 *
 * @param i2c_bus   外部传入的 I2C 总线句柄（与摄像头共享），NULL 则创建新总线
 * @return ESP_OK 成功
 */
esp_err_t voice_control_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief 启动语音监听
 *        创建 feed 和 detect 任务，开始识别语音命令
 *
 * @return ESP_OK 成功
 */
esp_err_t voice_control_start(void);

/**
 * @brief 停止语音监听
 *        停止 feed 和 detect 任务，释放资源
 *
 * @return ESP_OK 成功
 */
esp_err_t voice_control_stop(void);

/**
 * @brief 设置语音命令回调
 *        当检测到语音命令时调用此回调
 *
 * @param cb        回调函数
 * @param user_data 用户自定义数据
 */
void voice_control_set_cb(voice_cmd_cb_t cb, void *user_data);

/**
 * @brief 将命令 ID 映射到商品索引
 *
 * @param cmd_id 语音命令 ID
 * @return 商品索引（0-3），-1 表示未知命令
 */
int voice_cmd_to_product_index(int cmd_id);

#ifdef __cplusplus
}
#endif
