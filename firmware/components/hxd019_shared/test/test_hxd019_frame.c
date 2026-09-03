/**
 * @file test_hxd019_frame.c
 * @brief shared 层帧构建金标准自测（CONFIG_HXD019_SELFTEST=1 时参与编译）
 *
 * 金标准来源：datasheet（V6 总规则 + 接口 V1.5 命令表 + 码库 V7）
 * 注意：本文件允许 printf——仅自测路径，不进入正式链接产物（Kconfig 关闭时不编译）。
 */
#include <stdio.h>
#include <string.h>
#include "hxd019_frame.h"
#include "hxd019_protocol.h"
#include "hxd019_types.h"

static int fails;

static void dump_frame(const char *tag, const hxd019_frame_t *f)
{
    printf("[HXD019-TEST] %s len=%u:", tag, (unsigned)f->len);
    for (size_t i = 0; i < f->len; i++) {
        printf(" %02X", f->buf[i]);
    }
    printf("\n");
}

static void expect_frame(const char *tag, const hxd019_frame_t *got,
                         const uint8_t *want, size_t want_len)
{
    dump_frame(tag, got);
    if (got->len != want_len || memcmp(got->buf, want, want_len) != 0) {
        printf("[HXD019-TEST] FAIL %s (want len=%u)\n", tag, (unsigned)want_len);
        fails++;
    }
}

static void expect_err(const char *tag, hxd019_err_t got, hxd019_err_t want)
{
    if (got != want) {
        printf("[HXD019-TEST] FAIL %s err=%d want=%d\n", tag, got, want);
        fails++;
    }
}

int hxd019_selftest_run(void)
{
    hxd019_frame_t f;
    hxd019_ac_state_t st;

    /* --- 规则一：格力 830 组关机/开机/温度加（datasheet 命令表 + V6 规则一） --- */
    hxd019_build_ac_simple(830, HXD019_AC_FUNC_OFF, &f);
    {
        const uint8_t want[] = {0x30, 0x06, 0x03, 0x3E, 0x80};
        expect_frame("ac_simple off(830)", &f, want, sizeof(want));
    }
    hxd019_build_ac_simple(830, HXD019_AC_FUNC_ON, &f);
    {
        const uint8_t want[] = {0x30, 0x06, 0x03, 0x3E, 0x81};
        expect_frame("ac_simple on(830)", &f, want, sizeof(want));
    }
    hxd019_build_ac_simple(830, HXD019_AC_FUNC_TEMP_INC, &f);
    {
        const uint8_t want[] = {0x30, 0x06, 0x03, 0x3E, 0x97};
        expect_frame("ac_simple temp+(830)", &f, want, sizeof(want));
    }

    /* --- 规则二：16B 7 键（V6.c 范例 172 行：830 组 27℃ 制冷 开机 电源键，
     *     校验和 datasheet 未给值，按"前 15 字节之和低 8 位"推导 = 0x97） --- */
    hxd019_ac_state_default(&st);
    st.temp_c = 27;      /* 0x1B */
    st.mode = HXD019_MODE_COOL;
    st.power = HXD019_POWER_ON;
    st.key = HXD019_KEY_POWER;
    /* 其余取默认：风量自动 01 / 手动中 02 / 自动摆开 01 */
    hxd019_build_ac_state(830, &st, &f);
    {
        const uint8_t want[] = {0x30, 0x01, 0x03, 0x3E, 0x1B, 0x01, 0x02,
                                0x01, 0x01, 0x01, 0x02, 0x03, 0x00, 0x00, 0xFF, 0x97};
        expect_frame("ac_state 7key(830,27C,cool,power)", &f, want, sizeof(want));
    }

    /* --- 规则三：16B 11 键（仅 11 键格式编译时检查；校验和按同规则推导） --- */
#if HXD019_FMT_IS_11KEY
    hxd019_ac_state_default(&st);
    st.temp_c = 25;      /* 0x19 两版通用 */
    st.mode = HXD019_MODE_COOL;
    st.power = HXD019_POWER_ON;
    st.key = HXD019_KEY_SLEEP;
    st.sleep = 1;
    hxd019_build_ac_state(830, &st, &f);
    {
        const uint8_t want[] = {0x30, 0x01, 0x03, 0x3E, 0x19, 0x01, 0x02, 0x01, 0x01,
                                0x08, 0x02, 0x01, 0x00, 0x01, 0x00, 0x9C};
        expect_frame("ac_state 11key(830,25C,cool,sleep)", &f, want, sizeof(want));
    }
#endif

    /* --- 规则四：IPTV 第 2 组电源键（码库 V7 第 2 行 + 总规则四范例） --- */
    {
        const uint8_t key[] = {0x0F, 0xF0};
        const uint8_t com[] = {0x01, 0xFE, 0x00, 0x00};
        hxd019_build_av(0x01, key, com, &f);
        const uint8_t want[] = {0x30, 0x00, 0x01, 0x0F, 0xF0, 0x01, 0xFE, 0x00, 0x00, 0x2F};
        expect_frame("av iptv#2 power", &f, want, sizeof(want));
    }
    /* 接口表 B 范例 */
    {
        const uint8_t key[] = {0x02, 0xFD};
        const uint8_t com[] = {0x01, 0xFE, 0x77, 0x88};
        hxd019_build_av(0x01, key, com, &f);
        const uint8_t want[] = {0x30, 0x00, 0x01, 0x02, 0xFD, 0x01, 0xFE, 0x77, 0x88, 0x2E};
        expect_frame("av interface-table-B", &f, want, sizeof(want));
    }

    /* --- 学习/匹配 --- */
    hxd019_build_learn(&f);
    {
        const uint8_t want[] = {0x30, 0x20, 0x50};
        expect_frame("learn", &f, want, sizeof(want));
    }
    hxd019_build_match(&f);
    {
        const uint8_t want[] = {0x30, 0x70, 0xA0};
        expect_frame("match", &f, want, sizeof(want));
    }

    /* --- 负路径 --- */
    hxd019_ac_state_default(&st);
    st.temp_c = 15;
    expect_err("temp<16 rejected", hxd019_build_ac_state(830, &st, &f), HXD019_ERR_TEMP);
    st.temp_c = 32;
    expect_err("temp>31 rejected", hxd019_build_ac_state(830, &st, &f), HXD019_ERR_TEMP);
    st.temp_c = 25;
    st.fan = 5;
    expect_err("fan=5 rejected", hxd019_build_ac_state(830, &st, &f), HXD019_ERR_STATE);
    st.fan = HXD019_FAN_AUTO;
    st.key = HXD019_KEY_SLEEP;
    expect_err("7key rejects key>0x07", hxd019_build_ac_state(830, &st, &f), HXD019_ERR_KEY7);

    printf("[HXD019-TEST] %s (%d fail)\n", fails == 0 ? "ALL PASS" : "HAS FAIL", fails);
    return fails;
}
