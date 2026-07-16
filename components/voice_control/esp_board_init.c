/*
 * ESP32-P4-Function-EV-Board Audio Initialization
 * Uses esp_codec_dev API for audio codec (ES8311 + ES7210)
 * Supports shared I2C bus with camera
 */

#include <string.h>
#include "esp_board_init.h"
#include "esp_log.h"
#include "esp_err.h"

/* FreeRTOS headers */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* I2S and I2C driver headers */
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

/* esp_codec_dev headers */
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "es7210_adc.h"

static const char *TAG = "audio_board";

#define I2S_PORT        (0)
#define I2C_PORT        (0)

/* I2S GPIO pins */
#define BSP_I2S_MCLK   (GPIO_NUM_13)
#define BSP_I2S_SCLK   (GPIO_NUM_12)
#define BSP_I2S_WS     (GPIO_NUM_10)
#define BSP_I2S_DOUT   (GPIO_NUM_9)
#define BSP_I2S_DIN    (GPIO_NUM_11)

/* I2C GPIO pins */
#define BSP_I2C_SDA    (GPIO_NUM_7)
#define BSP_I2C_SCL    (GPIO_NUM_8)

/* Power Amplifier control */
#define BSP_PA_CTRL    (GPIO_NUM_53)

/* Audio codec handles */
static esp_codec_dev_handle_t s_speaker_dev = NULL;
static esp_codec_dev_handle_t s_mic_dev = NULL;

/* I2S handles */
static i2s_chan_handle_t s_i2s_tx_chan = NULL;
static i2s_chan_handle_t s_i2s_rx_chan = NULL;
static bool s_i2s_enabled = false;  /* I2S 是否已启用 */

/* I2C handles */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static bool s_i2c_bus_owned = false;  /* 是否由本组件创建 */
static i2c_master_dev_handle_t s_es8311_dev = NULL;
static i2c_master_dev_handle_t s_es7210_dev = NULL;

/* Custom I2C control interfaces */
static audio_codec_ctrl_if_t s_es8311_ctrl;
static audio_codec_ctrl_if_t s_es7210_ctrl;

int esp_get_feed_channel(void)
{
    return 1;
}

const char *esp_get_input_format(void)
{
    return "M";
}

/* ===================================================================
 * Custom I2C control interface implementation
 * =================================================================== */
static int custom_i2c_open(const audio_codec_ctrl_if_t *ctrl, void *cfg, int cfg_size)
{
    return ESP_CODEC_DEV_OK;
}

static bool custom_i2c_is_open(const audio_codec_ctrl_if_t *ctrl)
{
    return true;
}

static int custom_i2c_read_reg(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len)
{
    i2c_master_dev_handle_t dev = (ctrl == &s_es8311_ctrl) ? s_es8311_dev : s_es7210_dev;
    if (dev == NULL) return ESP_CODEC_DEV_DRV_ERR;

    uint8_t reg_addr = (uint8_t)reg;
    esp_err_t ret = i2c_master_transmit_receive(dev, &reg_addr, 1, data, data_len, 1000);
    return (ret == ESP_OK) ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_DRV_ERR;
}

static int custom_i2c_write_reg(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len, void *data, int data_len)
{
    i2c_master_dev_handle_t dev = (ctrl == &s_es8311_ctrl) ? s_es8311_dev : s_es7210_dev;
    if (dev == NULL) return ESP_CODEC_DEV_DRV_ERR;

    uint8_t *buf = malloc(1 + data_len);
    if (buf == NULL) return ESP_CODEC_DEV_NO_MEM;

    buf[0] = (uint8_t)reg;
    memcpy(buf + 1, data, data_len);

    esp_err_t ret = i2c_master_transmit(dev, buf, 1 + data_len, 1000);
    free(buf);
    return (ret == ESP_OK) ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_DRV_ERR;
}

static int custom_i2c_close(const audio_codec_ctrl_if_t *ctrl)
{
    return ESP_CODEC_DEV_OK;
}

