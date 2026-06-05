/*
 * Smart Vending Machine - Main Application
 *
 * Flow: Camera (OV5647) → Face Detection → LCD + Touch → Vending UI → SD Card Log
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <cstdio>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

/* Camera & Frame Capture */
#include "who_ov5647_cam.hpp"
#include "who_frame_cap.hpp"
#include "camera.h"

/* Face Detection + Recognition */
#include "who_detect.hpp"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"

/* SPI LCD + Touch + LVGL */
#include "spi_lcd_touch.h"

/* Vending Machine UI */
#include "vending_machine.h"

/* Face ID & SD Card Logging */
#include "face_id_manager.h"

static const char *TAG = "main";

/* ── 全局管线对象（用于暂停/恢复） ── */
static who::frame_cap::WhoFrameCap *g_frame_cap = NULL;
static who::detect::WhoDetect *g_detector = NULL;
static HumanFaceRecognizer *g_face_recognizer = NULL;  /* 人脸识别器 */

/* ── 同步原语 ── */
static SemaphoreHandle_t s_face_detected_sem = NULL;
static volatile bool s_vending_active = false;
static TaskHandle_t s_vending_ui_task_handle = NULL;   /* 用于通知购买完成 */
static TaskHandle_t s_start_vending_task_handle = NULL; /* 用于通知 UI 任务退出 */

/* ── 检测参数 ── */
#define DETECT_FPS          5.0f
#define DETECT_STACK_SIZE   (32 * 1024)  /* 识别推理需要更大栈空间 */
#define DETECT_PRIORITY     5
#define CAP_STACK_SIZE      (4 * 1024)
#define CAP_PRIORITY        6

/* ── 深度睡眠参数 ── */
#define IDLE_TIMEOUT_SEC    20          /* 无人脸 30 秒后进入深度睡眠 */
#define TOUCH_WAKE_GPIO     2           /* 触摸中断引脚 (LP GPIO 2) */

/* ── 空闲计时器 ── */
static int64_t s_last_face_time_us = 0;  /* 上次检测到人脸的时间戳 */

/* Forward declarations */
static void on_purchase_done(const char *product_name, const char *price_text, void *user_data);

/* ===================================================================
 * 人脸检测回调（同步执行识别 — 同一帧）
 * =================================================================== */
static void on_face_detected(const who::detect::WhoDetect::result_t &result)
{
    int face_count = result.det_res.size();

    if (face_count > 0 && !s_vending_active) {
        /* 重置空闲计时器 */
        s_last_face_time_us = esp_timer_get_time();
        /* 在当前帧同步执行人脸识别 */
        if (g_face_recognizer) {
            int db_count = g_face_recognizer->get_num_feats();
            ESP_LOGI(TAG, "Recognition: %d faces in DB, running recognize...", db_count);

            auto recog_result = g_face_recognizer->recognize(result.img, result.det_res);

            if (!recog_result.empty()) {
                /* 识别成功 — 匹配到已知人脸 */
                int feat_id = recog_result[0].id;
                float sim = recog_result[0].similarity;
                int user_id = face_id_find_or_create(feat_id);
                if (user_id > 0) {
                    face_id_set_current_user(user_id);
                    ESP_LOGI(TAG, "✓ Face recognized: feat_id=%d sim=%.2f → user_id=%d", feat_id, sim, user_id);
                }
            } else {
                /* 未知人脸 — 注册新用户并入库特征 */
                ESP_LOGI(TAG, "✗ Unknown face (DB had %d feats), enrolling...", db_count);
                int user_id = face_id_register(NULL);
                if (user_id > 0) {
                    face_id_set_current_user(user_id);
                    /* 将人脸特征注册到数据库 */
                    esp_err_t err = g_face_recognizer->enroll(result.img, result.det_res);
                    int new_count = g_face_recognizer->get_num_feats();
                    if (err == ESP_OK) {
                        ESP_LOGI(TAG, "✓ New face enrolled: user_id=%d, DB: %d → %d feats",
                                 user_id, db_count, new_count);
                    } else {
                        ESP_LOGE(TAG, "✗ Feature enroll FAILED (err=0x%x), DB still has %d feats", err, new_count);
                    }
                }
            }
        }

        ESP_LOGI(TAG, "Triggering vending UI");
        xSemaphoreGive(s_face_detected_sem);
    }
}

/* ===================================================================
 * 售货机 UI 任务
 * =================================================================== */
