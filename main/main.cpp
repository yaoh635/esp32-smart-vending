/*
 * Smart Vending Machine - Main Application
 *
 * Flow: Camera (OV5647) → Face Detection → LCD + Touch → Vending UI → SD Card Log
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <list>
#include <sys/stat.h>
#include <errno.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "esp_sleep.h"

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

/* Voice Control (ESP-SR) */
#include "voice_control.h"

/* WiFi + Web Server (ESP-Hosted) */
#include "wifi_init.h"
#include "web_server.h"

/* Inventory, Auth, Order (for mini-program REST API) */
#include "inventory_manager.h"
#include "admin_auth.h"
#include "order_manager.h"

static const char *TAG = "main";

/* ── 全局管线对象（用于暂停/恢复） ── */
static who::frame_cap::WhoFrameCap *g_frame_cap = NULL;
static who::detect::WhoDetect *g_detector = NULL;

/* ── LCD 显示（只初始化一次） ── */
static lv_display_t *g_lcd_display = NULL;
static bool g_lcd_initialized = false;

/* ── 同步原语 ── */
static SemaphoreHandle_t s_face_detected_sem = NULL;
static volatile bool s_vending_active = false;
static TaskHandle_t s_vending_ui_task_handle = NULL;   /* 用于通知购买完成 */
static TaskHandle_t s_start_vending_task_handle = NULL; /* 用于通知 UI 任务退出 */

/* ── 检测参数 ── */
#define DETECT_FPS          30.0f
#define DETECT_STACK_SIZE   (32 * 1024)
#define DETECT_PRIORITY     5
#define CAP_STACK_SIZE      (4 * 1024)
#define CAP_PRIORITY        6

/* ── 冷却参数 ── */
#define DETECTION_COOLDOWN_US    (3 * 1000 * 1000)  /* 3 秒冷却 */
#define WAKEUP_GPIO             GPIO_NUM_47         /* 光电开关引脚 */
#define WAKEUP_LEVEL            0                   /* 低电平唤醒 */
#define IDLE_BEFORE_SLEEP_MS    (15 * 1000)         /* 15 秒无检测 → 轻度睡眠 */

static int64_t s_last_detect_time = 0;
static volatile bool s_recognizing = false;  /* 识别中标志 */

/* Forward declarations */
static void on_purchase_done(const char *product_name, const char *price_text, void *user_data);

/* 异步识别任务参数 */
typedef struct {
    dl::image::img_t img;
    std::list<dl::detect::result_t> det_res;
} recognize_task_param_t;

/* 异步识别任务 */
static void recognize_task(void *arg)
{
    recognize_task_param_t *param = (recognize_task_param_t *)arg;

    ESP_LOGI(TAG, "Async recognition started...");

    /* 创建识别器（延迟加载模型） */
    HumanFaceRecognizer recognizer("/sdcard/face_feat_db", HumanFaceFeat::MFN_S8_V1, false);

    auto results = recognizer.recognize(param->img, param->det_res);
    if (!results.empty()) {
        int face_id = results[0].id;
        float sim = results[0].similarity;
        ESP_LOGI(TAG, "Recognized face ID: %d (similarity: %.2f)", face_id, sim);
        face_id_set_current_user(face_id);
    } else {
        ESP_LOGI(TAG, "Face not recognized (unknown)");
        face_id_set_current_user(1);
    }

    /* 识别完成，清除标志 */
    s_recognizing = false;

    /* 通知监控任务 */
    xSemaphoreGive(s_face_detected_sem);

    delete param;
    vTaskDelete(NULL);
}

