/**
 * @file hxd019_protocol.h
 * @brief HXD019EU 红外遥控芯片串口协议常量（shared 层，零硬件依赖）
 *
 * 来源：docs/HXD019EU红外遥控器芯片相关datasheet/
 *   - MinerU_markdown_HXD019E-EU（N和L）语音接口命令_接口V1.5_*.md（串口命令表）
 *   - 美的-格力-海尔-奥克斯-码库体验版_EU(N-L)-总规则-V6.c（四种发码规则，权威）
 *   - 019-免费码库-NME.ARC_TV_IPTV_table_V7.c.b-1.c（码库数据表）
 *
 * 物理层：UART 57600bps 8N1（接口 V1.5）
 *
 * 帧格式（四种发码规则 + 学习/匹配）：
 *   规则一 空调简单命令 5B：30 06 [码组:2B 大端] [功能:1B]          （无校验和）
 *   规则二 空调完整状态 16B（7 键）：30 01 [码组:2B] [状态7B] 03 00 00 FF [校验1B]
 *   规则三 空调完整状态 16B（11 键）：30 01 [码组:2B] [状态11B] [校验1B]
 *   规则四 非空调 10B：30 00 [F_code:1B] [KEY:2B] [COM:4B] [校验1B]
 *   学习 3B：30 20 50；匹配 3B：30 70 A0（芯片回传码组号，如 830）
 *   校验和 = 帧内校验字节之前所有字节之和的低 8 位（发送方自算）
 */
#ifndef HXD019_PROTOCOL_H
#define HXD019_PROTOCOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 命令字（帧头两字节） ---------------- */
#define HXD019_HDR_AC_SIMPLE    0x30, 0x06  /* 规则一 */
#define HXD019_HDR_AC_STATE     0x30, 0x01  /* 规则二/三 */
#define HXD019_HDR_AV           0x30, 0x00  /* 规则四 */
#define HXD019_LEARN_CODE1      0x30
#define HXD019_LEARN_CODE2      0x20
#define HXD019_LEARN_CODE3      0x50        /* 学习：30 20 50 */
#define HXD019_MATCH_CODE2      0x70
#define HXD019_MATCH_CODE3      0xA0        /* 匹配：30 70 A0 */

/* ---------------- 帧长 ---------------- */
#define HXD019_LEN_AC_SIMPLE    5U
#define HXD019_LEN_AC_STATE     16U
#define HXD019_LEN_AV           10U
#define HXD019_LEN_LEARN        3U
#define HXD019_LEN_MATCH        3U

/* 规则二（7 键）固定尾（V6.c：0x03 0x00 0x00 0xFF，DU/D 版芯片固定） */
#define HXD019_AC7_TAIL0        0x03
#define HXD019_AC7_TAIL1        0x00
#define HXD019_AC7_TAIL2        0x00
#define HXD019_AC7_TAIL3        0xFF

