/*
 * interface 层：BLE OTA transport（vendor ble_ota 组件桥接）
 *
 * 设计文档：.ai/docs/design-ota-transport-abstraction.md §5.1/§C.1（权威）
 *
 * 职责（自 ota_task.c 迁移改造，REQ-004 PR-1；ota_task.c 已删除）：
 *   - ringbuf 解耦 NimBLE host 回调上下文与落盘（回调不能同步写 flash）
 *   - 泵任务：ringbuf 收 chunk → lazy-open 会话（首个数据才 begin）→ sink write
 *     → 数据收满即 finish+activate(reboot)
 *   - 会话边界（vendor 会话回调）：Start → armed + epoch invalidate（P0-1 主防线）；
 *     Stop/断连 → 只清 armed（收尾竞态见 .c 文件头），泵任务按数据量收尾/abort
 *   - notify_sem 契约：非 static 全局（META-001 隐式契约，见 .c 注释）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 transport：ringbuf + 泵任务 + vendor 会话回调注册。
 * 调用前提：ota_sink_init() 已完成、vendor host_init 前后皆可（本任务不触 BLE 栈）。
 * 命名注意：函数名带 "ble_ota" 前缀——NimBLE host 自带 nimble/transport.h 的
 * ble_transport_init(void)（同名外部符号，链接期冲突），不得裸用 ble_transport_init。
 * 成功返回 true。 */
bool ble_ota_transport_init(uint32_t ringbuf_size);

/* vendor esp_ble_ota_recv_fw_data_callback 注册函数：每满 1 sector（4096B，尾
 * sector 可少）被组件调用一次，数据转送 ringbuf（会话外/被门控时源头上丢弃）。
 * app_main 中传给组件。 */
void ble_ota_transport_recv_cb(uint8_t *buf, uint32_t length);

#ifdef __cplusplus
}
#endif
