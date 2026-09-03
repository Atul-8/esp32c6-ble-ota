/**
 * @file hxd019_frame.c
 * @brief HXD019EU 帧构建纯函数（shared 层，零硬件依赖，可 host/PC 单测）
 *
 * 金标准（datasheet，Python 逐字节复算对照见 test/host_test_hxd019.py 与组件 README）：
 *  - 规则一：格力 830 组关机 30 06 03 3E 80 / 开机 30 06 03 3E 81（5B 无校验）
 *  - 规则二：格力 830 组 27℃ 制冷 风量自动 手动上摆 自动摆开 开机 电源键 模式制冷
 *            30 01 03 3E 1B 01 02 01 01 01 02 [03 00 00 FF] chk → 推导 0x97
 *            （datasheet 范例 172 行未给校验和值，留 [1B] 占位；帧内温度 0x1B=27℃）
 *  - 规则四：IPTV 第 2 组电源 30 00 01 0F F0 01 FE 00 00 2F（chk=0x2F 已复核 ✓）
 *  - 接口表 B：30 00 01 02 FD 01 FE 77 88 2E（chk=0x2E 已复核 ✓）
 *  - 学习 30 20 50 / 匹配 30 70 A0
 */
#include <string.h>
#include <stdbool.h>
#include "hxd019_frame.h"
#include "hxd019_protocol.h"
#include "hxd019_types.h"

uint8_t hxd019_checksum(const uint8_t *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return 0;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + buf[i]);  /* 低 8 位累加，故意溢出 */
    }
    return sum;
}

/* 码组号 → 大端 2B（830 → 03 3E） */
static void code_group_to_be(uint16_t group, uint8_t out[2])
{
    out[0] = (uint8_t)(group >> 8);
    out[1] = (uint8_t)(group & 0xFF);
}

