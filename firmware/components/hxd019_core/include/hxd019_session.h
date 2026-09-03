/**
 * @file hxd019_session.h
 * @brief HXD019EU 会话状态机（core 层）：绑定码组 + 空调状态缓存
 *
 * 职责：
 *  - 记录当前绑定码组（来自索引查询或匹配应答）
 *  - 缓存上一次完整状态帧的状态段（"只改温度"类增量操作直接改字段重发）
 *  - 特殊 F_code 处理钩子（datasheet 引用 remote_data_drv3010_V1.2.c
 *    custom_send_command_data()，未给实现——TODO 桩，见钩子注释）
 *
 * 线程模型：单线程使用（调用方保证，interface 层串行化）；无动态内存。
 */
#ifndef HXD019_SESSION_H
#define HXD019_SESSION_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "hxd019_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** F_code 特殊字节处理钩子（TODO 桩）
 *
 * datasheet（规则四/码库 V7 头注释）："每次按键要查一下 F_code 字节是否为特殊字节，
 * 参见函数 <红外码数据发码处理函数 remote_data_drv3010_V1.2.c> custom_send_command_data()"。
 * 该参考实现 datasheet 未提供。当前桩：对全部 F_code 返回 false（按普通字节直发）。
 * 真机联调若出现特定组（如 F_code=0x3C/0x64/0x72 等非 0x01 组）失灵，
 * 在此钩子内补特殊组映射/双发逻辑，不要改帧构建纯函数。
 */
typedef bool (*hxd019_fcode_hook_t)(uint8_t fmt_index);

/** 会话（约 40B，可静态分配在调用方） */
typedef struct {
    uint16_t code_group;          /* 当前绑定码组号（0 = 未绑定） */
    bool has_state;               /* last_state 是否有效 */
    hxd019_ac_state_t last_state; /* 上一次完整状态（增量操作基准） */
    hxd019_fcode_hook_t fcode_hook; /* 特殊 F_code 钩子，NULL = 用默认桩 */
} hxd019_session_t;

/** 初始化会话（清绑定与状态缓存，注册可选钩子） */
void hxd019_session_init(hxd019_session_t *s, hxd019_fcode_hook_t hook);

/** 绑定码组（索引查询或匹配应答后调用），同时清状态缓存（换组后旧状态无意义） */
void hxd019_session_bind(hxd019_session_t *s, uint16_t code_group);

/** 当前绑定码组；未绑定时返回 0 */
uint16_t hxd019_session_group(const hxd019_session_t *s);

/**
 * 取"基准状态"：有缓存返回缓存副本；无缓存返回 datasheet 默认状态。
 * 返回的 is_default 告知调用方是否为默认值（供上层决定是否先发对码帧）。
 */
void hxd019_session_base_state(hxd019_session_t *s, hxd019_ac_state_t *out, bool *is_default);

/** 提交新状态（发帧成功后由调用方调用，更新缓存） */
void hxd019_session_commit(hxd019_session_t *s, const hxd019_ac_state_t *st);

/** 解绑（清码组与状态缓存） */
void hxd019_session_unbind(hxd019_session_t *s);

#ifdef __cplusplus
}
#endif
#endif /* HXD019_SESSION_H */
