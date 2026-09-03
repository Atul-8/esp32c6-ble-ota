/**
 * @file hxd019_types.h
 * @brief HXD019EU 协议公共类型（shared 层）
 */
#ifndef HXD019_TYPES_H
#define HXD019_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 最大帧长：16B（空调完整状态帧） */
#define HXD019_FRAME_MAX 16U

/** 构建出的待发帧 */
typedef struct {
    uint8_t buf[HXD019_FRAME_MAX];
    size_t  len;
} hxd019_frame_t;

/** 构建/解析结果错误码 */
typedef enum {
    HXD019_OK = 0,
    HXD019_ERR_ARG = -1,     /* 参数非法（空指针/长度不足） */
    HXD019_ERR_TEMP = -2,    /* 温度越界 */
    HXD019_ERR_STATE = -3,   /* 状态字段越界（风量/风向/开关/键名/模式） */
    HXD019_ERR_KEY7 = -4,    /* 7 键版不支持该键名（仅 0x01-0x07） */
    HXD019_ERR_FMT = -5,     /* 未知帧格式 */
    HXD019_ERR_CHK = -6,     /* 校验和不符 */
    HXD019_ERR_NOENT = -7,   /* 码库中未找到品牌/码组 */
} hxd019_err_t;

/**
 * 空调完整状态（规则二 7 键 / 规则三 11 键，16B 帧）
 * 温度直接用摄氏度整数，字节编码由帧格式与 CONFIG_HXD019_TEMP_ENC_MINUS16 决定
 */
typedef struct {
    uint8_t temp_c;        /* 摄氏度（7 键版 19-30，11 键版 16-31），默认 25 */
    uint8_t fan;           /* HXD019_FAN_*，默认自动 */
    uint8_t swing;         /* HXD019_SWING_*（手动风向），默认中 */
    uint8_t swing_auto;    /* HXD019_LRSWING_*，默认开 */
    uint8_t power;         /* HXD019_POWER_*，默认关 */
    uint8_t key;           /* hxd019_key_name_t：本次按下的键 */
    uint8_t mode;          /* hxd019_mode_t，默认自动 */
    /* 以下 4 字段仅 11 键版有效，7 键版忽略 */
    uint8_t sleep;         /* 0 关 / 1 开，默认关 */
    uint8_t aux_heat;      /* 辅热 0/1，默认关 */
    uint8_t light;         /* 灯光 0/1，默认开（datasheet：只有灯光默认开） */
    uint8_t eco;           /* 节能 0/1，默认关 */
} hxd019_ac_state_t;

/** 完整状态帧风格（决定 16B 帧状态段 7B+固定尾 或 11B） */
typedef enum {
    HXD019_STATE_FMT_7KEY  = 7,   /* 规则二：无睡眠/辅热/灯光/节能，尾 03 00 00 FF */
    HXD019_STATE_FMT_11KEY = 11,  /* 规则三：含睡眠/辅热/灯光/节能，无固定尾 */
} hxd019_state_fmt_t;

/** 设备类型（码库索引用） */
typedef enum {
    HXD019_DEV_AC   = 0,  /* 空调（g_remote_arc_info） */
    HXD019_DEV_IPTV = 1,  /* 网络机顶盒（remote_IPTV_info） */
    HXD019_DEV_TV   = 2,  /* 电视机（TV_info） */
} hxd019_dev_type_t;

/** 按默认值填充状态结构（datasheet 默认：25℃/自动风/中摆/自动摆开/关机/电源键/自动模式，灯光开） */
void hxd019_ac_state_default(hxd019_ac_state_t *def);

#ifdef __cplusplus
}
#endif
#endif /* HXD019_TYPES_H */
