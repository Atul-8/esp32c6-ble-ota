/*
 * interface 层：OTA 落盘任务
 * 改造自 espressif/ble_ota v0.1.17 官方 example 的 ota_task：
 *   - ringbuf 解耦 BLE 收包与 flash 写入
 *   - 每完成 1 个 sector（4096B）调 ota_progress_save（issue #3）
 *   - OTA 完成激活后 ota_progress_clear
 *   - 日志锚点格式固定，供上位机联调 grep
 */
#include "ota_task.h"

#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "ble_ota.h"

#include "ota_version.h"
#include "ota_progress_store.h"

#define OTA_RINGBUF_SIZE  8192
#define OTA_TASK_SIZE     8192
#define OTA_TASK_PRIORITY 5

static const char *TAG = "BLE_OTA";

static RingbufHandle_t s_ringbuf = NULL;
static esp_ota_handle_t s_out_handle;
/* 注意：必须是非 static 全局——ble_ota 组件 Stop 命令处理里
 * `extern SemaphoreHandle_t notify_sem` 反向引用 app 侧此符号（组件隐式契约） */
SemaphoreHandle_t notify_sem = NULL;
static esp_partition_t s_target_partition;

bool ble_ota_ringbuf_init(uint32_t ringbuf_size)
{
    s_ringbuf = xRingbufferCreate(ringbuf_size, RINGBUF_TYPE_BYTEBUF);
    return s_ringbuf != NULL;
}

size_t ota_write_to_ringbuf(const uint8_t *data, size_t size)
{
    BaseType_t done = xRingbufferSend(s_ringbuf, (void *)data, size, (TickType_t)portMAX_DELAY);
    return done ? size : 0;
}

/* ble_ota 组件回调：每完成 1 个 sector（4096B，尾 sector 可少）调用一次 */
void ota_recv_fw_cb(uint8_t *buf, uint32_t length)
{
    (void)ota_write_to_ringbuf(buf, length);
}

void ota_task(void *arg)
{
    uint32_t recv_len = 0;
    uint32_t saved_sectors = 0;
    uint8_t *data = NULL;
    size_t item_size = 0;
    /* ERR-007 修复：fw_length 由 BLE Start 命令在本任务启动之后才写入（唯一写入时机），
     * 禁止启动时缓存——必须在数据分支内实时调用 esp_ble_ota_get_fw_length()（example 同款语义） */

    notify_sem = xSemaphoreCreateCounting(100, 0);
    xSemaphoreGive(notify_sem);

    /* 选目标槽：running 的下一个 OTA 槽（example 同款逻辑，适配无 factory 双槽表） */
    esp_partition_t *partition_ptr = (esp_partition_t *)esp_ota_get_boot_partition();
    if (partition_ptr == NULL || partition_ptr->type != ESP_PARTITION_TYPE_APP) {
        ESP_LOGE(TAG, "boot partition NULL or not app");
        goto OTA_ERROR;
    }

    {
        esp_partition_subtype_t target_subtype;
        const esp_partition_t *next = esp_ota_get_next_update_partition(partition_ptr);
        target_subtype = next ? next->subtype : ESP_PARTITION_SUBTYPE_APP_OTA_0;

        partition_ptr = (esp_partition_t *)esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, target_subtype, NULL);
        if (partition_ptr == NULL) {
            ESP_LOGE(TAG, "target partition (subtype %d) not found", target_subtype);
            goto OTA_ERROR;
        }
        memcpy(&s_target_partition, partition_ptr, sizeof(esp_partition_t));
    }

    if (esp_ota_begin(&s_target_partition, OTA_SIZE_UNKNOWN, &s_out_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed");
        goto OTA_ERROR;
    }

    ESP_LOGI(TAG, "ota_task ready, target=%s, wait Start cmd", s_target_partition.label);

    for (;;) {
        data = (uint8_t *)xRingbufferReceive(s_ringbuf, &item_size, (TickType_t)portMAX_DELAY);
        if (data == NULL) {
            continue;
        }

        xSemaphoreTake(notify_sem, portMAX_DELAY);

        if (item_size != 0) {
            if (esp_ota_write(s_out_handle, (const void *)data, item_size) != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed");
                vRingbufferReturnItem(s_ringbuf, (void *)data);
                xSemaphoreGive(notify_sem);
                goto OTA_ERROR;
            }
            recv_len += item_size;
            vRingbufferReturnItem(s_ringbuf, (void *)data);

            /* 实时读 fw_length：Start 命令晚于本任务启动（ERR-007） */
            uint32_t fw_len = esp_ble_ota_get_fw_length();

            /* 每收满 1 个 sector 持久化一次进度（issue #3） */
            while (recv_len - saved_sectors * OTA_SECTOR_SIZE >= OTA_SECTOR_SIZE) {
                saved_sectors++;
                esp_err_t perr = ota_progress_save(fw_len, recv_len, saved_sectors);
                ESP_LOGI(TAG, "progress saved: sector=%" PRIu32 ", offset=%" PRIu32 "%s",
                         saved_sectors, saved_sectors * OTA_SECTOR_SIZE,
                         (perr == ESP_OK) ? "" : " (nvs err)");
            }

            if (recv_len >= fw_len) {
                xSemaphoreGive(notify_sem);
                break;
            }
        } else {
            vRingbufferReturnItem(s_ringbuf, (void *)data);
        }
        /* ERR-009 修复：正常路径循环体末尾无条件 give（对齐 example ota_task）。
         * count 恒为 0/1，绝不累积：失败/完成路径的额外 give 只发生在任务
         * 即将退出（break/goto）时，此后循环不再运行。 */
        xSemaphoreGive(notify_sem);
    }

    if (esp_ota_end(s_out_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed");
        goto OTA_ERROR;
    }

    if (esp_ota_set_boot_partition(&s_target_partition) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed");
        goto OTA_ERROR;
    }

    ESP_LOGI(TAG, "OTA done: activate %s, rebooting", s_target_partition.label);
    (void)ota_progress_clear();
    vSemaphoreDelete(notify_sem);
    esp_restart();

OTA_ERROR:
    ESP_LOGE(TAG, "OTA failed");
    vTaskDelete(NULL);
}

void ota_task_init(void)
{
    xTaskCreate(&ota_task, "ota_task", OTA_TASK_SIZE, NULL, OTA_TASK_PRIORITY, NULL);
}