hxd019_err_t hxd019_build_ac_simple(uint16_t code_group, uint8_t func_code, hxd019_frame_t *out)
{
    if (out == NULL) {
        return HXD019_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->buf[0] = 0x30;
    out->buf[1] = 0x06;
    code_group_to_be(code_group, &out->buf[2]);
    out->buf[4] = func_code;
    out->len = HXD019_LEN_AC_SIMPLE;
    return HXD019_OK;  /* 5B 无校验和 */
}

hxd019_err_t hxd019_build_learn(hxd019_frame_t *out)
{
    if (out == NULL) {
        return HXD019_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->buf[0] = HXD019_LEARN_CODE1;
    out->buf[1] = HXD019_LEARN_CODE2;
    out->buf[2] = HXD019_LEARN_CODE3;
    out->len = HXD019_LEN_LEARN;
    return HXD019_OK;
}

hxd019_err_t hxd019_build_match(hxd019_frame_t *out)
{
    if (out == NULL) {
        return HXD019_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->buf[0] = HXD019_LEARN_CODE1;
    out->buf[1] = HXD019_MATCH_CODE2;
    out->buf[2] = HXD019_MATCH_CODE3;
    out->len = HXD019_LEN_MATCH;
    return HXD019_OK;
}

hxd019_err_t hxd019_build_av(uint8_t fmt_index,
                             const uint8_t key_code[HXD019_AV_KEY_LEN],
                             const uint8_t com_code[HXD019_AV_COM_LEN],
                             hxd019_frame_t *out)
{
    if (out == NULL || key_code == NULL || com_code == NULL) {
        return HXD019_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->buf[0] = 0x30;
    out->buf[1] = 0x00;
    out->buf[2] = fmt_index;                            /* F_code */
    out->buf[3] = key_code[0];                          /* KEY_code 2B，表内原序 */
    out->buf[4] = key_code[1];
    memcpy(&out->buf[5], com_code, HXD019_AV_COM_LEN);  /* COM_CODE 4B */
    out->len = HXD019_LEN_AV;
    out->buf[9] = hxd019_checksum(out->buf, 9);         /* 前 9B 之和低 8 位 */
    return HXD019_OK;
}

/* 状态段字段合法性校验（7 键版温度上限在 build 内按格式分支判） */
static hxd019_err_t validate_state(const hxd019_ac_state_t *s)
{
    if (s == NULL) {
        return HXD019_ERR_ARG;
    }
    if (s->fan < HXD019_FAN_AUTO || s->fan > HXD019_FAN_HIGH) {
        return HXD019_ERR_STATE;
    }
    if (s->swing < HXD019_SWING_UP || s->swing > HXD019_SWING_DOWN) {
        return HXD019_ERR_STATE;
    }
    if (s->swing_auto > HXD019_LRSWING_ON) {
        return HXD019_ERR_STATE;
    }
    if (s->power > HXD019_POWER_ON) {
        return HXD019_ERR_STATE;
    }
    if (s->mode < HXD019_MODE_AUTO || s->mode > HXD019_MODE_HEAT) {
        return HXD019_ERR_STATE;
    }
    if (s->sleep > 1 || s->aux_heat > 1 || s->light > 1 || s->eco > 1) {
        return HXD019_ERR_STATE;
    }
    return HXD019_OK;
}

/*
 * 温度编码（℃ → 状态段字节 0）：
 *  - 7 键帧（规则二）：表值 19℃→0x13 ... 30℃→0x1E，即 字节 = ℃（19-30）
 *  - 11 键帧（规则三）：V7/新库 字节 = ℃（16℃→0x10 ... 31℃→0x1F）；
 *    d/du 库 datasheet 注明"这个数据要减 16"→ 字节 = ℃ - 16（编译期 CONFIG_HXD019_TEMP_ENC_MINUS16）
 */
static hxd019_err_t temp_encode(uint8_t temp_c, uint8_t *out)
{
#if HXD019_TEMP_ENC_MINUS16_EN
    /* d/du 库：字节 = ℃ - 16（16℃→0x00…，datasheet 规则三注释"减 16"）。
     * 该编码仅对 11 键帧有意义；真机联调前保持默认关（见组件 README 疑点 2） */
    *out = (uint8_t)(temp_c - 16);
    return HXD019_OK;
#else
    /* 默认：字节 = ℃，与 datasheet 两版表值直配（19→0x13、25→0x19、31→0x1F） */
    *out = temp_c;
    return HXD019_OK;
#endif
}

hxd019_err_t hxd019_build_ac_state(uint16_t code_group,
                                   const hxd019_ac_state_t *state,
                                   hxd019_frame_t *out)
{
    if (out == NULL) {
        return HXD019_ERR_ARG;
    }
    hxd019_err_t err = validate_state(state);
    if (err != HXD019_OK) {
        return err;
    }
    if (state->key < HXD019_KEY_POWER || state->key > HXD019_KEY_STRONG) {
        return HXD019_ERR_STATE;
    }

    const bool fmt11 = (HXD019_FMT_IS_11KEY != 0);
    size_t n;
    uint8_t temp_min, temp_max;
    if (!fmt11) {
        if (state->key > HXD019_KEY_TEMP_DEC) {
            return HXD019_ERR_KEY7;  /* 7 键版只支持键名 0x01-0x07 */
        }
        temp_min = HXD019_TEMP7_MIN_C;   /* 19 */
        temp_max = HXD019_TEMP7_MAX_C;   /* 30 */
        n = 11;                          /* 头 4B + 状态 7B */
    } else {
        temp_min = HXD011_TEMP11_MIN_C;  /* 16 */
        temp_max = HXD011_TEMP11_MAX_C;  /* 31 */
        n = 15;                          /* 头 4B + 状态 11B */
    }
    if (state->temp_c < temp_min || state->temp_c > temp_max) {
        return HXD019_ERR_TEMP;
    }

    memset(out, 0, sizeof(*out));
    out->buf[0] = 0x30;
    out->buf[1] = 0x01;
    code_group_to_be(code_group, &out->buf[2]);

    uint8_t tb;
    err = temp_encode(state->temp_c, &tb);
    if (err != HXD019_OK) {
        return err;
    }
    out->buf[4] = tb;          /* 状态段字节 0：温度 */
    out->buf[5] = state->fan;
    out->buf[6] = state->swing;
    out->buf[7] = state->swing_auto;
    out->buf[8] = state->power;
    out->buf[9] = state->key;
    out->buf[10] = state->mode;
    if (!fmt11) {
        out->buf[n++] = HXD019_AC7_TAIL0;  /* 固定尾 03 00 00 FF（DU/D 版芯片） */
        out->buf[n++] = HXD019_AC7_TAIL1;
        out->buf[n++] = HXD019_AC7_TAIL2;
        out->buf[n++] = HXD019_AC7_TAIL3;
    } else {
        out->buf[11] = state->sleep;
        out->buf[12] = state->aux_heat;
        out->buf[13] = state->light;
        out->buf[14] = state->eco;
    }

    out->buf[n] = hxd019_checksum(out->buf, n);
    out->len = n + 1;
    return HXD019_OK;
}

void hxd019_ac_state_default(hxd019_ac_state_t *def)
{
    if (def == NULL) {
        return;
    }
    memset(def, 0, sizeof(*def));
    def->temp_c = HXD019_TEMP_DEFAULT;        /* 25℃ → 0x19 两版通用 */
    def->fan = HXD019_FAN_DEFAULT;            /* 自动 */
    def->swing = HXD019_SWING_DEFAULT;        /* 中 */
    def->swing_auto = HXD019_LRSWING_DEFAULT; /* 开 */
    def->power = HXD019_POWER_OFF;            /* 关 */
    def->key = HXD019_KEY_POWER;              /* 对码时发开机键数据 */
    def->mode = HXD019_MODE_DEFAULT;          /* 自动 */
    def->sleep = 0;
    def->aux_heat = 0;
    def->light = 1;                           /* datasheet：只有灯光默认开 */
    def->eco = 0;
}
