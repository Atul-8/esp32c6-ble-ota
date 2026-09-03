/**
 * @file hxd019_codec.c
 * @brief HXD019EU 码库精简索引（core 层）
 *
 * 精简索引来源（docs/.../019-免费码库-NME.ARC_TV_IPTV_table_V7.c.b-1.c）：
 *  - 空调：g_remote_arc_info 体验版 4 品牌（美的/海尔/格力/奥克斯）+ V7 完整表中的
 *    常用品牌组号（小米/大金/松下/海信/TCL/长虹），组号取各品牌索引行前几项
 *  - IPTV：remote_IPTV_info 前 8 品牌（烽火/华为/斯达康/同洲/中兴/大亚/长虹/创维），
 *    groups 存的是"数据表组序号"（0 起，remote_IPTV_table 下标）
 *  - TV：TV_info 常用品牌（康佳/海尔），同样存数据表组序号
 *
 * 注意：空调 groups 存"码组号"（如 830，直接进帧大端 2B）；
 *       IPTV/TV groups 存"数据表组序号"（需查数据表得 F_code/KEY/COM，经
 *       hxd019_codec_entry_get() 取动态注册表，或留 host 侧）。
 * 完整码库≈25KB+ 不进固件；动态注册接口见 hxd019_codec_register_table()。
 */
#include <string.h>
#include "hxd019_codec.h"

/* ---------------- 空调（码组号，十进制） ---------------- */
static const uint16_t arc_midea[]  = {1032, 1016, 930, 663, 525, 521};
static const uint16_t arc_haier[]  = {1001, 1000, 972, 973, 974, 975, 976};
static const uint16_t arc_gree[]   = {830, 526, 1056};
static const uint16_t arc_aux[]    = {960, 915, 890};
static const uint16_t arc_xiaomi[] = {1052, 958};        /* V7 表：米家 2 组 */
static const uint16_t arc_daikin[] = {1061, 1038, 1030, 600, 599}; /* V7 表前 5 组 */
static const uint16_t arc_pana[]   = {1078, 849, 677, 828, 829};   /* V7 表前 5 组 */
static const uint16_t arc_hisense[] = {1063, 1062, 1053, 1038};    /* V7 表前 4 组 */
static const uint16_t arc_tcl[]    = {1071, 1002, 1001, 519, 626}; /* V7 表前 5 组 */
static const uint16_t arc_changhong[] = {1075, 1073, 1065, 1041};  /* V7 表前 4 组 */

/* ---------------- IPTV（数据表组序号，0 起） ---------------- */
static const uint16_t iptv_beacon[]  = {0, 33, 56, 61, 17};
static const uint16_t iptv_huawei[]  = {100, 101, 102, 103, 104, 105, 106, 107, 108,
                                        50, 51, 52, 53, 54, 1, 2, 3, 4, 5, 19, 20};
static const uint16_t iptv_uts[]     = {6, 7};
static const uint16_t iptv_states[]  = {50, 51, 52, 53, 54, 8, 22, 23};
static const uint16_t iptv_zte[]     = {179, 180, 181, 50, 51, 52, 53, 54, 9, 10, 11, 24, 25};
static const uint16_t iptv_daya[]    = {12, 13, 15};
static const uint16_t iptv_changhong[] = {89, 13, 50, 51, 52, 53, 54};
static const uint16_t iptv_skyworth[]  = {232, 93, 92, 201, 90, 50, 14, 51, 52, 53, 54};

/* ---------------- TV（数据表组序号，0 起） ---------------- */
static const uint16_t tv_konka[]  = {64, 29};
static const uint16_t tv_haier[]  = {28, 29};

#define BRAND(nm, tp, arr) { .name = nm, .type = (tp), .groups = (arr), .group_cnt = sizeof(arr) / sizeof((arr)[0]) }

