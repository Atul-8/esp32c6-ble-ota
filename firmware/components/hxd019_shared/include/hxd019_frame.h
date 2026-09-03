/**
 * @file hxd019_frame.h
 * @brief HXD019EU 帧构建纯函数 API（shared 层）
 *
 * 帧格式选择经编译期宏（由组件 Kconfig 经 sdkconfig.h 注入）：
 *   CONFIG_HXD019_STATE_FMT_11KEY  — 定义时用规则三 11 键帧，否则规则二 7 键帧
 *   CONFIG_HXD019_TEMP_ENC_MINUS16 — 定义时 11 键帧温度字节 = ℃-16（d/du 库）
 * 对外统一提供两个数值型预处理宏：
 *   HXD019_FMT_IS_11KEY            — 1 = 11 键帧 / 0 = 7 键帧
 *   HXD019_TEMP_ENC_MINUS16_EN     — 1 = 温度减 16 编码
 */
#ifndef HXD019_FRAME_H
#define HXD019_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include "hxd019_types.h"
#include "hxd019_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Kconfig → 数值宏（枚举值不能用于 #if，故此解析收口于此） ---- */
#if defined(CONFIG_HXD019_STATE_FMT_11KEY) && CONFIG_HXD019_STATE_FMT_11KEY
#define HXD019_FMT_IS_11KEY 1
#else
#define HXD019_FMT_IS_11KEY 0
#endif

#if defined(CONFIG_HXD019_TEMP_ENC_MINUS16) && CONFIG_HXD019_TEMP_ENC_MINUS16
#define HXD019_TEMP_ENC_MINUS16_EN 1
#else
#define HXD019_TEMP_ENC_MINUS16_EN 0
#endif

/** 校验和：buf 前 len 字节之和的低 8 位 */
uint8_t hxd019_checksum(const uint8_t *buf, size_t len);

/** 规则一：空调简单命令 5B（无校验和）。func_code 用 hxd019_ac_func_t */
hxd019_err_t hxd019_build_ac_simple(uint16_t code_group, uint8_t func_code, hxd019_frame_t *out);

/** 规则二/三：空调完整状态 16B（7 键/11 键由 HXD019_FMT_IS_11KEY 编译期决定，附校验和） */
hxd019_err_t hxd019_build_ac_state(uint16_t code_group,
                                   const hxd019_ac_state_t *state,
                                   hxd019_frame_t *out);

/** 规则四：非空调 10B（F_code + KEY 2B + COM 4B + 校验和，码库数据表取值） */
hxd019_err_t hxd019_build_av(uint8_t fmt_index,
                             const uint8_t key_code[HXD019_AV_KEY_LEN],
                             const uint8_t com_code[HXD019_AV_COM_LEN],
                             hxd019_frame_t *out);

/** 学习命令 3B：30 20 50 */
hxd019_err_t hxd019_build_learn(hxd019_frame_t *out);

/** 匹配命令 3B：30 70 A0（芯片应答码组号，经 interface 层回调上抛） */
hxd019_err_t hxd019_build_match(hxd019_frame_t *out);

#ifdef __cplusplus
}
#endif
#endif /* HXD019_FRAME_H */