static void vending_ui_task(void *arg)
{
    lv_display_t *display = (lv_display_t *)arg;

    /* 保存任务句柄，供 on_purchase_done 通过通知唤醒 */
    s_vending_ui_task_handle = xTaskGetCurrentTaskHandle();

    /* 获取 LVGL 锁并启动售货机 */
    spi_lcd_touch_lock();

    vending_machine_config_t config = vending_machine_get_default_config();
    config.motor_gpio = VENDING_MOTOR_GPIO;
    config.purchase_cb = on_purchase_done;
    config.purchase_cb_user_data = NULL;

    esp_err_t ret = vending_machine_start(display, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start vending machine UI");
        spi_lcd_touch_unlock();
        s_vending_ui_task_handle = NULL;
        s_vending_active = false;
        /* 通知 start_vending_machine 可以退出 */
        if (s_start_vending_task_handle) {
            xTaskNotifyGive(s_start_vending_task_handle);
        }
        vTaskDelete(NULL);
        return;
    }

    spi_lcd_touch_unlock();

    /* 启动 LVGL 任务 */
    spi_lcd_touch_start_task();

    ESP_LOGI(TAG, "Vending machine UI running, waiting for purchase...");

    /* 阻塞等待购买完成通知（由 on_purchase_done 回调发送） */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    /* 购买完成后额外等待几秒显示感谢界面 */
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Vending cycle complete");

    s_vending_ui_task_handle = NULL;
    s_vending_active = false;

    /* 通知 start_vending_machine 可以退出 */
    if (s_start_vending_task_handle) {
        xTaskNotifyGive(s_start_vending_task_handle);
    }

    vTaskDelete(NULL);
}

/* ===================================================================
 * 购买完成回调（在出货完成时触发）
 * =================================================================== */
static void on_purchase_done(const char *product_name, const char *price_text, void *user_data)
{
    (void)user_data;
    ESP_LOGI(TAG, ">>> on_purchase_done called: %s %s (user_id=%d) <<<",
             product_name, price_text, face_id_get_current_user());

    /* 记录到 SD 卡 */
    face_id_purchase_callback(product_name, price_text, user_data);
    ESP_LOGI(TAG, ">>> Purchase callback done <<<");

    /* 通知 vending_ui_task 可以退出（非阻塞，可在任意上下文调用） */
    if (s_vending_ui_task_handle) {
        xTaskNotifyGive(s_vending_ui_task_handle);
    }
}

/* ===================================================================
 * 启动售货机流程（暂停检测 → 初始化 LCD → 启动 UI）
 * =================================================================== */
static void start_vending_machine(void)
{
    if (s_vending_active) return;
    s_vending_active = true;

    ESP_LOGI(TAG, "=== Starting vending machine flow ===");

    /* 1. 暂停人脸检测管线，释放 CPU */
    if (g_detector) {
        g_detector->pause_async();
        g_detector->wait_for_paused(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "Face detector paused");
    }
    if (g_frame_cap) {
        g_frame_cap->pause();
        ESP_LOGI(TAG, "Frame capture paused");
    }

    /* 2. 初始化 SPI LCD + Touch */
    ESP_LOGI(TAG, "Initializing SPI LCD + Touch...");
    lv_display_t *display = NULL;
    spi_lcd_touch_config_t lcd_cfg = spi_lcd_touch_get_default_config();
    lcd_cfg.touch_enabled = true;

    esp_err_t ret = spi_lcd_touch_init(&lcd_cfg, &display);
    if (ret != ESP_OK || display == NULL) {
        ESP_LOGE(TAG, "Failed to init SPI LCD: %s", esp_err_to_name(ret));
        s_vending_active = false;
        /* LCD 失败，恢复检测 */
        if (g_frame_cap) g_frame_cap->resume();
        if (g_detector) g_detector->resume();
        return;
    }
    ESP_LOGI(TAG, "SPI LCD ready");

    /* 3. 用户已在识别回调中设置，直接获取 */
    int user_id = face_id_get_current_user();
    if (user_id > 0) {
        ESP_LOGI(TAG, "Current user ID %d (from recognition)", user_id);
    } else {
        ESP_LOGW(TAG, "No user set (recognition may have failed)");
    }

    /* 4. 启动售货机 UI 任务 */
    s_start_vending_task_handle = xTaskGetCurrentTaskHandle();
    xTaskCreate(vending_ui_task, "vending_ui", 16 * 1024, display, 4, NULL);

    /* 5. 阻塞等待 UI 任务完成通知 */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_start_vending_task_handle = NULL;

    /* 6. 关闭 LCD 背光并释放 SPI 总线 */
    ESP_LOGI(TAG, "Turning off LCD backlight...");
    gpio_config_t bk_cfg = {};
    bk_cfg.mode = GPIO_MODE_OUTPUT;
    bk_cfg.pin_bit_mask = 1ULL << CONFIG_SPI_LCD_TOUCH_BK_LIGHT_GPIO;
    gpio_config(&bk_cfg);
    gpio_set_level((gpio_num_t)CONFIG_SPI_LCD_TOUCH_BK_LIGHT_GPIO, 0);
    spi_lcd_touch_deinit();

    ESP_LOGI(TAG, "Resuming face detection pipeline...");

    /* 7. 恢复检测管线 */
    if (g_frame_cap) g_frame_cap->resume();
    if (g_detector) g_detector->resume();

    ESP_LOGI(TAG, "=== Face detection resumed ===");
}

