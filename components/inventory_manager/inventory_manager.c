/*
 * Inventory Manager Component Implementation
 * Tracks product stock levels and sales statistics on SD card
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "inventory_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static const char *TAG = "inventory_mgr";

/* ── 互斥锁（保护文件读写） ── */
static SemaphoreHandle_t s_inventory_mutex = NULL;

/* ── 内存中的库存缓存 ── */
static product_inventory_t s_inventory[INVENTORY_MAX_PRODUCTS];
static int s_inventory_count = 0;
static bool s_initialized = false;
static bool s_sd_available = false;   /* SD 卡是否可用（不可用时纯内存模式） */

/* ── 默认商品（中文名，与 vending_machine_ui 一致） ── */
static const product_inventory_t s_default_products[] = {
    {"Cola",     100, 0, 2.00f},
    {"Sprite",   100, 0, 3.00f},
    {"Water",    100, 0, 1.50f},
    {"RedBull",   80, 0, 6.00f},
    {"Chips",     80, 0, 3.00f},
    {"Chocolate", 60, 0, 8.00f},
};
#define DEFAULT_COUNT (sizeof(s_default_products) / sizeof(s_default_products[0]))

/* ── 内部辅助函数 ── */

/**
 * @brief 从 SD 卡加载库存 CSV
 */
static esp_err_t load_inventory_from_sd(void)
{
    /* 用 stat() 检查文件是否存在（对标 admin_auth，避免 fopen("r") 失败影响后续写入） */
    struct stat st;
    if (stat(INVENTORY_CSV_PATH, &st) != 0) {
        ESP_LOGW(TAG, "Inventory CSV not found, will create default");
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(INVENTORY_CSV_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "Inventory CSV exists but cannot open, will create default");
        return ESP_ERR_NOT_FOUND;
    }

    s_inventory_count = 0;
    char line[128];

    /* Skip header */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    while (fgets(line, sizeof(line), f) && s_inventory_count < INVENTORY_MAX_PRODUCTS) {
        product_inventory_t *item = &s_inventory[s_inventory_count];
        char name_buf[INVENTORY_MAX_NAME_LEN] = {0};
        int total = 0, sold = 0;
        float price = 0.0f;

        if (sscanf(line, "%31[^,],%d,%d,%f", name_buf, &total, &sold, &price) >= 4) {
            strncpy(item->name, name_buf, INVENTORY_MAX_NAME_LEN - 1);
            item->name[INVENTORY_MAX_NAME_LEN - 1] = '\0';
            item->total_stock = total;
            item->sold_count = sold;
            item->price = price;
            s_inventory_count++;
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d products from inventory CSV", s_inventory_count);
    return ESP_OK;
}

/**
 * @brief 写入默认库存到 SD 卡（失败时降级为纯内存模式）
 */
static esp_err_t create_default_inventory(void)
{
    /* 始终先加载到内存 */
    memcpy(s_inventory, s_default_products, sizeof(s_default_products));
    s_inventory_count = DEFAULT_COUNT;

    /* 尝试写入 SD 卡 */
    FILE *f = fopen(INVENTORY_CSV_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "SD card unavailable, using RAM-only inventory (%d products), errno=%d", DEFAULT_COUNT, errno);
        s_sd_available = false;
        return ESP_OK;  /* 降级成功，不返回错误 */
    }

    fprintf(f, "product_name,total_stock,sold_count,price\n");
    for (int i = 0; i < DEFAULT_COUNT; i++) {
        fprintf(f, "%s,%d,%d,%.2f\n",
                s_default_products[i].name,
                s_default_products[i].total_stock,
                s_default_products[i].sold_count,
                (double)s_default_products[i].price);
    }
    fclose(f);
    s_sd_available = true;

    ESP_LOGI(TAG, "Created default inventory on SD card (%d products)", DEFAULT_COUNT);
    return ESP_OK;
}

/**
 * @brief 从 purchase_log.csv 回填 sold_count（历史数据同步）
 */
static void sync_sales_from_log(void)
{
    FILE *f = fopen("/sdcard/purchase_log.csv", "r");
    if (!f) {
        ESP_LOGW(TAG, "No purchase log found, skip sales sync");
        return;
    }

    char line[256];
    int synced = 0;

    /* 跳过 header */
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        long long ts;
        int fid;
        char product[64];
        char price[32];
        if (sscanf(line, "%lld,%d,%63[^,],%31s",
                   &ts, &fid, product, price) >= 3) {
            /* 在库存中查找匹配商品，累加 sold_count */
            for (int i = 0; i < s_inventory_count; i++) {
                if (strcmp(s_inventory[i].name, product) == 0) {
                    s_inventory[i].sold_count++;
                    synced++;
                    break;
                }
            }
        }
    }
    fclose(f);

    if (synced > 0) {
        ESP_LOGI(TAG, "Synced %d sales from purchase log", synced);
        inventory_save_to_sd();
    }
}