static void init_custom_i2c_ctrl(audio_codec_ctrl_if_t *ctrl)
{
    memset(ctrl, 0, sizeof(audio_codec_ctrl_if_t));
    ctrl->open = custom_i2c_open;
    ctrl->is_open = custom_i2c_is_open;
    ctrl->read_reg = custom_i2c_read_reg;
    ctrl->write_reg = custom_i2c_write_reg;
    ctrl->close = custom_i2c_close;
}

/* ===================================================================
 * 公共 API
 * =================================================================== */

esp_err_t esp_board_init(int sample_rate, int channels, int bits_per_sample,
                         i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing audio hardware");

    /* Enable Power Amplifier */
    gpio_reset_pin(BSP_PA_CTRL);
    gpio_set_direction(BSP_PA_CTRL, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_PA_CTRL, 1);
    ESP_LOGI(TAG, "PA enabled on GPIO%d", BSP_PA_CTRL);

    /* I2C 总线处理：使用外部传入的或创建新的 */
    if (i2c_bus != NULL) {
        s_i2c_bus = i2c_bus;
        s_i2c_bus_owned = false;
        ESP_LOGI(TAG, "Using shared I2C bus");
    } else {
        ESP_LOGI(TAG, "Creating new I2C bus: SDA=GPIO%d, SCL=GPIO%d", BSP_I2C_SDA, BSP_I2C_SCL);
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_PORT,
            .sda_io_num = BSP_I2C_SDA,
            .scl_io_num = BSP_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus creation failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_i2c_bus_owned = true;
    }

    /* 创建 I2C 设备句柄 */
    ESP_LOGI(TAG, "Adding audio codecs to I2C bus...");

    i2c_device_config_t es8311_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x18,
        .scl_speed_hz = 100000,
    };
    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &es8311_cfg, &s_es8311_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t es7210_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x36,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(s_i2c_bus, &es7210_cfg, &s_es7210_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 初始化 I2S（复用已存在的通道或创建新通道） */
    ESP_LOGI(TAG, "I2S: MCLK=GPIO%d, BCLK=GPIO%d, WS=GPIO%d, DOUT=GPIO%d, DIN=GPIO%d",
             BSP_I2S_MCLK, BSP_I2S_SCLK, BSP_I2S_WS, BSP_I2S_DOUT, BSP_I2S_DIN);

    if (s_i2s_tx_chan == NULL || s_i2s_rx_chan == NULL) {
        /* 首次初始化：创建新通道 */
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;
        ret = i2s_new_channel(&chan_cfg, &s_i2s_tx_chan, &s_i2s_rx_chan);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "I2S channel creation failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "I2S channels created");
    } else {
        ESP_LOGI(TAG, "Reusing existing I2S channels");
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            (bits_per_sample == 16) ? I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_24BIT,
            (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO
        ),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    ret = i2s_channel_init_std_mode(s_i2s_tx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_init_std_mode(s_i2s_rx_chan, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_i2s_tx_chan);
    if (ret != ESP_OK) return ret;

    ret = i2s_channel_enable(s_i2s_rx_chan);
    if (ret != ESP_OK) return ret;

    s_i2s_enabled = true;
    ESP_LOGI(TAG, "I2S enabled");
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 初始化 I2C 控制接口 */
    init_custom_i2c_ctrl(&s_es8311_ctrl);
    init_custom_i2c_ctrl(&s_es7210_ctrl);

    /* 创建 I2S 数据接口 */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_PORT,
        .rx_handle = (void *)s_i2s_rx_chan,
        .tx_handle = (void *)s_i2s_tx_chan,
    };
    const audio_codec_data_if_t *i2s_data = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (i2s_data == NULL) {
        ESP_LOGE(TAG, "I2S data interface creation failed");
        return ESP_FAIL;
    }

    /* 初始化 ES8311（扬声器） */
    es8311_codec_cfg_t es8311_codec_cfg = {
        .ctrl_if = &s_es8311_ctrl,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_PA_CTRL,
        .master_mode = false,
        .use_mclk = true,
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es8311_codec_cfg);
    if (es8311 != NULL) {
        esp_codec_dev_cfg_t spk_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT,
            .codec_if = es8311,
            .data_if = i2s_data,
        };
        s_speaker_dev = esp_codec_dev_new(&spk_cfg);
        if (s_speaker_dev != NULL) {
            esp_codec_dev_sample_info_t fs = {
                .sample_rate = sample_rate,
                .channel = channels,
                .bits_per_sample = bits_per_sample,
            };
            esp_codec_dev_open(s_speaker_dev, &fs);
            esp_codec_dev_set_out_vol(s_speaker_dev, 60);
            ESP_LOGI(TAG, "Speaker ready");
        }
    }

    /* 初始化 ES7210（麦克风） */
    es7210_codec_cfg_t es7210_codec_cfg = {
        .ctrl_if = &s_es7210_ctrl,
        .master_mode = false,
        .mic_selected = ES7210_SEL_MIC1,
        .mclk_src = ES7210_MCLK_FROM_PAD,
    };
    const audio_codec_if_t *es7210 = es7210_codec_new(&es7210_codec_cfg);
    if (es7210 == NULL) {
        ESP_LOGE(TAG, "ES7210 creation failed");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t mic_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210,
        .data_if = i2s_data,
    };
    s_mic_dev = esp_codec_dev_new(&mic_cfg);
    if (s_mic_dev != NULL) {
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = sample_rate,
            .channel = channels,
            .bits_per_sample = bits_per_sample,
        };
        esp_codec_dev_open(s_mic_dev, &fs);
        esp_codec_dev_set_in_gain(s_mic_dev, 20);
        ESP_LOGI(TAG, "Microphone ready (gain: 20dB)");
    } else {
        ESP_LOGE(TAG, "Microphone creation failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Audio hardware initialized");
    return ESP_OK;
}