static const hxd019_brand_entry_t s_brands[] = {
    /* 空调：体验版 4 品牌（中文名为主键） */
    BRAND("美的",   HXD019_DEV_AC, arc_midea),
    BRAND("Midea",  HXD019_DEV_AC, arc_midea),
    BRAND("海尔",   HXD019_DEV_AC, arc_haier),
    BRAND("Haier",  HXD019_DEV_AC, arc_haier),
    BRAND("格力",   HXD019_DEV_AC, arc_gree),
    BRAND("Gree",   HXD019_DEV_AC, arc_gree),
    BRAND("奥克斯", HXD019_DEV_AC, arc_aux),
    BRAND("AUX",    HXD019_DEV_AC, arc_aux),
    /* 空调：V7 完整表常用品牌 */
    BRAND("小米",      HXD019_DEV_AC, arc_xiaomi),
    BRAND("MI JIA",    HXD019_DEV_AC, arc_xiaomi),
    BRAND("大金",      HXD019_DEV_AC, arc_daikin),
    BRAND("Daikin",    HXD019_DEV_AC, arc_daikin),
    BRAND("松下",      HXD019_DEV_AC, arc_pana),
    BRAND("Panasonic", HXD019_DEV_AC, arc_pana),
    BRAND("海信",      HXD019_DEV_AC, arc_hisense),
    BRAND("Hisense",   HXD019_DEV_AC, arc_hisense),
    BRAND("TCL",       HXD019_DEV_AC, arc_tcl),
    BRAND("长虹",      HXD019_DEV_AC, arc_changhong),
    BRAND("Changhong", HXD019_DEV_AC, arc_changhong),
    /* IPTV：前 8 品牌 */
    BRAND("烽火",   HXD019_DEV_IPTV, iptv_beacon),
    BRAND("beacon", HXD019_DEV_IPTV, iptv_beacon),
    BRAND("华为",   HXD019_DEV_IPTV, iptv_huawei),
    BRAND("Huawei", HXD019_DEV_IPTV, iptv_huawei),
    BRAND("斯达康", HXD019_DEV_IPTV, iptv_uts),
    BRAND("UTS",    HXD019_DEV_IPTV, iptv_uts),
    BRAND("同洲",   HXD019_DEV_IPTV, iptv_states),
    BRAND("states", HXD019_DEV_IPTV, iptv_states),
    BRAND("中兴",   HXD019_DEV_IPTV, iptv_zte),
    BRAND("ZTE",    HXD019_DEV_IPTV, iptv_zte),
    BRAND("大亚",   HXD019_DEV_IPTV, iptv_daya),
    BRAND("Daya",   HXD019_DEV_IPTV, iptv_daya),
    BRAND("长虹",   HXD019_DEV_IPTV, iptv_changhong),
    BRAND("创维",   HXD019_DEV_IPTV, iptv_skyworth),
    BRAND("SKYWORTH", HXD019_DEV_IPTV, iptv_skyworth),
    /* TV */
    BRAND("康佳",   HXD019_DEV_TV, tv_konka),
    BRAND("Konka",  HXD019_DEV_TV, tv_konka),
    BRAND("海尔",   HXD019_DEV_TV, tv_haier),
};

#define BRAND_CNT (sizeof(s_brands) / sizeof(s_brands[0]))

/* 动态注册表（最多各类型一张；RAM 代价：每类型一个指针+计数） */
static const hxd019_av_code_table_t *s_tables[3] = {NULL, NULL, NULL};

const hxd019_brand_entry_t *hxd019_brand_find(const char *name, hxd019_dev_type_t type)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < BRAND_CNT; i++) {
        if (s_brands[i].type == type && strcmp(s_brands[i].name, name) == 0) {
            return &s_brands[i];
        }
    }
    return NULL;
}

hxd019_err_t hxd019_codegroup_get(const hxd019_brand_entry_t *brand, size_t idx, uint16_t *out)
{
    if (brand == NULL || out == NULL) {
        return HXD019_ERR_ARG;
    }
    if (idx >= brand->group_cnt) {
        return HXD019_ERR_NOENT;
    }
    *out = brand->groups[idx];
    return HXD019_OK;
}

hxd019_err_t hxd019_codec_register_table(const hxd019_av_code_table_t *table)
{
    if (table == NULL || table->entries == NULL || table->type > HXD019_DEV_TV) {
        return HXD019_ERR_ARG;
    }
    s_tables[table->type] = table;
    return HXD019_OK;
}

hxd019_err_t hxd019_codec_entry_get(hxd019_dev_type_t type, size_t table_idx,
                                    hxd019_av_code_entry_t *out)
{
    if (out == NULL || type > HXD019_DEV_TV) {
        return HXD019_ERR_ARG;
    }
    const hxd019_av_code_table_t *t = s_tables[type];
    if (t == NULL || table_idx >= t->cnt) {
        return HXD019_ERR_NOENT;
    }
    *out = t->entries[table_idx];
    return HXD019_OK;
}