/* ---------------- 规则一：空调功能码（019EU(N) 命令列表，V6.c 55-101 行） ---------------- */
typedef enum {
    HXD019_AC_FUNC_OFF            = 0x80,  /* 关闭空调 */
    HXD019_AC_FUNC_ON             = 0x81,  /* 打开空调 */
    HXD019_AC_FUNC_MODE_AUTO      = 0xA1,  /* 模式自动 */
    HXD019_AC_FUNC_MODE_COOL      = 0xA2,  /* 模式制冷 */
    HXD019_AC_FUNC_MODE_DRY       = 0xA3,  /* 模式抽湿 */
    HXD019_AC_FUNC_MODE_FAN       = 0xA4,  /* 模式送风 */
    HXD019_AC_FUNC_MODE_HEAT      = 0xA5,  /* 模式加热 */
    HXD019_AC_FUNC_TEMP_16        = 0x40,  /* 16℃ ... 0x4F=31℃（字节 = 0x40 + ℃-16） */
    HXD019_AC_FUNC_TEMP_31        = 0x4F,
    HXD019_AC_FUNC_FAN_AUTO       = 0x51,  /* 自动风量 */
    HXD019_AC_FUNC_FAN_LOW        = 0x52,  /* 风量低 */
    HXD019_AC_FUNC_FAN_MID        = 0x53,  /* 风量中 */
    HXD019_AC_FUNC_FAN_HIGH       = 0x54,  /* 风量高 */
    HXD019_AC_FUNC_SWING_UP       = 0x61,  /* 风向向上（上下扫风） */
    HXD019_AC_FUNC_SWING_MID      = 0x62,  /* 风向中 */
    HXD019_AC_FUNC_SWING_DOWN     = 0x63,  /* 风向向下（上下停摆） */
    HXD019_AC_FUNC_LRSWING_OFF    = 0x70,  /* 自动风向关闭（左右停摆） */
    HXD019_AC_FUNC_LRSWING_ON     = 0x71,  /* 自动风向打开（左右摆风） */
    HXD019_AC_FUNC_SLEEP_OFF      = 0xB0,  /* 睡眠关 */
    HXD019_AC_FUNC_SLEEP_ON       = 0xB1,  /* 睡眠开 */
    HXD019_AC_FUNC_HEAT_OFF       = 0xC0,  /* 辅热关 */
    HXD019_AC_FUNC_HEAT_ON        = 0xC1,  /* 辅热开 */
    HXD019_AC_FUNC_LIGHT_OFF      = 0xD0,  /* 灯光关 */
    HXD019_AC_FUNC_LIGHT_ON       = 0xD1,  /* 灯光开 */
    HXD019_AC_FUNC_ECO_OFF        = 0xE0,  /* 节能关 */
    HXD019_AC_FUNC_ECO_ON         = 0xE1,  /* 节能开 */
    HXD019_AC_FUNC_TEMP_DEC       = 0x96,  /* 温度减一度 */
    HXD019_AC_FUNC_TEMP_INC       = 0x97,  /* 温度加一度 */
    HXD019_AC_FUNC_QUICK_COOL     = 0x9C,  /* 快速制冷 */
    HXD019_AC_FUNC_QUICK_HEAT     = 0x9D,  /* 快速制热 */
    HXD019_AC_FUNC_MUTE           = 0x9E,  /* 静音 */
    HXD019_AC_FUNC_STRONG         = 0x9F,  /* 强经（强力） */
} hxd019_ac_func_t;

/* ---------------- 完整状态帧：状态段编码（V6.c 规则二/三 7B/11B 注释） ---------------- */

/* 温度范围（摄氏度，两版帧格式不同） */
#define HXD019_TEMP7_MIN_C   19   /* 7 键版（规则二）：19-30℃ */
#define HXD019_TEMP7_MAX_C   30
#define HXD011_TEMP11_MIN_C  16   /* 11 键版（规则三）：16-31℃ */
#define HXD011_TEMP11_MAX_C  31
#define HXD019_TEMP_MIN_C    HXD011_TEMP11_MIN_C  /* 全局边界（最宽范围） */
#define HXD019_TEMP_MAX_C    HXD011_TEMP11_MAX_C
#define HXD019_TEMP_DEFAULT  25   /* datasheet：默认 25 度 = 0x19（两版通用） */

/*
 * 11 键帧温度字段编码（库版本差异，datasheet 规则三原文注释：
 *   "0x10//16度 ... 0x1f//31度  如果是用d，du的库，这个数据要减16"）：
 *   默认（V7/新库）：字节 = ℃（16℃→0x10，25℃→0x19，31℃→0x1F）
 *   d/du 库（编译期选 CONFIG_HXD019_TEMP_ENC_MINUS16）：字节 = ℃ - 16
 * 7 键帧（规则二）表值明确 19℃→0x13 ... 30℃→0x1E（字节 = ℃），不受此项影响。
 * 注：金标准以 datasheet 表值为准，d/du 差异待真机联调确认（疑点见组件 README）。
 * 宏解析（CONFIG → HXD019_TEMP_ENC_MINUS16_EN）收口在 hxd019_frame.h，此处仅注释说明。
 */

