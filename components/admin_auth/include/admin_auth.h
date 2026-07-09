/*
 * Admin Authentication Component
 * Password verification and management via SD card config file
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 文件路径 ── */
#define ADMIN_AUTH_CONFIG_PATH   "/sdcard/admin.cfg"
#define ADMIN_AUTH_DEFAULT_PWD   "admin123"

/* ── API 函数 ── */

/**
 * @brief 初始化管理员认证模块
 *        如 SD 卡上无 admin.cfg，自动创建并写入默认密码 "admin123" 的 SHA256 哈希
 * @return ESP_OK 成功
 */
esp_err_t admin_auth_init(void);

/**
 * @brief 验证管理员密码
 * @param password  明文密码
 * @return true 密码正确，false 密码错误
 */
bool admin_auth_verify(const char *password);

/**
 * @brief 修改管理员密码
 * @param old_password  旧密码（明文）
 * @param new_password  新密码（明文）
 * @return ESP_OK 修改成功，ESP_ERR_INVALID_ARG 旧密码错误
 */
esp_err_t admin_auth_change_password(const char *old_password, const char *new_password);

/**
 * @brief 重置为默认密码（调试用）
 * @return ESP_OK
 */
esp_err_t admin_auth_reset_to_default(void);

#ifdef __cplusplus
}
#endif
