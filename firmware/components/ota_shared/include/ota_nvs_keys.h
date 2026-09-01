/*
 * shared 层：NVS 命名空间与键名常量
 * 依赖约束：本组件只允许 stdint/stddef/string 标准头。
 * 全固件 NVS 键统一在此定义，禁止各层散落硬编码字符串。
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* OTA 断点续传进度（issue #3）—— blob 存储，见 ota_progress_store */
#define OTA_NVS_NAMESPACE        "ota_prog"
#define OTA_NVS_KEY_PROGRESS     "progress"

/* 回滚自检读写回路（issue #2）—— u8 存储 */
#define OTA_NVS_NAMESPACE_DIAG   "ota_diag"
#define OTA_NVS_KEY_SELFTEST     "selftest"
#define OTA_SELFTEST_PATTERN     0xA5u

#ifdef __cplusplus
}
#endif