/* ===================================================================
 * 进入深度睡眠
 * =================================================================== */
static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "=== Entering deep sleep ===");

    /* 1. 停止摄像头和检测管线（释放硬件资源） */
    ESP_LOGI(TAG, "Stopping camera pipeline...");
    if (g_detector) {
        g_detector->stop();
    }
    if (g_frame_cap) {
        g_frame_cap->stop();
    }
    g_detector = NULL;
    g_frame_cap = NULL;
    camera_deinit();  /* 释放 ISP 硬件 */
    vTaskDelay(pdMS_TO_TICKS(500));  /* 等待硬件完全释放 */

    /* 2. 配置电源域：确保 RTC 域在深度睡眠时保持供电 */
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    /* 3. 关闭 LCD 背光 */
    gpio_config_t bk_cfg = {};
    bk_cfg.mode = GPIO_MODE_OUTPUT;
    bk_cfg.pin_bit_mask = 1ULL << CONFIG_SPI_LCD_TOUCH_BK_LIGHT_GPIO;
    gpio_config(&bk_cfg);
    gpio_set_level((gpio_num_t)CONFIG_SPI_LCD_TOUCH_BK_LIGHT_GPIO, 0);

    /* 4. 释放 SPI 总线 */
    spi_lcd_touch_deinit();

    /* 5. 配置触摸中断引脚为输入+上拉（确保未触摸时为高电平） */
    ESP_LOGI(TAG, "Configuring GPIO %d with pull-up...", TOUCH_WAKE_GPIO);
    gpio_config_t touch_cfg = {};
    touch_cfg.pin_bit_mask = 1ULL << TOUCH_WAKE_GPIO;
    touch_cfg.mode = GPIO_MODE_INPUT;
    touch_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    touch_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    touch_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&touch_cfg);

    /* 等待 GPIO 变为高电平（触摸释放），最多等 5 秒 */
    for (int i = 0; i < 50; i++) {
        if (gpio_get_level((gpio_num_t)TOUCH_WAKE_GPIO) == 1) {
            ESP_LOGI(TAG, "Touch released");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 6. 配置唤醒源：GPIO 2（触摸中断）低电平唤醒 */
    ESP_LOGI(TAG, "Wake source: GPIO %d (touch interrupt, low level)", TOUCH_WAKE_GPIO);
    esp_err_t wake_err = esp_sleep_enable_ext1_wakeup_io(
        1ULL << TOUCH_WAKE_GPIO,
        ESP_EXT1_WAKEUP_ANY_LOW
    );
    if (wake_err != ESP_OK) {
        ESP_LOGE(TAG, "EXT1 wakeup config FAILED: %s (0x%x)", esp_err_to_name(wake_err), wake_err);
        /* 回退：用定时器唤醒 */
        ESP_LOGW(TAG, "Fallback: timer wakeup (60s)");
        esp_sleep_enable_timer_wakeup(60 * 1000 * 1000);
    } else {
        ESP_LOGI(TAG, "EXT1 wakeup configured OK on GPIO %d", TOUCH_WAKE_GPIO);
    }

    /* 确认当前 GPIO 状态 */
    ESP_LOGI(TAG, "GPIO %d level before sleep: %d", TOUCH_WAKE_GPIO,
             gpio_get_level((gpio_num_t)TOUCH_WAKE_GPIO));

    /* 进入深度睡眠 */
    ESP_LOGI(TAG, "Goodbye!");
    esp_deep_sleep_start();
}

/* ===================================================================
 * 人脸监控任务（主循环 + 空闲检测 → 深度睡眠）
 * =================================================================== */
static void face_monitor_task(void *arg)
{
    (void)arg;

    /* 初始化空闲计时器 */
    s_last_face_time_us = esp_timer_get_time();

    while (1) {
        /* 等待人脸检测信号，超时后检查是否需要休眠 */
        BaseType_t got_signal = xSemaphoreTake(s_face_detected_sem,
                                                pdMS_TO_TICKS(5000));

        if (got_signal == pdTRUE) {
            /* 检测到人脸，更新时间戳，启动售货机 */
            s_last_face_time_us = esp_timer_get_time();
            if (!s_vending_active) {
                start_vending_machine();
                /* 售货机完成后重置计时器 */
                s_last_face_time_us = esp_timer_get_time();
            }
        } else {
            /* 超时 — 检查是否超过空闲阈值 */
            int64_t idle_sec = (esp_timer_get_time() - s_last_face_time_us) / 1000000;
            if (idle_sec >= IDLE_TIMEOUT_SEC && !s_vending_active) {
                ESP_LOGI(TAG, "Idle for %lld seconds, entering deep sleep...", idle_sec);
                enter_deep_sleep();
                /* 不会执行到这里 */
            }
        }
    }
}

/* ===================================================================
 * app_main
 * =================================================================== */
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Smart Vending Machine Starting...");

    /* 检查唤醒原因 */
    uint64_t wakeup_causes = esp_sleep_get_wakeup_causes();
    if (wakeup_causes & (1ULL << ESP_SLEEP_WAKEUP_EXT1)) {
        ESP_LOGI(TAG, "  Wakeup: Touch (GPIO %d)", TOUCH_WAKE_GPIO);
        /* 等待触摸释放，防止立即重新进入睡眠 */
        vTaskDelay(pdMS_TO_TICKS(500));
    } else if (wakeup_causes & (1ULL << ESP_SLEEP_WAKEUP_TIMER)) {
        ESP_LOGI(TAG, "  Wakeup: Timer");
    } else {
        ESP_LOGI(TAG, "  Wakeup: Cold boot");
    }
    ESP_LOGI(TAG, "========================================");

    /* ── Step 0: SD 卡 + 人脸 ID 管理器 ── */
    ESP_LOGI(TAG, "[0/4] Initializing SD card...");
    esp_err_t ret = face_id_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card init FAILED (err=0x%x)! Purchases will NOT be logged.", ret);
    } else {
        ESP_LOGI(TAG, "SD card ready, %d faces registered", face_id_get_count());
    }

    /* ── Step 1: 同步原语 ── */
    ESP_LOGI(TAG, "[1/4] Creating synchronization primitives...");
    s_face_detected_sem = xSemaphoreCreateBinary();
    assert(s_face_detected_sem);

    /* ── Step 2: 摄像头 + 人脸检测+识别管线 ── */
    ESP_LOGI(TAG, "[2/4] Initializing camera and face recognition...");

    /* 帧采集管线 */
    g_frame_cap = new who::frame_cap::WhoFrameCap();
    g_frame_cap->add_node<who::frame_cap::WhoFetchNode>(
        "fetch_ov5647",
        new who::cam::WhoOV5647Cam()
    );
    g_frame_cap->run({
        {CAP_STACK_SIZE, CAP_PRIORITY, 0}
    });
    ESP_LOGI(TAG, "Camera pipeline started");

    /* 人脸检测器 */
    auto *last_node = g_frame_cap->get_last_node();
    g_detector = new who::detect::WhoDetect("face_detect", last_node);
    g_detector->set_model(new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1));
    g_detector->set_fps(DETECT_FPS);
    g_detector->set_detect_result_cb(on_face_detected);
    g_detector->run(DETECT_STACK_SIZE, DETECT_PRIORITY, 1);

    /* 人脸识别器（独立于检测，同步调用） */
    g_face_recognizer = new HumanFaceRecognizer("/sdcard/face_feat_db.bin");

    ESP_LOGI(TAG, "Face recognition started (FPS=%.1f, %d faces in DB)",
             DETECT_FPS, g_face_recognizer->get_num_feats());

    /* ── Step 3: 启动监控 ── */
    ESP_LOGI(TAG, "[3/4] Starting face monitor...");
    xTaskCreate(face_monitor_task, "face_monitor", 4 * 1024, NULL, 3, NULL);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  System ready! Waiting for faces...");
    ESP_LOGI(TAG, "  Idle timeout: %d sec (touch GPIO %d to wake)", IDLE_TIMEOUT_SEC, TOUCH_WAKE_GPIO);
    ESP_LOGI(TAG, "========================================");
}
