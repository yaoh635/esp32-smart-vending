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
static HumanFaceRecognizer *g_face_recognizer = NULL;  /* 人脸识别器 */

/* ── LCD 显示（只初始化一次） ── */
static lv_display_t *g_lcd_display = NULL;
static bool g_lcd_initialized = false;

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

/* Forward declarations */
static void on_purchase_done(const char *product_name, const char *price_text, void *user_data);

/* ===================================================================
 * 人脸检测回调（同步执行识别 — 同一帧）
 * =================================================================== */
static void on_face_detected(const who::detect::WhoDetect::result_t &result)
{
    int face_count = result.det_res.size();

    /* Update web server face count for status reporting */
    web_server_set_face_count(face_count);

    if (face_count > 0 && !s_vending_active) {
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
 * 人脸监控任务（主循环 + 空闲检测 → 深度睡眠）
 * =================================================================== */
static void face_monitor_task(void *arg)
{
    (void)arg;

    while (1) {
        /* 等待人脸检测信号 */
        BaseType_t got_signal = xSemaphoreTake(s_face_detected_sem,
                                                pdMS_TO_TICKS(5000));

        if (got_signal == pdTRUE && !s_vending_active) {
            start_vending_machine();
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

    /* ── Step 1: 同步原语 ── */
    ESP_LOGI(TAG, "[1/5] Creating synchronization primitives...");
    s_face_detected_sem = xSemaphoreCreateBinary();
    assert(s_face_detected_sem);

    /* ── Step 2: 摄像头 + 人脸检测+识别管线 ── */
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

    /* 降低检测阈值，提高暗光/模糊环境下的检测率 */
    auto *detect_model = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1);
    detect_model->set_score_thr(0.3f, 0);  /* MSR阶段: 0.5→0.3 */
    detect_model->set_score_thr(0.3f, 1);  /* MNP阶段: 0.5→0.3 */
    detect_model->set_nms_thr(0.4f, 0);    /* MSR NMS: 0.5→0.4 */
    detect_model->set_nms_thr(0.4f, 1);    /* MNP NMS: 0.5→0.4 */
    g_detector->set_model(detect_model);

    g_detector->set_fps(DETECT_FPS);
    g_detector->set_detect_result_cb(on_face_detected);
    g_detector->run(DETECT_STACK_SIZE, DETECT_PRIORITY, 1);

    /* 人脸识别器（独立于检测，同步调用） */
    /* 检查特征数据库是否损坏（特征数远大于用户数说明损坏） */
    int user_count = face_id_get_count();
    ESP_LOGI(TAG, "Face DB check: %d users registered", user_count);

    g_face_recognizer = new HumanFaceRecognizer("/sdcard/face_feat_db.bin");
    int feat_count = g_face_recognizer->get_num_feats();

    /* 如果特征数超过用户数的3倍（允许同一用户多次注册），删除重建 */
    if (feat_count > 0 && user_count > 0 && feat_count > (user_count * 3 + 10)) {
        ESP_LOGW(TAG, "Face feature DB corrupted! feats=%d, users=%d. Rebuilding...",
                 feat_count, user_count);
        delete g_face_recognizer;
        remove("/sdcard/face_feat_db.bin");
        g_face_recognizer = new HumanFaceRecognizer("/sdcard/face_feat_db.bin");
        feat_count = g_face_recognizer->get_num_feats();
        ESP_LOGI(TAG, "Face feature DB rebuilt, now has %d feats", feat_count);
    }

    ESP_LOGI(TAG, "Face recognition started (FPS=%.1f, %d faces in DB)",
             DETECT_FPS, g_face_recognizer->get_num_feats());

    /* ── Step 3: 启动监控 ── */
    ESP_LOGI(TAG, "[3/5] Starting face monitor...");
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