/* ── 公共 API ── */

esp_err_t inventory_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* 创建互斥锁 */
    s_inventory_mutex = xSemaphoreCreateMutex();
    if (!s_inventory_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* 尝试加载库存文件 */
    esp_err_t ret = load_inventory_from_sd();
    if (ret != ESP_OK) {
        ret = create_default_inventory();
        if (ret != ESP_OK) {
            vSemaphoreDelete(s_inventory_mutex);
            s_inventory_mutex = NULL;
            return ret;
        }
    }

    s_initialized = true;

    /* 从购买记录回填销量 */
    sync_sales_from_log();

    ESP_LOGI(TAG, "Inventory manager initialized (%d products)", s_inventory_count);
    return ESP_OK;
}

esp_err_t inventory_get_all(product_inventory_t *items, int *count)
{
    if (!s_initialized || !items || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_inventory_mutex, portMAX_DELAY);
    memcpy(items, s_inventory, sizeof(product_inventory_t) * s_inventory_count);
    *count = s_inventory_count;
    xSemaphoreGive(s_inventory_mutex);

    return ESP_OK;
}

esp_err_t inventory_sell_one(const char *product_name)
{
    if (!s_initialized || !product_name) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_inventory_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_inventory_count; i++) {
        if (strcmp(s_inventory[i].name, product_name) == 0) {
            /* 检查库存 */
            if (s_inventory[i].total_stock <= 0) {
                ESP_LOGW(TAG, "Product '%s' out of stock!", product_name);
                ret = ESP_ERR_INVALID_STATE;
                break;
            }
            s_inventory[i].total_stock--;
            s_inventory[i].sold_count++;
            ESP_LOGI(TAG, "Sold 1x '%s': stock=%d, sold=%d",
                     product_name,
                     s_inventory[i].total_stock,
                     s_inventory[i].sold_count);
            ret = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_inventory_mutex);

    /* 写回 SD 卡 */
    if (ret == ESP_OK) {
        inventory_save_to_sd();
    }

    return ret;
}

