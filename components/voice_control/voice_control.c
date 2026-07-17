/*
 * Voice Control Component Implementation
 * ESP-SR based speech recognition for vending machine
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

/* ESP-SR headers */
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_process_sdkconfig.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"

/* Board init */
#include "esp_board_init.h"

/* Voice control API */
#include "voice_control.h"

static const char *TAG = "voice_ctrl";

/* ── 内部状态 ── */
static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t *s_afe_data = NULL;
static srmodel_list_t *s_models = NULL;
static volatile int s_task_flag = 0;
static TaskHandle_t s_feed_task = NULL;
static TaskHandle_t s_detect_task = NULL;

/* ── 回调 ── */
static voice_cmd_cb_t s_voice_cb = NULL;
static void *s_voice_cb_user_data = NULL;

/* ── 商品名称映射 ── */
static const char *get_product_name(int cmd_id)
{
    if (cmd_id >= VOICE_CMD_COLA_1 && cmd_id <= VOICE_CMD_COLA_3) {
        return "Cola";
    } else if (cmd_id >= VOICE_CMD_SPRITE_1 && cmd_id <= VOICE_CMD_SPRITE_3) {
        return "Sprite";
    } else if (cmd_id >= VOICE_CMD_WATER_1 && cmd_id <= VOICE_CMD_WATER_6) {
        return "Water";
    } else if (cmd_id >= VOICE_CMD_CHIPS_1 && cmd_id <= VOICE_CMD_CHIPS_3) {
        return "Chips";
    }
    return "Unknown";
}

/* ===================================================================
 * Feed 任务：从麦克风读取音频数据并送入 AFE
 * =================================================================== */
static void feed_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int audio_chunksize = s_afe_handle->get_feed_chunksize(afe_data);
    int nch = s_afe_handle->get_feed_channel_num(afe_data);
    int feed_channel = esp_get_feed_channel();

    ESP_LOGI(TAG, "Feed task: chunksize=%d, nch=%d, feed_channel=%d",
             audio_chunksize, nch, feed_channel);

    assert(nch == feed_channel);

    int16_t *i2s_buff = malloc(audio_chunksize * sizeof(int16_t) * feed_channel);
    assert(i2s_buff);

    while (s_task_flag) {
        esp_get_feed_data(true, i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);
        s_afe_handle->feed(afe_data, i2s_buff);
    }

    free(i2s_buff);
    ESP_LOGI(TAG, "Feed task exiting");
    vTaskDelete(NULL);
}

/* ===================================================================
 * Detect 任务：从 AFE 获取处理后的音频并执行语音识别
 * =================================================================== */
static void detect_task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = s_afe_handle->get_fetch_chunksize(afe_data);

    /* 获取中文 MultiNet 模型 */
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (mn_name == NULL) {
        ESP_LOGE(TAG, "No Chinese MultiNet model found!");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "MultiNet model: %s", mn_name);

    /* 创建 MultiNet 实例 */
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);

    /* 运行时加载自定义售卖机语音指令 */
    esp_mn_commands_alloc(multinet, model_data);

    /* 可乐 */
    esp_mn_commands_add(1, "wo xiang yao ke le");
    esp_mn_commands_add(2, "wo yao ke le");
    esp_mn_commands_add(3, "ke le");
    /* 雪碧 */
    esp_mn_commands_add(4, "wo xiang yao xue bi");
    esp_mn_commands_add(5, "wo yao xue bi");
    esp_mn_commands_add(6, "xue bi");
    /* 水 */
    esp_mn_commands_add(7, "wo xiang yao shui");
    esp_mn_commands_add(8, "wo yao shui");
    esp_mn_commands_add(9, "shui");
    esp_mn_commands_add(10, "wo xiang yao kuang quan shui");
    esp_mn_commands_add(11, "wo yao kuang quan shui");
    esp_mn_commands_add(12, "kuang quan shui");
    /* 薯片 */
    esp_mn_commands_add(13, "wo xiang yao shu pian");
    esp_mn_commands_add(14, "wo yao shu pian");
    esp_mn_commands_add(15, "shu pian");

    esp_mn_error_t *mn_err = esp_mn_commands_update();
    if (mn_err) {
        ESP_LOGE(TAG, "Commands update failed: %d errors", mn_err->num);
        for (int i = 0; i < mn_err->num && i < 5; i++) {
            ESP_LOGE(TAG, "  Error %d: cmd=%d str=%s", i,
                     mn_err->phrases[i]->command_id,
                     mn_err->phrases[i]->string ? mn_err->phrases[i]->string : "null");
        }
    }

    int mu_chunksize = multinet->get_samp_chunksize(model_data);
    assert(mu_chunksize == afe_chunksize);

    /* 打印激活的语音命令 */
    multinet->print_active_speech_commands(model_data);

    /* 禁用唤醒词，直接监听命令 */
    s_afe_handle->disable_wakenet(afe_data);

    ESP_LOGI(TAG, "Voice detection started (no wake word)");

    while (s_task_flag) {
        afe_fetch_result_t *res = s_afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGE(TAG, "AFE fetch error!");
            break;
        }

        esp_mn_state_t mn_state = multinet->detect(model_data, res->data);

        if (mn_state == ESP_MN_STATE_DETECTING) {
            continue;
        }

        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_result = multinet->get_results(model_data);
            for (int i = 0; i < mn_result->num; i++) {
                ESP_LOGI(TAG, "Command %d: id=%d prob=%.2f string=%s",
                         i + 1, mn_result->command_id[i],
                         mn_result->prob[i], mn_result->string);
            }

            /* 调用回调 */
            if (mn_result->num > 0 && s_voice_cb) {
                int cmd_id = mn_result->command_id[0];
                const char *product = get_product_name(cmd_id);
                ESP_LOGI(TAG, "Voice command: %s (id=%d)", product, cmd_id);
                s_voice_cb(cmd_id, product, s_voice_cb_user_data);
            }

            multinet->clean(model_data);
        }

        if (mn_state == ESP_MN_STATE_TIMEOUT) {
            multinet->clean(model_data);
        }
    }

    if (model_data) {
        multinet->destroy(model_data);
    }
    ESP_LOGI(TAG, "Detect task exiting");
    vTaskDelete(NULL);
}

