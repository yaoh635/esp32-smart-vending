/*
 * Order Manager Component
 * Order lifecycle engine: create → confirm → ship (or cancel)
 * Persisted to /sdcard/orders.csv, with in-memory cache for active orders
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

/* ── 文件路径 ── */
#define ORDER_CSV_PATH         "/sdcard/orders.csv"
#define ORDER_MAX_ACTIVE       50       /* 最大活跃订单数（内存缓存） */
#define ORDER_ID_LEN           16       /* 订单 ID 字符串长度 */
#define ORDER_TIMEOUT_SEC      60       /* 订单超时时间（秒） */
#define ORDER_EXPIRY_SCAN_SEC  10       /* 超时扫描间隔（秒） */

/* ── 订单状态 ── */
typedef enum {
    ORDER_PENDING   = 0,   /* 已提交，等待用户到售货机前确认 */
    ORDER_CONFIRMED = 1,   /* 用户已确认（人脸识别匹配），准备出货 */
    ORDER_SHIPPED   = 2,   /* 出货完成 */
    ORDER_CANCELLED = 3    /* 超时取消或用户手动取消 */
} order_status_t;

/* ── 支付方式 ── */
typedef enum {
    PAYMENT_FACE = 0,      /* 人脸支付 */
    PAYMENT_SCAN = 1       /* 扫码支付 */
} payment_method_t;

/* ── 订单记录 ── */
typedef struct {
    char order_id[ORDER_ID_LEN];
    int  user_id;
    char product_name[32];
    float price;
    int  payment_method;         /* payment_method_t */
    int  status;                 /* order_status_t */
    int64_t created_at_us;
    int64_t confirmed_at_us;
    int64_t shipped_at_us;
} order_record_t;

/* ── API 函数 ── */

/**
 * @brief 初始化订单管理器
 *        加载已有订单到内存缓存，启动超时扫描定时器
 * @return ESP_OK 成功
 */
esp_err_t order_manager_init(void);

/**
 * @brief 创建新订单（状态 = pending）
 * @param user_id       用户 ID
 * @param product_name  商品名称
 * @param price         单价
 * @param payment       支付方式 (PAYMENT_FACE / PAYMENT_SCAN)
 * @param[out] order_id_out  输出的订单 ID 字符串
 * @param id_len        输出缓冲区长度
 * @return ESP_OK 成功，ESP_ERR_INVALID_STATE 库存不足
 */
esp_err_t order_create(int user_id, const char *product_name, float price,
                       payment_method_t payment, char *order_id_out, size_t id_len);

/**
 * @brief 查询订单状态
 * @param order_id  订单 ID
 * @param[out] record  订单记录
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 不存在
 */
esp_err_t order_get(const char *order_id, order_record_t *record);

/**
 * @brief 确认订单（pending → confirmed）
 * @param order_id  订单 ID
 * @return ESP_OK 成功
 */
esp_err_t order_confirm(const char *order_id);

/**
 * @brief 完成出货（confirmed → shipped）+ 扣减库存
 * @param order_id  订单 ID
 * @return ESP_OK 成功
 */
esp_err_t order_ship(const char *order_id);

/**
 * @brief 取消订单（pending → cancelled）
 * @param order_id  订单 ID
 * @return ESP_OK 成功
 */
esp_err_t order_cancel(const char *order_id);

/**
 * @brief 查找指定用户的待取订单（pending 状态）
 * @param user_id  用户 ID
 * @param[out] record  找到的订单（仅第一个）
 * @return ESP_OK 找到，ESP_ERR_NOT_FOUND 该用户无待取订单
 */
esp_err_t order_find_pending_by_user(int user_id, order_record_t *record);

/**
 * @brief 获取所有活跃订单（pending + confirmed）
 * @param[out] records  订单数组
 * @param[out] count    实际数量（最多 ORDER_MAX_ACTIVE）
 * @return ESP_OK
 */
esp_err_t order_get_all_active(order_record_t *records, int *count);

/**
 * @brief 获取指定用户的所有订单
 * @param user_id  用户 ID
 * @param[out] records  订单数组
 * @param[out] count    实际数量
 * @return ESP_OK
 */
esp_err_t order_get_by_user(int user_id, order_record_t *records, int *count);

/**
 * @brief 将当前所有活跃订单写回 CSV（SD 卡持久化）
 * @return ESP_OK
 */
esp_err_t order_save_to_sd(void);

/**
 * @brief 获取订单状态的可读字符串
 * @return "pending" / "confirmed" / "shipped" / "cancelled"
 */
const char *order_status_str(order_status_t status);

/**
 * @brief 获取支付方式的可读字符串
 * @return "face" / "scan"
 */
const char *order_payment_str(payment_method_t method);

#ifdef __cplusplus
}
#endif
