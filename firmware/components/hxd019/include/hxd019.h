/**
 * @file hxd019.h
 * @brief HXD019EU 红外遥控芯片驱动对外 API（interface 层：UART 收发 + 日志）
 *
 * 物理连接：ESP32-C6 UART（Kconfig: HXD019_UART_NUM/TX/RX，默认 UART1/GPIO4/5）
 *           ↔ HXD019EU 芯片串口，57600bps 8N1。
 *
 * 用法（空调典型流程）：
 *   1. hxd019_init()
 *   2. 绑定码组：hxd019_brand_find("格力",AC) + hxd019_codegroup_get(...) → hxd019_bind_group(830)
 *      或遥控器对码：hxd019_match(match_cb) → 回调得到码组号
 *   3. 便捷函数：hxd019_ac_power(true) / hxd019_ac_temp(26) / hxd019_ac_mode(HXD019_MODE_COOL) ...
 *      （完整状态帧，内部缓存上次状态做增量修改）
 *      或简单命令帧：hxd019_ac_simple(HXD019_AC_FUNC_ON)
 *   4. 非空调：hxd019_send_av_key(fmt, key2b, com4b)
 *
 * 线程模型：send 路径内部有互斥，可多任务调用；回调在 RX 任务上下文执行，
 * 回调内禁止阻塞/长耗时操作（只存数据或发队列）。
 */
#ifndef HXD019_H
#define HXD019_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hxd019_types.h"
#include "hxd019_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 匹配应答回调：芯片上传码组号（如 830）；ctx 为注册时传入的上下文 */
typedef void (*hxd019_match_cb_t)(uint16_t code_group, void *ctx);

/** 学习应答数据回调（芯片学习到遥控器波形后上传的原始数据，协议未细化，透传） */
typedef void (*hxd019_learn_cb_t)(const uint8_t *data, size_t len, void *ctx);

/**
 * 初始化：安装 UART 驱动（57600 8N1）+ 启动 RX 任务。
 * GPIO/UART 号取 Kconfig（HXD019_UART_TX/RX/NUM）。
 * @return HXD019_OK / HXD019_ERR_ARG（参数或驱动安装失败）
 */
hxd019_err_t hxd019_init(void);

/** 反初始化（停 RX 任务 + 卸载驱动；本工程常驻，一般不调） */
void hxd019_deinit(void);

/* ---------------- 帧收发基础 ---------------- */

/** 发送一帧（内部串行化；日志锚点 [HXD019] tx frame: xx xx xx） */
hxd019_err_t hxd019_send_frame(const hxd019_frame_t *frame);

/** 规则一直发：空调简单命令（5B，无状态缓存） */
hxd019_err_t hxd019_ac_simple(uint8_t func_code);

/* ---------------- 空调完整状态便捷函数（规则二/三，16B，带会话缓存） ---------------- */

/** 绑定码组（索引查询或匹配结果），等效 hxd019_session_bind */
hxd019_err_t hxd019_bind_group(uint16_t code_group);

/** 开关机（发完整状态帧，键名=电源） */
hxd019_err_t hxd019_ac_power(bool on);

/** 设温度（16-31℃，发完整状态帧，键名=温度加/减不适用——直接设目标值） */
hxd019_err_t hxd019_ac_temp(uint8_t temp_c);

/** 温度 +/-1（基于会话缓存，越界钳位） */
hxd019_err_t hxd019_ac_temp_step(int8_t delta);

/** 设模式（hxd019_mode_t） */
hxd019_err_t hxd019_ac_mode(uint8_t mode);

/** 设风量（HXD019_FAN_*） */
hxd019_err_t hxd019_ac_fan(uint8_t fan);

/** 手动风向（HXD019_SWING_*） */
hxd019_err_t hxd019_ac_swing(uint8_t swing);

/** 自动（左右）风向开关 */
hxd019_err_t hxd019_ac_lrswing(bool on);

/* 以下仅 11 键帧格式编译时有效；7 键格式下返回 HXD019_ERR_KEY7 */
hxd019_err_t hxd019_ac_sleep(bool on);
hxd019_err_t hxd019_ac_aux_heat(bool on);
hxd019_err_t hxd019_ac_light(bool on);
hxd019_err_t hxd019_ac_eco(bool on);

/** 直发当前状态+指定键名（快捷键：HXD019_KEY_QUICK_COOL 等 0x0C-0x0F） */
hxd019_err_t hxd019_ac_send_key(uint8_t key);

/* ---------------- 非空调（规则四，10B） ---------------- */

/**
 * 发一个非空调键（数据表取值：F_code/KEY 2B/COM 4B）。
 * F_code 特殊字节钩子在 core 会话层（TODO 桩，见 hxd019_session.h）。
 */
hxd019_err_t hxd019_send_av_key(uint8_t fmt_index,
                                const uint8_t key_code[HXD019_AV_KEY_LEN],
                                const uint8_t com_code[HXD019_AV_COM_LEN]);

/* ---------------- 学习/匹配 ---------------- */

/** 发学习命令（30 20 50）；学习数据经 learn_cb 上抛（可 NULL 忽略） */
hxd019_err_t hxd019_start_learn(hxd019_learn_cb_t cb, void *ctx);

/**
 * 发匹配命令（30 70 A0）并注册应答回调；遥控器按键后芯片上传码组号。
 * 回调在 RX 任务上下文执行（禁止阻塞）；成功匹配的码组建议调用方 hxd019_bind_group()。
 * @return HXD019_OK（命令已发，结果异步）
 */
hxd019_err_t hxd019_match(hxd019_match_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
#endif /* HXD019_H */
