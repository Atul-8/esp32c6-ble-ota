/*
 * interface 层：OTA 落盘任务接口
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 创建 ringbuf（app_main 中在注册回调之前调用）。成功返回 true。 */
bool ble_ota_ringbuf_init(uint32_t ringbuf_size);

/* ble_ota 组件回调：每完成 1 个 sector 被调用，数据转送 ringbuf。
 * app_main 中传给 esp_ble_ota_recv_fw_data_callback()。 */
void ota_recv_fw_cb(uint8_t *buf, uint32_t length);

/* ble_ota 组件回调内部使用：把组件回调数据塞入 ringbuf */
size_t ota_write_to_ringbuf(const uint8_t *data, size_t size);

/* 创建 ota_task（host_init + 回调注册完成后调用） */
void ota_task_init(void);

/* ota_task 主体（FreeRTOS 任务入口） */
void ota_task(void *arg);

#ifdef __cplusplus
}
#endif