esp_err_t inventory_restock(const char *product_name, int count)
{
    if (!s_initialized || !product_name || count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_inventory_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_inventory_count; i++) {
        if (strcmp(s_inventory[i].name, product_name) == 0) {
            s_inventory[i].total_stock += count;
            ESP_LOGI(TAG, "Restocked '%s': +%d, total=%d",
                     product_name, count, s_inventory[i].total_stock);
            ret = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_inventory_mutex);

    if (ret == ESP_OK) {
        inventory_save_to_sd();
    }

    return ret;
}

esp_err_t inventory_get_sales_summary(sales_summary_t *summary)
{
    if (!s_initialized || !summary) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(summary, 0, sizeof(sales_summary_t));

    xSemaphoreTake(s_inventory_mutex, portMAX_DELAY);

    for (int i = 0; i < s_inventory_count; i++) {
        summary->total_sales += s_inventory[i].sold_count;
        summary->total_revenue += s_inventory[i].sold_count * s_inventory[i].price;
        summary->today_sales += s_inventory[i].sold_count;  /* 简化：所有销量 */
        summary->today_revenue += s_inventory[i].sold_count * s_inventory[i].price;
    }

    /* 读取注册用户数（从 face_db.csv） */
    FILE *f = fopen("/sdcard/face_db.csv", "r");
    if (f) {
        char line[128];
        if (fgets(line, sizeof(line), f)) { /* skip header */
            while (fgets(line, sizeof(line), f)) {
                if (strlen(line) > 1) summary->total_users++;
            }
        }
        fclose(f);
    }

    xSemaphoreGive(s_inventory_mutex);
    return ESP_OK;
}

/**
 * @brief qsort 比较函数（按销量降序）
 */
static int cmp_sold_desc(const void *a, const void *b)
{
    const product_inventory_t *pa = (const product_inventory_t *)a;
    const product_inventory_t *pb = (const product_inventory_t *)b;
    return pb->sold_count - pa->sold_count;
}

esp_err_t inventory_get_ranking(product_inventory_t *ranking, int *count)
{
    if (!s_initialized || !ranking || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_inventory_mutex, portMAX_DELAY);
    memcpy(ranking, s_inventory, sizeof(product_inventory_t) * s_inventory_count);
    *count = s_inventory_count;
    xSemaphoreGive(s_inventory_mutex);

    /* 按销量降序排序 */
    qsort(ranking, *count, sizeof(product_inventory_t), cmp_sold_desc);

    return ESP_OK;
}

esp_err_t inventory_get_product(const char *product_name, product_inventory_t *item)
{
    if (!s_initialized || !product_name || !item) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_inventory_mutex, portMAX_DELAY);

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_inventory_count; i++) {
        if (strcmp(s_inventory[i].name, product_name) == 0) {
            memcpy(item, &s_inventory[i], sizeof(product_inventory_t));
            ret = ESP_OK;
            break;
        }
    }

    xSemaphoreGive(s_inventory_mutex);
    return ret;
}

#define RECOMMEND_MAX_COUNT  5