esp_err_t esp_board_deinit(void)
{
    if (!s_i2s_enabled) {
        ESP_LOGW(TAG, "Audio hardware not initialized, skipping deinit");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing audio hardware...");

    /* 关闭并释放音频编解码器设备 */
    if (s_speaker_dev) {
        esp_codec_dev_close(s_speaker_dev);
        esp_codec_dev_delete(s_speaker_dev);
        s_speaker_dev = NULL;
        ESP_LOGI(TAG, "Speaker device released");
    }
    if (s_mic_dev) {
        esp_codec_dev_close(s_mic_dev);
        esp_codec_dev_delete(s_mic_dev);
        s_mic_dev = NULL;
        ESP_LOGI(TAG, "Microphone device released");
    }

    /* 禁用 I2S 通道（不删除，避免驱动内部状态不一致） */
    if (s_i2s_tx_chan) {
        i2s_channel_disable(s_i2s_tx_chan);
        ESP_LOGI(TAG, "I2S TX channel disabled");
    }
    if (s_i2s_rx_chan) {
        i2s_channel_disable(s_i2s_rx_chan);
        ESP_LOGI(TAG, "I2S RX channel disabled");
    }

    /* 释放 I2C 设备句柄（不释放共享的 I2C 总线） */
    if (s_es8311_dev) {
        i2c_master_bus_rm_device(s_es8311_dev);
        s_es8311_dev = NULL;
    }
    if (s_es7210_dev) {
        i2c_master_bus_rm_device(s_es7210_dev);
        s_es7210_dev = NULL;
    }

    /* 关闭 PA */
    gpio_set_level(BSP_PA_CTRL, 0);

    s_i2s_enabled = false;
    ESP_LOGI(TAG, "Audio hardware deinitialized");
    return ESP_OK;
}

esp_err_t esp_get_feed_data(bool enable, int16_t *buffer, int length)
{
    if (s_mic_dev == NULL) return ESP_ERR_INVALID_STATE;
    if (!enable) return ESP_OK;

    esp_err_t ret = esp_codec_dev_read(s_mic_dev, buffer, length);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Mic read failed: %d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_audio_play(const int16_t *data, int length, uint32_t timeout)
{
    if (s_speaker_dev == NULL) return ESP_ERR_INVALID_STATE;

    esp_err_t ret = esp_codec_dev_write(s_speaker_dev, data, length);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Speaker write failed: %d", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}
