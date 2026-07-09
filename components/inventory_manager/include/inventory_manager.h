/*
 * Inventory Manager Component
 * Tracks product stock levels and sales statistics on SD card
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
#define INVENTORY_MOUNT_POINT   "/sdcard"
#define INVENTORY_CSV_PATH      "/sdcard/inventory.csv"

/* ── 常量 ── */
#define INVENTORY_MAX_PRODUCTS  20
#define INVENTORY_MAX_NAME_LEN  32

/* ── 商品库存结构体 ── */
typedef struct {
    char name[INVENTORY_MAX_NAME_LEN];   /**< 商品名称 */
    int  total_stock;                     /**< 总进货量 */
    int  sold_count;                      /**< 已售数量 */
    float price;                          /**< 单价 */
} product_inventory_t;

/* ── 销售统计摘要 ── */
typedef struct {
    int   total_sales;       /**< 总销量（件数） */
    float total_revenue;     /**< 总营收 */
    int   today_sales;       /**< 今日销量 */
    float today_revenue;     /**< 今日营收 */
    int   total_users;       /**< 已注册用户数 */
} sales_summary_t;

/* ── 日销售记录 ── */
typedef struct {
    int   year;
    int   month;
    int   day;
    char  product_name[INVENTORY_MAX_NAME_LEN];
    int   count;
    float revenue;
} daily_sales_t;

/* ── 推荐项 ── */
typedef struct {
    char   product_name[INVENTORY_MAX_NAME_LEN];
    float  price;
    int    total_stock;
    int    sold_count;
    int    buy_freq;          /**< 该用户购买次数（个性化推荐时使用） */
    int    recommend_score;   /**< 推荐分数（越高越推荐） */
    char   reason[32];        /**< 推荐理由，如 "常买" / "热销" */
} recommend_item_t;

/* ── API 函数 ── */

/**
 * @brief 初始化库存管理器
 *        从 SD 卡加载 inventory.csv。如文件不存在，使用默认商品创建。
 * @return ESP_OK 成功
 */
esp_err_t inventory_manager_init(void);

/**
 * @brief 获取所有商品的库存信息
 * @param[out] items  库存数组（调用者分配内存）
 * @param[out] count  实际商品数量
 * @return ESP_OK 成功
 */
esp_err_t inventory_get_all(product_inventory_t *items, int *count);

/**
 * @brief 卖出指定商品 1 件（sold_count++, total_stock--）
 * @param product_name  商品名称（需与 CSV 中的名称完全匹配）
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 商品不存在，ESP_ERR_INVALID_STATE 库存不足
 */
esp_err_t inventory_sell_one(const char *product_name);

/**
 * @brief 补货（增加 total_stock）
 * @param product_name  商品名称
 * @param count         补货数量
 * @return ESP_OK 成功
 */
esp_err_t inventory_restock(const char *product_name, int count);

/**
 * @brief 获取销售统计摘要
 * @param[out] summary  统计摘要
 * @return ESP_OK 成功
 */
esp_err_t inventory_get_sales_summary(sales_summary_t *summary);

/**
 * @brief 获取畅销排行（按销量降序）
 * @param[out] ranking  排序后的商品数组
 * @param[out] count    商品数量
 * @return ESP_OK 成功
 */
esp_err_t inventory_get_ranking(product_inventory_t *ranking, int *count);

/**
 * @brief 获取单个商品详情
 * @param product_name  商品名称
 * @param[out] item     输出的商品信息
 * @return ESP_OK 成功，ESP_ERR_NOT_FOUND 商品不存在
 */
esp_err_t inventory_get_product(const char *product_name, product_inventory_t *item);

/**
 * @brief 获取个性化推荐商品
 *        读取 purchase_log.csv 统计该用户购买次数最多的商品，
 *        不足时用全局销量排行补充，返回 Top N
 * @param user_id       用户 ID
 * @param[out] items    推荐结果数组（调用者分配，建议至少 INVENTORY_MAX_PRODUCTS）
 * @param[out] count    实际返回数量（最多 5）
 * @return ESP_OK 成功
 */
esp_err_t inventory_get_recommend(int user_id, recommend_item_t *items, int *count);

/**
 * @brief 将当前库存数据写回 SD 卡
 * @return ESP_OK 成功
 */
esp_err_t inventory_save_to_sd(void);

#ifdef __cplusplus
}
#endif