/* 人脸检测回调 — 检测到人脸后异步识别 */
static void on_face_detected(const who::detect::WhoDetect::result_t &result)
{
    int face_count = result.det_res.size();
    web_server_set_face_count(face_count);

    if (face_count > 0 && !s_vending_active && !s_recognizing) {
        /* 冷却检查 */
        int64_t now = esp_timer_get_time();
        if (s_last_detect_time > 0 &&
            (now - s_last_detect_time) < DETECTION_COOLDOWN_US) {
            return;
        }
        s_last_detect_time = now;

        /* 标记识别中，防止重复触发 */
        s_recognizing = true;

        ESP_LOGI(TAG, "Face detected (count=%d), starting async recognition...", face_count);

        /* 复制检测结果，启动异步识别任务 */
        recognize_task_param_t *param = new recognize_task_param_t;
        param->img = result.img;
        param->det_res = result.det_res;

        xTaskCreate(recognize_task, "face_recognize", 32 * 1024, param, 4, NULL);
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

    /* 购买完成后等待感谢界面显示完毕 */
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

    /* 更新库存 */
    inventory_sell_one(product_name);

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
    s_recognizing = false;  /* 重置识别标志 */

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

    /* 2. 初始化语音控制（共享摄像头 I2C 总线） */
    ESP_LOGI(TAG, "Initializing voice control...");
    i2c_master_bus_handle_t i2c_bus = camera_get_i2c_bus_handle();
    esp_err_t vc_ret = voice_control_init(i2c_bus);
    if (vc_ret != ESP_OK) {
        ESP_LOGW(TAG, "Voice control init failed (continuing without voice): %s",
                 esp_err_to_name(vc_ret));
    }

    /* 3. 初始化 SPI LCD + Touch（仅首次） */
    if (!g_lcd_initialized) {
        ESP_LOGI(TAG, "Initializing SPI LCD + Touch...");
        spi_lcd_touch_config_t lcd_cfg = spi_lcd_touch_get_default_config();
        lcd_cfg.touch_enabled = true;

        esp_err_t ret = spi_lcd_touch_init(&lcd_cfg, &g_lcd_display);
        if (ret != ESP_OK || g_lcd_display == NULL) {
            ESP_LOGE(TAG, "Failed to init SPI LCD: %s", esp_err_to_name(ret));
            s_vending_active = false;
            /* LCD 失败，恢复检测 */
            voice_control_stop();
            if (g_frame_cap) g_frame_cap->resume();
            if (g_detector) g_detector->resume();
            return;
        }
        g_lcd_initialized = true;
        ESP_LOGI(TAG, "SPI LCD initialized");
    } else {
        ESP_LOGI(TAG, "Reusing existing SPI LCD");
        /* 打开背光 */
        gpio_config_t bk_cfg = {};
        bk_cfg.mode = GPIO_MODE_OUTPUT;
        bk_cfg.pin_bit_mask = 1ULL << CONFIG_SPI_LCD_TOUCH_BK_LIGHT_GPIO;
        gpio_config(&bk_cfg);
        gpio_set_level((gpio_num_t)CONFIG_SPI_LCD_TOUCH_BK_LIGHT_GPIO, 1);
    }

    /* 4. 用户已在识别回调中设置，直接获取 */
    int user_id = face_id_get_current_user();
    if (user_id > 0) {
        ESP_LOGI(TAG, "Current user ID %d (from recognition)", user_id);
    } else {
        ESP_LOGW(TAG, "No user set (recognition may have failed)");
    }

    /* 5. 启动售货机 UI 任务 */
    s_start_vending_task_handle = xTaskGetCurrentTaskHandle();
    xTaskCreate(vending_ui_task, "vending_ui", 16 * 1024, g_lcd_display, 4, NULL);

    /* 6. 阻塞等待 UI 任务完成通知 */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    s_start_vending_task_handle = NULL;

    /* 7. 停止售货机 UI（禁用定时器回调，防止访问已销毁对象） */
    vending_machine_stop();

    /* 8. 停止语音控制（释放 I2S 资源） */
    voice_control_stop();

    /* 9. 关闭 LCD 背光（不释放 LCD 资源，下次复用） */
    ESP_LOGI(TAG, "Turning off LCD backlight...");
    gpio_config_t bk_cfg = {};
    bk_cfg.mode = GPIO_MODE_OUTPUT;
    bk_cfg.pin_bit_mask = 1ULL << CONFIG_SPI_LCD_TOUCH_BK_LIGHT_GPIO;
    gpio_config(&bk_cfg);
    gpio_set_level((gpio_num_t)CONFIG_SPI_LCD_TOUCH_BK_LIGHT_GPIO, 0);

    ESP_LOGI(TAG, "Resuming face detection pipeline...");

    /* 10. 恢复检测管线 */
    if (g_frame_cap) g_frame_cap->resume();
    if (g_detector) g_detector->resume();

    ESP_LOGI(TAG, "=== Face detection resumed ===");
}

/* ===================================================================
 * 人脸监控任务（主循环 + 空闲检测 → 轻度睡眠）
 * =================================================================== */
static void face_monitor_task(void *arg)
{
    (void)arg;
    int64_t last_active_time = esp_timer_get_time();

    while (1) {
        /* 等待人脸检测信号（3 秒超时） */
        BaseType_t got_signal = xSemaphoreTake(s_face_detected_sem,
                                                pdMS_TO_TICKS(3000));

        /* 每 3 秒打印 GPIO47 电平 */
        ESP_LOGI(TAG, "GPIO%d level: %d", WAKEUP_GPIO, gpio_get_level(WAKEUP_GPIO));

        if (got_signal == pdTRUE && !s_vending_active) {
            last_active_time = esp_timer_get_time();
            start_vending_machine();
        } else {
            /* 检查空闲时间 */
            int64_t idle_ms = (esp_timer_get_time() - last_active_time) / 1000;
            if (idle_ms >= IDLE_BEFORE_SLEEP_MS) {
                /* 等待 GPIO 为高电平（光电开关未触发），避免立即唤醒 */
                if (gpio_get_level(WAKEUP_GPIO) == WAKEUP_LEVEL) {
                    ESP_LOGI(TAG, "GPIO%d is LOW, waiting before sleep...", WAKEUP_GPIO);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                ESP_LOGI(TAG, "Idle for %lld ms, entering light sleep...", idle_ms);

                esp_err_t err = esp_light_sleep_start();

                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Woke up from light sleep, GPIO%d level: %d",
                             WAKEUP_GPIO,
                             gpio_get_level(WAKEUP_GPIO));
                } else {
                    ESP_LOGW(TAG, "Light sleep failed: %s", esp_err_to_name(err));
                }
                last_active_time = esp_timer_get_time();
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
    ESP_LOGI(TAG, "========================================");

    /* 配置 GPIO47 为输入（光电开关常态高） */
    gpio_config_t wakeup_io_cfg = {};
    wakeup_io_cfg.pin_bit_mask = 1ULL << WAKEUP_GPIO;
    wakeup_io_cfg.mode = GPIO_MODE_INPUT;
    wakeup_io_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    wakeup_io_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    wakeup_io_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&wakeup_io_cfg);

    /* 配置 GPIO 唤醒源（低电平触发） */
    gpio_wakeup_enable(WAKEUP_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();

    /* ── Step 0: SD 卡 + 人脸 ID 管理器 ── */
    ESP_LOGI(TAG, "[0/5] Initializing SD card...");
    esp_err_t ret = face_id_manager_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD card init FAILED (err=0x%x)! Purchases will NOT be logged.", ret);
    } else {
        ESP_LOGI(TAG, "SD card ready, %d faces registered", face_id_get_count());
    }

    /* ── Step 0.5: 库存 + 认证 + 订单管理器 ── */
    ESP_LOGI(TAG, "[0.5/5] Initializing inventory, auth, order...");
    esp_err_t inv_ret = inventory_manager_init();
    esp_err_t auth_ret = admin_auth_init();
    esp_err_t order_ret = order_manager_init();
    ESP_LOGI(TAG, "  Inventory: %s  Auth: %s  Order: %s",
             inv_ret == ESP_OK ? "OK" : "FAIL",
             auth_ret == ESP_OK ? "OK" : "FAIL",
             order_ret == ESP_OK ? "OK" : "FAIL");

    /* ── Step 1: 同步原语 + 异步识别队列 ── */
    ESP_LOGI(TAG, "[1/5] Creating synchronization primitives...");
    s_face_detected_sem = xSemaphoreCreateBinary();
    assert(s_face_detected_sem);

    /* ── Step 2: 摄像头 + 人脸检测管线 ── */
    ESP_LOGI(TAG, "[2/5] Initializing camera and face recognition...");

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

    /* 检测阈值 0.3（模糊画面需要低阈值），误检由冷却+相似度+注册限制过滤 */
    auto *detect_model = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1);
    detect_model->set_score_thr(0.2f, 0);  /* MSR阶段: 0.2 */
    detect_model->set_score_thr(0.2f, 1);  /* MNP阶段: 0.2 */
    detect_model->set_nms_thr(0.4f, 0);    /* MSR NMS: 0.4 */
    detect_model->set_nms_thr(0.4f, 1);    /* MNP NMS: 0.4 */
    g_detector->set_model(detect_model);

    g_detector->set_fps(DETECT_FPS);
    g_detector->set_detect_result_cb(on_face_detected);
    g_detector->run(DETECT_STACK_SIZE, DETECT_PRIORITY, 1);

    /* ── Step 3: 启动监控任务 ── */
    ESP_LOGI(TAG, "[3/5] Starting face monitor task...");
    xTaskCreate(face_monitor_task, "face_monitor", 4 * 1024, NULL, 3, NULL);

    /* ── Step 4: WiFi + Web Server (camera streaming) ── */
#if CONFIG_APP_WIFI_ENABLED
    ESP_LOGI(TAG, "[4/5] Initializing WiFi...");
    if (wifi_init() == ESP_OK) {
        char ip_str[16] = {0};
        wifi_get_ip_str(ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "WiFi connected, IP: %s", ip_str);

        /* Start web server with camera stream */
        static web_server_config_t ws_cfg;
        ws_cfg.frame_cap_node = (void *)g_frame_cap->get_last_node();
        ws_cfg.detector = (void *)g_detector;
        ws_cfg.vending_active = &s_vending_active;
        ws_cfg.cam_width = CAM_H_RES;
        ws_cfg.cam_height = CAM_V_RES;

        esp_err_t ws_ret = web_server_start(&ws_cfg);
        if (ws_ret == ESP_OK) {
            ESP_LOGI(TAG, "Web server: http://%s/", ip_str);
        } else {
            ESP_LOGE(TAG, "Web server start failed: %s", esp_err_to_name(ws_ret));
        }
    } else {
        ESP_LOGW(TAG, "WiFi init failed, web server not started");
    }
#endif

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  System ready! Waiting for faces...");
    ESP_LOGI(TAG, "========================================");
}