esp_err_t inventory_get_recommend(int user_id, recommend_item_t *items, int *count)
{
    if (!s_initialized || !items || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    xSemaphoreTake(s_inventory_mutex, portMAX_DELAY);

    /* ── 第1步：扫描 purchase_log.csv，统计该用户购买频率 ── */
    typedef struct {
        char name[INVENTORY_MAX_NAME_LEN];
        int  freq;
    } freq_entry_t;

    freq_entry_t freq_table[INVENTORY_MAX_PRODUCTS];
    memset(freq_table, 0, sizeof(freq_table));
    int freq_count = 0;

    FILE *f = fopen("/sdcard/purchase_log.csv", "r");
    if (f) {
        char line[256];
        /* 跳过 header */
        if (fgets(line, sizeof(line), f)) {
            while (fgets(line, sizeof(line), f)) {
                long long ts;
                int fid;
                char product[64];
                char price[32];
                if (sscanf(line, "%lld,%d,%63[^,],%31s",
                           &ts, &fid, product, price) >= 3) {
                    if (fid == user_id) {
                        /* 查找或插入频率表 */
                        int found = -1;
                        for (int j = 0; j < freq_count; j++) {
                            if (strcmp(freq_table[j].name, product) == 0) {
                                found = j;
                                break;
                            }
                        }
                        if (found >= 0) {
                            freq_table[found].freq++;
                        } else if (freq_count < INVENTORY_MAX_PRODUCTS) {
                            strncpy(freq_table[freq_count].name, product,
                                    INVENTORY_MAX_NAME_LEN - 1);
                            freq_table[freq_count].name[INVENTORY_MAX_NAME_LEN - 1] = '\0';
                            freq_table[freq_count].freq = 1;
                            freq_count++;
                        }
                    }
                }
            }
        }
        fclose(f);
    }

    /* ── 第2步：按购买频率降序排列（冒泡，数据量小） ── */
    for (int i = 0; i < freq_count - 1; i++) {
        for (int j = i + 1; j < freq_count; j++) {
            if (freq_table[j].freq > freq_table[i].freq) {
                freq_entry_t tmp = freq_table[i];
                freq_table[i] = freq_table[j];
                freq_table[j] = tmp;
            }
        }
    }

    /* ── 第3步：先填历史购买推荐（"常买"） ── */
    for (int i = 0; i < freq_count && *count < RECOMMEND_MAX_COUNT; i++) {
        for (int j = 0; j < s_inventory_count; j++) {
            if (strcmp(s_inventory[j].name, freq_table[i].name) == 0) {
                strncpy(items[*count].product_name, s_inventory[j].name,
                        INVENTORY_MAX_NAME_LEN - 1);
                items[*count].price = s_inventory[j].price;
                items[*count].total_stock = s_inventory[j].total_stock;
                items[*count].sold_count = s_inventory[j].sold_count;
                items[*count].buy_freq = freq_table[i].freq;
                items[*count].recommend_score = freq_table[i].freq * 10;
                strncpy(items[*count].reason, "常买",
                        sizeof(items[*count].reason) - 1);
                (*count)++;
                break;
            }
        }
    }

    /* ── 第4步：不足时用全局销量排行补充（"热销"） ── */
    if (*count < RECOMMEND_MAX_COUNT) {
        product_inventory_t ranking[INVENTORY_MAX_PRODUCTS];
        int rank_count = s_inventory_count;
        memcpy(ranking, s_inventory, sizeof(product_inventory_t) * s_inventory_count);

        /* 按 sold_count 降序排列 */
        for (int i = 0; i < rank_count - 1; i++) {
            for (int j = i + 1; j < rank_count; j++) {
                if (ranking[j].sold_count > ranking[i].sold_count) {
                    product_inventory_t tmp = ranking[i];
                    ranking[i] = ranking[j];
                    ranking[j] = tmp;
                }
            }
        }

        for (int i = 0; i < rank_count && *count < RECOMMEND_MAX_COUNT; i++) {
            /* 跳过已推荐的 */
            bool already = false;
            for (int k = 0; k < *count; k++) {
                if (strcmp(items[k].product_name, ranking[i].name) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) continue;

            strncpy(items[*count].product_name, ranking[i].name,
                    INVENTORY_MAX_NAME_LEN - 1);
            items[*count].price = ranking[i].price;
            items[*count].total_stock = ranking[i].total_stock;
            items[*count].sold_count = ranking[i].sold_count;
            items[*count].buy_freq = 0;
            items[*count].recommend_score = ranking[i].sold_count;
            strncpy(items[*count].reason, "热销",
                    sizeof(items[*count].reason) - 1);
            (*count)++;
        }
    }

    xSemaphoreGive(s_inventory_mutex);

    ESP_LOGI(TAG, "Recommend for user %d: %d items (history=%d)",
             user_id, *count, freq_count);
    return ESP_OK;
}

esp_err_t inventory_save_to_sd(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* SD 卡不可用 → 纯内存模式，跳过持久化 */
    if (!s_sd_available) {
        return ESP_OK;
    }

    xSemaphoreTake(s_inventory_mutex, portMAX_DELAY);

    FILE *f = fopen(INVENTORY_CSV_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "SD card lost, switching to RAM-only mode");
        s_sd_available = false;
        xSemaphoreGive(s_inventory_mutex);
        return ESP_OK;  /* 降级，不返回错误 */
    }

    fprintf(f, "product_name,total_stock,sold_count,price\n");
    for (int i = 0; i < s_inventory_count; i++) {
        fprintf(f, "%s,%d,%d,%.2f\n",
                s_inventory[i].name,
                s_inventory[i].total_stock,
                s_inventory[i].sold_count,
                (double)s_inventory[i].price);
    }
    fclose(f);

    xSemaphoreGive(s_inventory_mutex);
    ESP_LOGI(TAG, "Inventory saved to SD card");
    return ESP_OK;
}
