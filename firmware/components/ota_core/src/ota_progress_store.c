/*
 * core 层：OTA 断点续传进度持久化（issue #3）
 * 规则：core 层禁止 ESP_LOG/printf，错误经返回值上报。
 *
 * 存储模型：单个 NVS blob（ota_prog/progress），原子覆盖写。
 * 记录字段与 ble_ota 组件的 sector 粒度对齐（1 sector = 4096B）。
 *
 * 边界结论（读组件源码确认）：ble_ota 组件重启后 cur_sector 从 0 重新计数，
 * 不支持跨重启续传。本模块交付的是：
 *   1) 重连续传的进度落盘支撑（上位机侧可配合 Indicate ACK 的期望 sector 号续传）
 *   2) 进度可观测性（重启后可读出上次传输到哪个 sector）
 */
#include "ota_progress_store.h"

#include <string.h>

#include "esp_err.h"
#include "nvs.h"

#include "ota_nvs_keys.h"

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 结构有效性标识 */
    uint32_t image_size;    /* Start 命令声明的固件总长 */
    uint32_t offset;        /* 已写入字节数（sector 数 * 4096） */
    uint32_t sector_index;  /* 下一个待收 sector 序号 */
} ota_progress_blob_t;

#define OTA_PROGRESS_MAGIC 0x4F545047u /* "OTPG" */

static esp_err_t progress_rw(bool write, uint32_t *image_size, uint32_t *offset, uint32_t *sector_index)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    if (write) {
        ota_progress_blob_t blob = {
            .magic = OTA_PROGRESS_MAGIC,
            .image_size = *image_size,
            .offset = *offset,
            .sector_index = *sector_index,
        };
        err = nvs_set_blob(h, OTA_NVS_KEY_PROGRESS, &blob, sizeof(blob));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
    } else {
        ota_progress_blob_t blob;
        size_t len = sizeof(blob);
        err = nvs_get_blob(h, OTA_NVS_KEY_PROGRESS, &blob, &len);
        if (err == ESP_OK) {
            if (len != sizeof(blob) || blob.magic != OTA_PROGRESS_MAGIC) {
                err = ESP_ERR_INVALID_STATE;
            } else {
                *image_size = blob.image_size;
                *offset = blob.offset;
                *sector_index = blob.sector_index;
            }
        }
    }

    nvs_close(h);
    return err;
}

esp_err_t ota_progress_save(uint32_t image_size, uint32_t offset, uint32_t sector_index)
{
    return progress_rw(true, &image_size, &offset, &sector_index);
}

esp_err_t ota_progress_load(uint32_t *image_size, uint32_t *offset, uint32_t *sector_index)
{
    if (image_size == NULL || offset == NULL || sector_index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return progress_rw(false, image_size, offset, sector_index);
}

esp_err_t ota_progress_clear(void)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_key(h, OTA_NVS_KEY_PROGRESS);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; /* 本来就没有，视为清空成功 */
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}
