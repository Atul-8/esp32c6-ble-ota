/*
 * shared 层：版本与 BLE 常量
 * 依赖约束：本组件只允许 stdint/stddef/string 标准头，禁止任何 ESP/NimBLE 头。
 *
 * 注意：OTA_APP_VERSION 必须与根 CMakeLists.txt 中的 PROJECT_VER 保持一致
 * （PROJECT_VER 是 esp_app_get_description()->version 的来源，日志锚点以它为准）。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* 固件版本（与根 CMakeLists.txt PROJECT_VER 同步维护） */
#define OTA_APP_VERSION          "1.0.0"

/* BLE 广播名（MAC 后 4 位 1128；组件硬编码 "nimble-ble-ota"，app 在 host_init 后覆盖） */
#define OTA_BLE_DEVICE_NAME      "C6-OTA-1128"

/* ble_ota 组件按 sector 回调，1 sector = 4096 字节 */
#define OTA_SECTOR_SIZE          4096u

#ifdef __cplusplus
}
#endif
