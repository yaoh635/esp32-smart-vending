/*
 * Admin Authentication Component Implementation
 * Password stored as SHA256 hash on SD card
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "admin_auth.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* mbedTLS — ESP-IDF 内置 */
#include "mbedtls/sha256.h"

/* FreeRTOS */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "admin_auth";

/* ── SHA256 哈希长度（十六进制字符串） ── */
#define SHA256_HEX_LEN  64   /* 32 字节 → 64 个十六进制字符 */

/* ── 互斥锁 ── */
static SemaphoreHandle_t s_mutex = NULL;
static bool s_initialized = false;
static bool s_sd_available = false;   /* SD 卡是否可用 */
static char s_ram_hash[SHA256_HEX_LEN + 1] = {0};  /* 内存中的密码哈希 */

/* ── 内部辅助 ── */

/**
 * @brief 计算字符串的 SHA256 哈希，输出十六进制字符串
 */
static void sha256_hex(const char *input, char *output, size_t output_len)
{
    unsigned char hash[32];
    mbedtls_sha256_context ctx;

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); /* 0 = SHA-256, 非 SHA-224 */
    mbedtls_sha256_update(&ctx, (const unsigned char *)input, strlen(input));
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    /* 转为十六进制字符串 */
    for (int i = 0; i < 32 && (i * 2 + 1) < (int)output_len; i++) {
        snprintf(output + i * 2, output_len - i * 2, "%02x", hash[i]);
    }
}

/**
 * @brief 从 SD 卡读取存储的哈希值
 * @param hash_out  输出缓冲区（至少 SHA256_HEX_LEN + 1 字节）
 * @return true 读取成功，false 文件不存在或读取失败
 */
static bool read_stored_hash(char *hash_out, size_t len)
{
    FILE *f = fopen(ADMIN_AUTH_CONFIG_PATH, "r");
    if (!f) return false;

    if (!fgets(hash_out, (int)len, f)) {
        fclose(f);
        return false;
    }

    /* 去除末尾换行符 */
    size_t slen = strlen(hash_out);
    while (slen > 0 && (hash_out[slen - 1] == '\n' || hash_out[slen - 1] == '\r')) {
        hash_out[--slen] = '\0';
    }

    fclose(f);
    return (slen == SHA256_HEX_LEN);
}

/**
 * @brief 将哈希值写入 SD 卡
 */
static esp_err_t write_stored_hash(const char *hash)
{
    FILE *f = fopen(ADMIN_AUTH_CONFIG_PATH, "w");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open admin.cfg for writing");
        return ESP_FAIL;
    }

    fprintf(f, "%s\n", hash);
    fclose(f);
    return ESP_OK;
}

/* ── 公共 API ── */

esp_err_t admin_auth_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    /* 计算默认密码哈希（始终需要，SD 或内存模式都用） */
    sha256_hex(ADMIN_AUTH_DEFAULT_PWD, s_ram_hash, sizeof(s_ram_hash));

    /* 检查 admin.cfg 是否存在 */
    struct stat st;
    if (stat(ADMIN_AUTH_CONFIG_PATH, &st) != 0) {
        /* 文件不存在 — 尝试创建 */
        esp_err_t ret = write_stored_hash(s_ram_hash);
        if (ret != ESP_OK) {
            /* SD 卡不可用 → 降级为纯内存模式 */
            ESP_LOGW(TAG, "SD card unavailable, using RAM-only auth (default password)");
            s_sd_available = false;
            s_initialized = true;
            return ESP_OK;
        }
        ESP_LOGI(TAG, "Created admin.cfg with default password hash");
    }

    s_sd_available = true;
    s_initialized = true;
    ESP_LOGI(TAG, "Admin auth initialized (SD card)");
    return ESP_OK;
}

bool admin_auth_verify(const char *password)
{
    if (!s_initialized || !password) return false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool match = false;

    if (!s_sd_available) {
        /* 纯内存模式：直接比较默认密码哈希 */
        char input_hash[SHA256_HEX_LEN + 1];
        sha256_hex(password, input_hash, sizeof(input_hash));
        match = (strcmp(s_ram_hash, input_hash) == 0);
    } else {
        /* SD 卡模式：读取文件中的哈希 */
        char stored_hash[SHA256_HEX_LEN + 1];
        if (!read_stored_hash(stored_hash, sizeof(stored_hash))) {
            ESP_LOGE(TAG, "Failed to read stored hash, falling back to RAM");
            char input_hash[SHA256_HEX_LEN + 1];
            sha256_hex(password, input_hash, sizeof(input_hash));
            match = (strcmp(s_ram_hash, input_hash) == 0);
        } else {
            char input_hash[SHA256_HEX_LEN + 1];
            sha256_hex(password, input_hash, sizeof(input_hash));
            match = (strcmp(stored_hash, input_hash) == 0);
        }
    }

    if (match) {
        ESP_LOGI(TAG, "Admin password verified OK");
    } else {
        ESP_LOGW(TAG, "Admin password verification FAILED");
    }

    xSemaphoreGive(s_mutex);
    return match;
}

esp_err_t admin_auth_change_password(const char *old_password, const char *new_password)
{
    if (!s_initialized || !old_password || !new_password) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(new_password) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* 验证旧密码（复用 RAM hash 或 SD 文件） */
    char stored_hash[SHA256_HEX_LEN + 1];
    const char *valid_hash = s_ram_hash;  /* 默认用 RAM 中的默认哈希 */

    if (s_sd_available) {
        if (read_stored_hash(stored_hash, sizeof(stored_hash))) {
            valid_hash = stored_hash;
        }
    }

    char old_hash[SHA256_HEX_LEN + 1];
    sha256_hex(old_password, old_hash, sizeof(old_hash));

    if (strcmp(valid_hash, old_hash) != 0) {
        ESP_LOGW(TAG, "Old password mismatch, change rejected");
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    /* 计算新密码哈希 */
    char new_hash[SHA256_HEX_LEN + 1];
    sha256_hex(new_password, new_hash, sizeof(new_hash));

    /* 尝试写入 SD 卡，失败则仅更新内存 */
    esp_err_t ret = ESP_OK;
    if (s_sd_available) {
        ret = write_stored_hash(new_hash);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SD write failed, password change in RAM only");
            s_sd_available = false;
        }
    }

    /* 更新 RAM 中的哈希 */
    memcpy(s_ram_hash, new_hash, sizeof(s_ram_hash));

    if (ret == ESP_OK || !s_sd_available) {
        ESP_LOGI(TAG, "Admin password changed successfully (%s)",
                 s_sd_available ? "SD+RAM" : "RAM only");
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;  /* 内存模式始终成功 */
}

esp_err_t admin_auth_reset_to_default(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    char default_hash[SHA256_HEX_LEN + 1];
    sha256_hex(ADMIN_AUTH_DEFAULT_PWD, default_hash, sizeof(default_hash));
    esp_err_t ret = write_stored_hash(default_hash);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Admin password reset to default");
    }

    xSemaphoreGive(s_mutex);
    return ret;
}
