/*
 * core 层：OTA 版本回滚确认（issue #2）
 *
 * 背景：CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 下，新 app 首次启动处于
 * ESP_OTA_IMG_PENDING_VERIFY 状态；该状态下 esp_ota_begin 会拒绝写入，
 * 必须先确认有效。确认动作必须在 BLE 初始化之前完成。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_ROLLBACK_NO_ACTION = 0,      /* 非 PENDING_VERIFY（已是 VALID 等），无需动作 */
    OTA_ROLLBACK_CONFIRMED,          /* 自检通过，已标记有效取消回滚 */
    OTA_ROLLBACK_REBOOT_REQUESTED,   /* 自检失败，已请求回滚重启（不会返回） */
} ota_rollback_action_t;

/*
 * 自检：NVS 读写回路 + 空闲堆 > 50KB。
 * 返回 true 表示当前运行环境可信。
 */
bool ota_rollback_selfcheck(void);

/*
 * 回滚确认：读 running partition 状态，
 *   - PENDING_VERIFY + 自检通过  -> esp_ota_mark_app_valid_cancel_rollback()
 *   - PENDING_VERIFY + 自检失败  -> esp_ota_mark_app_invalid_rollback_and_reboot()（不返回）
 *   - 其��状态                   -> 无动作
 * 返回本次执行的动作，供 interface 层打日志。
 */
ota_rollback_action_t ota_rollback_confirm(void);

#ifdef __cplusplus
}
#endif
