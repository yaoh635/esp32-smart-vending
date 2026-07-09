/*
 * Order Manager Component Implementation
 * Order lifecycle engine: create → confirm → ship (or cancel)
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "order_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

static const char *TAG = "order_mgr";

/* ── 全局状态 ── */
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;
static int s_next_order_id = 1;

/* ── 内存缓存（仅存活跃订单：pending + confirmed） ── */
static order_record_t s_active_orders[ORDER_MAX_ACTIVE];
static int s_active_count = 0;

/* ── 超时扫描定时器 ── */
static TimerHandle_t s_expiry_timer = NULL;

/* ── 内部辅助 ── */

/**
 * @brief 将订单追加写入 CSV（单行追加，省内存但文件会越来越大）
 *        这里用全量重写方式，保证 CSV 与内存缓存一致
 */
static esp_err_t rewrite_csv(void)
{
    FILE *f = fopen(ORDER_CSV_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open orders.csv for writing");
        return ESP_FAIL;
    }

    fprintf(f, "order_id,user_id,product_name,price,payment_method,status,"
               "created_at_us,confirmed_at_us,shipped_at_us\n");

    for (int i = 0; i < s_active_count; i++) {
        order_record_t *o = &s_active_orders[i];
        fprintf(f, "%s,%d,%s,%.2f,%d,%d,%lld,%lld,%lld\n",
                o->order_id, o->user_id, o->product_name,
                (double)o->price, o->payment_method, o->status,
                (long long)o->created_at_us,
                (long long)o->confirmed_at_us,
                (long long)o->shipped_at_us);
    }

    fclose(f);
    return ESP_OK;
}

/**
 * @brief 从 CSV 加载已有订单到内存
 */
static esp_err_t load_from_csv(void)
{
    FILE *f = fopen(ORDER_CSV_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "orders.csv not found, starting fresh");
        return ESP_ERR_NOT_FOUND;
    }

    s_active_count = 0;
    char line[256];

    /* Skip header */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return ESP_OK;
    }

    while (fgets(line, sizeof(line), f) && s_active_count < ORDER_MAX_ACTIVE) {
        order_record_t o = {0};
        long long created = 0, confirmed = 0, shipped = 0;

        if (sscanf(line, "%15[^,],%d,%31[^,],%f,%d,%d,%lld,%lld,%lld",
                   o.order_id, &o.user_id, o.product_name, &o.price,
                   &o.payment_method, &o.status,
                   (long long *)&created,
                   (long long *)&confirmed,
                   (long long *)&shipped) >= 6) {
            o.created_at_us = (int64_t)created;
            o.confirmed_at_us = (int64_t)confirmed;
            o.shipped_at_us = (int64_t)shipped;

            /* 只加载活跃订单（pending + confirmed）到内存 */
            if (o.status == ORDER_PENDING || o.status == ORDER_CONFIRMED) {
                s_active_orders[s_active_count++] = o;

                /* 更新自增 ID */
                int id = atoi(o.order_id);
                if (id >= s_next_order_id) {
                    s_next_order_id = id + 1;
                }
            }
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d active orders, next_id=%d", s_active_count, s_next_order_id);
    return ESP_OK;
}

/**
 * @brief 查找活跃订单的索引，-1 表示未找到
 */
static int find_active_index(const char *order_id)
{
    for (int i = 0; i < s_active_count; i++) {
        if (strcmp(s_active_orders[i].order_id, order_id) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 移除已完成/已取消订单出内存缓存并写回 CSV
 */
static void remove_from_active(int index)
{
    ESP_LOGI(TAG, "Order %s completed, removing from active cache",
             s_active_orders[index].order_id);

    /* 将记录追加到一个永久日志文件中（可选） */
    /* 这里简单地从活跃数组移除：用最后一个元素填充 */

    if (index < s_active_count - 1) {
        s_active_orders[index] = s_active_orders[s_active_count - 1];
    }
    s_active_count--;
    rewrite_csv();
}

/* ── 超时扫描回调 ── */
static void expiry_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return; /* 拿不到锁，下轮再扫描 */
    }

    int64_t now_us = esp_timer_get_time();

    for (int i = s_active_count - 1; i >= 0; i--) {
        if (s_active_orders[i].status == ORDER_PENDING) {
            int64_t elapsed_s = (now_us - s_active_orders[i].created_at_us) / 1000000;
            if (elapsed_s >= ORDER_TIMEOUT_SEC) {
                ESP_LOGI(TAG, "Order %s expired (%llds), auto-cancelling",
                         s_active_orders[i].order_id, (long long)elapsed_s);
                s_active_orders[i].status = ORDER_CANCELLED;
                remove_from_active(i);
            }
        }
    }

    xSemaphoreGive(s_mutex);
}

/* ── 公共 API ── */

esp_err_t order_manager_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* 加载已有订单 */
    esp_err_t ret = load_from_csv();

    /* 如果 CSV 不存在，创建空文件 */
    if (ret == ESP_ERR_NOT_FOUND) {
        FILE *f = fopen(ORDER_CSV_PATH, "w");
        if (f) {
            fprintf(f, "order_id,user_id,product_name,price,payment_method,status,"
                       "created_at_us,confirmed_at_us,shipped_at_us\n");
            fclose(f);
            ESP_LOGI(TAG, "Created empty orders.csv");
        }
    }

    /* 启动超时扫描定时器 */
    s_expiry_timer = xTimerCreate(
        "order_expiry",
        pdMS_TO_TICKS(ORDER_EXPIRY_SCAN_SEC * 1000),
        pdTRUE,   /* auto-reload */
        NULL,
        expiry_timer_cb
    );
    if (s_expiry_timer) {
        xTimerStart(s_expiry_timer, 0);
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Order manager initialized (order_timeout=%ds, scan_interval=%ds)",
             ORDER_TIMEOUT_SEC, ORDER_EXPIRY_SCAN_SEC);
    return ESP_OK;
}

esp_err_t order_create(int user_id, const char *product_name, float price,
                       payment_method_t payment, char *order_id_out, size_t id_len)
{
    if (!s_initialized || !product_name || !order_id_out) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_active_count >= ORDER_MAX_ACTIVE) {
        ESP_LOGE(TAG, "Too many active orders (%d max)", ORDER_MAX_ACTIVE);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    /* 填充订单 */
    order_record_t *o = &s_active_orders[s_active_count];
    memset(o, 0, sizeof(*o));
    snprintf(o->order_id, ORDER_ID_LEN, "%d", s_next_order_id++);
    o->user_id = user_id;
    strncpy(o->product_name, product_name, sizeof(o->product_name) - 1);
    o->product_name[sizeof(o->product_name) - 1] = '\0';
    o->price = price;
    o->payment_method = (int)payment;
    o->status = ORDER_PENDING;
    o->created_at_us = esp_timer_get_time();

    /* 输出订单 ID */
    strncpy(order_id_out, o->order_id, id_len);
    order_id_out[id_len - 1] = '\0';

    s_active_count++;

    /* 持久化到 SD 卡 */
    rewrite_csv();

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Order %s created: user=%d product=%s price=%.2f payment=%d",
             o->order_id, user_id, product_name, (double)price, payment);
    return ESP_OK;
}

