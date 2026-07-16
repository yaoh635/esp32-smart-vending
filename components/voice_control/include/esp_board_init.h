/*
 * ESP32-P4-Function-EV-Board Audio Initialization
 * Using esp_codec_dev API for audio codec (ES8311 + ES7210)
 */

#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化音频硬件
 *        初始化 I2S + ES8311（扬声器）+ ES7210（麦克风）
 *
 * @param sample_rate    采样率（如 16000）
 * @param channels       通道数（1 = 单声道）
 * @param bits_per_sample 每样本位数（16）
 * @param i2c_bus        外部 I2C 总线句柄（与摄像头共享），NULL 则创建新总线
 * @return ESP_OK 成功
 */
esp_err_t esp_board_init(int sample_rate, int channels, int bits_per_sample,
                         i2c_master_bus_handle_t i2c_bus);

/**
 * @brief 释放音频硬件资源
 *        释放 I2S 通道、I2C 设备句柄、音频编解码器设备
 * @return ESP_OK 成功
 */
esp_err_t esp_board_deinit(void);

/**
 * @brief 获取音频采集通道数
 */
int esp_get_feed_channel(void);

/**
 * @brief 获取 AFE 输入格式字符串
 */
const char *esp_get_input_format(void);

/**
 * @brief 从麦克风读取音频数据
 *
 * @param enable 使能标志
 * @param buffer 数据缓冲区
 * @param length 数据长度（字节）
 * @return ESP_OK 成功
 */
esp_err_t esp_get_feed_data(bool enable, int16_t *buffer, int length);

/**
 * @brief 通过扬声器播放音频数据
 *
 * @param data    音频数据
 * @param length  数据长度（字节）
 * @param timeout 超时（ticks）
 * @return ESP_OK 成功
 */
esp_err_t esp_audio_play(const int16_t *data, int length, uint32_t timeout);

#ifdef __cplusplus
}
#endif
