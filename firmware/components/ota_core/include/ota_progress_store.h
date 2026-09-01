/*
 * core 层：OTA 断点续传进度持久化（issue #3）
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 保存进度（每完成 1 个 sector 调用一次）。
 * @param image_size   Start 命令声明的固件总长
 * @param offset       已写入字节数
 * @param sector_index 已完成的 sector 数（下一个 sector 序号）
 */
esp_err_t ota_progress_save(uint32_t image_size, uint32_t offset, uint32_t sector_index);

/* 读取上次会话进度。无记录返回 ESP_ERR_NVS_NOT_FOUND。 */
esp_err_t ota_progress_load(uint32_t *image_size, uint32_t *offset, uint32_t *sector_index);

/* OTA 成功激活后清空进度记录。键不存在视为成功。 */
esp_err_t ota_progress_clear(void);

#ifdef __cplusplus
}
#endif