/* ===================================================================
 * 公共 API
 * =================================================================== */

esp_err_t voice_control_init(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing voice control...");

    /* 初始化 SR 模型 */
    ESP_LOGI(TAG, "Loading SR models...");
    s_models = esp_srmodel_init("model");
    if (s_models == NULL) {
        ESP_LOGE(TAG, "Failed to load SR models! Check partition table.");
        return ESP_FAIL;
    }

    /* 初始化音频硬件 */
    ESP_LOGI(TAG, "Initializing audio hardware...");
    esp_err_t ret = esp_board_init(16000, 1, 16, i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Audio board init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 创建 AFE */
    ESP_LOGI(TAG, "Creating Audio Front-End...");
    const char *input_format = esp_get_input_format();
    afe_config_t *afe_config = afe_config_init(input_format, s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    s_afe_handle = esp_afe_handle_from_config(afe_config);
    s_afe_data = s_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "Failed to create AFE!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Voice control initialized");
    return ESP_OK;
}

esp_err_t voice_control_start(void)
{
    if (s_afe_data == NULL) {
        ESP_LOGE(TAG, "Voice control not initialized!");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_task_flag) {
        ESP_LOGW(TAG, "Voice control already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting voice detection tasks...");
    s_task_flag = 1;

    /* 创建检测任务（优先级较高） */
    xTaskCreatePinnedToCore(&detect_task, "voice_detect", 8 * 1024,
                            (void *)s_afe_data, 5, &s_detect_task, 1);

    /* 创建音频采集任务 */
    xTaskCreatePinnedToCore(&feed_task, "voice_feed", 8 * 1024,
                            (void *)s_afe_data, 5, &s_feed_task, 0);

    ESP_LOGI(TAG, "Voice detection started");
    return ESP_OK;
}

esp_err_t voice_control_stop(void)
{
    if (!s_task_flag) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping voice detection...");
    s_task_flag = 0;

    /* 等待任务自行退出（任务会调用 vTaskDelete(NULL)） */
    vTaskDelay(pdMS_TO_TICKS(1000));

    s_feed_task = NULL;
    s_detect_task = NULL;

    /* 销毁 AFE */
    if (s_afe_data && s_afe_handle) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
    }

    /* 等待 AFE 资源完全释放 */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 释放音频硬件资源（I2S、I2C 设备、编解码器） */
    esp_board_deinit();

    ESP_LOGI(TAG, "Voice control stopped");
    return ESP_OK;
}

void voice_control_set_cb(voice_cmd_cb_t cb, void *user_data)
{
    s_voice_cb = cb;
    s_voice_cb_user_data = user_data;
}

int voice_cmd_to_product_index(int cmd_id)
{
    if (cmd_id >= VOICE_CMD_COLA_1 && cmd_id <= VOICE_CMD_COLA_3) {
        return 0;  /* Cola */
    } else if (cmd_id >= VOICE_CMD_SPRITE_1 && cmd_id <= VOICE_CMD_SPRITE_3) {
        return 1;  /* Sprite */
    } else if (cmd_id >= VOICE_CMD_WATER_1 && cmd_id <= VOICE_CMD_WATER_6) {
        return 2;  /* Water */
    } else if (cmd_id >= VOICE_CMD_CHIPS_1 && cmd_id <= VOICE_CMD_CHIPS_3) {
        return 3;  /* Chips */
    }
    return -1;  /* Unknown */
}