/* 风量（状态段字节 1） */
#define HXD019_FAN_AUTO    0x01
#define HXD019_FAN_LOW     0x02
#define HXD019_FAN_MID     0x03
#define HXD019_FAN_HIGH    0x04
#define HXD019_FAN_DEFAULT HXD019_FAN_AUTO

/* 手动风向（状态段字节 2） */
#define HXD019_SWING_UP      0x01  /* 向上（上下扫风） */
#define HXD019_SWING_MID     0x02  /* 中（默认 02） */
#define HXD019_SWING_DOWN    0x03  /* 向下（上下停扫） */
#define HXD019_SWING_DEFAULT HXD019_SWING_MID

/* 自动风向（状态段字节 3） */
#define HXD019_LRSWING_OFF     0x00
#define HXD019_LRSWING_ON      0x01  /* 默认开 */
#define HXD019_LRSWING_DEFAULT HXD019_LRSWING_ON

/* 开关（状态段字节 4） */
#define HXD019_POWER_OFF 0x00
#define HXD019_POWER_ON  0x01

/* 键名（状态段字节 5，15 键；7 键版仅 0x01-0x07） */
typedef enum {
    HXD019_KEY_POWER      = 0x01,  /* 电源 */
    HXD019_KEY_MODE       = 0x02,  /* 模式 */
    HXD019_KEY_FAN        = 0x03,  /* 风量 */
    HXD019_KEY_SWING      = 0x04,  /* 手动风向 */
    HXD019_KEY_LRSWING    = 0x05,  /* 自动风向 */
    HXD019_KEY_TEMP_INC   = 0x06,  /* 温度加 */
    HXD019_KEY_TEMP_DEC   = 0x07,  /* 温度减 */
    HXD019_KEY_SLEEP      = 0x08,  /* 睡眠：风量小、风向上、温度 26（上层要对应） */
    HXD019_KEY_HEAT       = 0x09,  /* 辅热，改指定值 */
    HXD019_KEY_LIGHT      = 0x0A,  /* 灯光，改指定值 */
    HXD019_KEY_ECO        = 0x0B,  /* 节能 */
    HXD019_KEY_QUICK_COOL = 0x0C,  /* 快捷制冷：芯片默认相关值，与上层主控值无关 */
    HXD019_KEY_QUICK_HEAT = 0x0D,  /* 快捷制热 */
    HXD019_KEY_MUTE       = 0x0E,  /* 静音：风量小、手动向上、自动关 */
    HXD019_KEY_STRONG     = 0x0F,  /* 强经：风速最大、风向下，制冷 18 度/制热 30 度 */
} hxd019_key_name_t;

/* 模式（状态段字节 6） */
typedef enum {
    HXD019_MODE_AUTO = 0x01,  /* 自动（默认） */
    HXD019_MODE_COOL = 0x02,  /* 制冷 */
    HXD019_MODE_DRY  = 0x03,  /* 抽湿 */
    HXD019_MODE_FAN  = 0x04,  /* 送风 */
    HXD019_MODE_HEAT = 0x05,  /* 制热 */
} hxd019_mode_t;
#define HXD019_MODE_DEFAULT HXD019_MODE_AUTO

/* 睡眠/辅热/灯光/节能（11 键版状态段字节 7-10）：0 关 1 开；只有灯光默认开 */

/* ---------------- 规则四：非空调帧 ---------------- */
#define HXD019_AV_KEY_LEN  2U  /* KEY_code 2B（数据表内每键两字节，原序拷贝） */
#define HXD019_AV_COM_LEN  4U  /* COM_CODE 4B（数据表每组最后四字节） */

#ifdef __cplusplus
}
#endif
#endif /* HXD019_PROTOCOL_H */