esp_err_t order_get(const char *order_id, order_record_t *record)
{
    if (!s_initialized || !order_id || !record) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_active_index(order_id);
    if (idx >= 0) {
        *record = s_active_orders[idx];
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t order_confirm(const char *order_id)
{
    if (!s_initialized || !order_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_active_index(order_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_active_orders[idx].status != ORDER_PENDING) {
        ESP_LOGW(TAG, "Order %s is not pending (status=%d), cannot confirm",
                 order_id, s_active_orders[idx].status);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_active_orders[idx].status = ORDER_CONFIRMED;
    s_active_orders[idx].confirmed_at_us = esp_timer_get_time();

    rewrite_csv();

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Order %s confirmed (user=%d product=%s)",
             order_id, s_active_orders[idx].user_id,
             s_active_orders[idx].product_name);
    return ESP_OK;
}

esp_err_t order_ship(const char *order_id)
{
    if (!s_initialized || !order_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_active_index(order_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_active_orders[idx].status != ORDER_CONFIRMED) {
        ESP_LOGW(TAG, "Order %s is not confirmed (status=%d), cannot ship",
                 order_id, s_active_orders[idx].status);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_active_orders[idx].status = ORDER_SHIPPED;
    s_active_orders[idx].shipped_at_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Order %s shipped: user=%d product=%s price=%.2f",
             order_id, s_active_orders[idx].user_id,
             s_active_orders[idx].product_name,
             (double)s_active_orders[idx].price);

    /* 移除出活跃缓存 */
    remove_from_active(idx);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t order_cancel(const char *order_id)
{
    if (!s_initialized || !order_id) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int idx = find_active_index(order_id);
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_active_orders[idx].status != ORDER_PENDING) {
        ESP_LOGW(TAG, "Order %s is not pending, cannot cancel", order_id);
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Order %s cancelled", order_id);
    s_active_orders[idx].status = ORDER_CANCELLED;
    remove_from_active(idx);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t order_find_pending_by_user(int user_id, order_record_t *record)
{
    if (!s_initialized || !record) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < s_active_count; i++) {
        if (s_active_orders[i].user_id == user_id &&
            s_active_orders[i].status == ORDER_PENDING) {
            *record = s_active_orders[i];
            xSemaphoreGive(s_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t order_get_all_active(order_record_t *records, int *count)
{
    if (!s_initialized || !records || !count) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(records, s_active_orders, sizeof(order_record_t) * s_active_count);
    *count = s_active_count;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t order_get_by_user(int user_id, order_record_t *records, int *count)
{
    if (!s_initialized || !records || !count) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    *count = 0;
    for (int i = 0; i < s_active_count; i++) {
        if (s_active_orders[i].user_id == user_id &&
            *count < ORDER_MAX_ACTIVE) {
            records[*count] = s_active_orders[i];
            (*count)++;
        }
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t order_save_to_sd(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t ret = rewrite_csv();
    xSemaphoreGive(s_mutex);
    return ret;
}

const char *order_status_str(order_status_t status)
{
    switch (status) {
        case ORDER_PENDING:   return "pending";
        case ORDER_CONFIRMED: return "confirmed";
        case ORDER_SHIPPED:   return "shipped";
        case ORDER_CANCELLED: return "cancelled";
        default:              return "unknown";
    }
}

const char *order_payment_str(payment_method_t method)
{
    switch (method) {
        case PAYMENT_FACE: return "face";
        case PAYMENT_SCAN: return "scan";
        default:           return "unknown";
    }
}
