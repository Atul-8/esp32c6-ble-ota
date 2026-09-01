/*
 * core 层：OTA 版本回滚确认（issue #2）
 * 规则：core 层禁止 ESP_LOG/printf，状态经返回值/输出参数上报给 interface 层。
 */
#include "ota_rollback.h"

#include <stdbool.h>

#include "esp_err.h"
#include "esp_system.h"   /* esp_get_free_heap_size */
#include "esp_ota_ops.h"  /* esp_ota_get_state_partition / mark_valid / mark_invalid */
#include "esp_partition.h"
#include "nvs.h"

#include "ota_nvs_keys.h"

/* 自检通过所需的最小空闲堆（字节）：50KB */
#define OTA_SELFCHECK_MIN_FREE_HEAP 51200u

bool ota_rollback_selfcheck(void)
{
    bool ok = false;
    nvs_handle_t h = 0;

    /* 1. NVS 读写回路测试 */
    if (nvs_open(OTA_NVS_NAMESPACE_DIAG, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }

    uint8_t w = OTA_SELFTEST_PATTERN;
    uint8_t r = 0;
    if (nvs_set_u8(h, OTA_NVS_KEY_SELFTEST, w) != ESP_OK) {
        goto out;
    }
    if (nvs_commit(h) != ESP_OK) {
        goto out;
    }
    if (nvs_get_u8(h, OTA_NVS_KEY_SELFTEST, &r) != ESP_OK || r != w) {
        goto out;
    }
    ok = true;

out:
    nvs_close(h);

    if (!ok) {
        return false;
    }

    /* 2. 堆水位检查 */
    if (esp_get_free_heap_size() < OTA_SELFCHECK_MIN_FREE_HEAP) {
        return false;
    }

    /* 3. 后续可扩展外设自检 */
    return true;
}

ota_rollback_action_t ota_rollback_confirm(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return OTA_ROLLBACK_NO_ACTION;
    }

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        /* 非 OTA 槽（factory）或状态不可读：无需确认 */
        return OTA_ROLLBACK_NO_ACTION;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        /* 已确认过（VALID）或其他状态：直接放行 */
        return OTA_ROLLBACK_NO_ACTION;
    }

    if (ota_rollback_selfcheck()) {
        if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            return OTA_ROLLBACK_CONFIRMED;
        }
        /* 标记失败极罕见（ota_data 写坏），按不可信处理 */
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return OTA_ROLLBACK_REBOOT_REQUESTED; /* 不可达，reboot 不返回 */
    }

    /* 自检失败：标记无效并回滚重启（不返回） */
    esp_ota_mark_app_invalid_rollback_and_reboot();
    return OTA_ROLLBACK_REBOOT_REQUESTED;
}
