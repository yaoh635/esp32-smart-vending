/*
 * Face ID Manager Component
 * Manages face ID registration and purchase logging on SD card
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_ID_MOUNT_POINT    "/sdcard"
#define FACE_DB_PATH           "/sdcard/face_db.csv"
#define PURCHASE_LOG_PATH      "/sdcard/purchase_log.csv"
#define FACE_ID_MAX_NAME_LEN   32
#define FACE_ID_MAX_FACES      100

/**
 * @brief 人脸记录结构体
 */
typedef struct {
    int id;                              /**< 人脸 ID */
    char name[FACE_ID_MAX_NAME_LEN];    /**< 用户名称 */
    int64_t enroll_time;                 /**< 注册时间戳 (微秒) */
} face_record_t;

/**
 * @brief 购买记录结构体
 */
typedef struct {
    int64_t timestamp;                   /**< 购买时间戳 (微秒) */
    int face_id;                         /**< 人脸 ID (-1 表示未知用户) */
    char product_name[32];               /**< 商品名称 */
    char price_text[16];                 /**< 价格文本 */
} purchase_record_t;

/**
 * @brief 初始化人脸 ID 管理器
 *        挂载 SD 卡，加载人脸数据库。
 *
 * @return 成功返回 ESP_OK
 */
esp_err_t face_id_manager_init(void);

/**
 * @brief 注册一个新人脸 ID
 *        分配一个新的 ID 并写入 SD 卡。
 *
 * @param name  用户名称（可选，传 NULL 自动生成）
 * @return 新的人脸 ID，失败返回 -1
 */
int face_id_register(const char *name);

/**
 * @brief 根据人脸识别结果查找或创建用户
 *        如果识别到已知 ID，返回该 ID；否则注册新用户。
 *
 * @param recognized_id  识别到的人脸 ID（来自 WHO 框架）
 * @return 用户 ID
 */
int face_id_find_or_create(int recognized_id);

/**
 * @brief 记录一次购买
 *        将购买信息写入 SD 卡的 purchase_log.csv。
 *
 * @param face_id      人脸 ID
 * @param product_name 商品名称
 * @param price_text   价格文本
 * @return 成功返回 ESP_OK
 */
esp_err_t face_id_log_purchase(int face_id, const char *product_name,
                               const char *price_text);

/**
 * @brief 购买完成回调（可直接传给 vending_machine_config_t）
 */
void face_id_purchase_callback(const char *product_name,
                               const char *price_text,
                               void *user_data);

/**
 * @brief 设置当前活跃用户 ID
 *        在人脸识别成功后调用。
 *
 * @param id  人脸 ID
 */
void face_id_set_current_user(int id);

/**
 * @brief 获取当前活跃用户 ID
 * @return 当前用户 ID，-1 表示无用户
 */
int face_id_get_current_user(void);

/**
 * @brief 获取人脸记录数量
 * @return 已注册的人脸数量
 */
int face_id_get_count(void);

#ifdef __cplusplus
}
#endif
