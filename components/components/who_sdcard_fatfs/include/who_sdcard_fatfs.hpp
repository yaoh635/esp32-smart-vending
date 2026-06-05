#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t fatfs_sd_mount(void);
esp_err_t fatfs_sd_unmount(void);

#ifdef __cplusplus
}
#endif
