/**
 * @file hxd019_codelist.h
 * @brief HXD019EU 码库索引类型（shared 层）
 *
 * 码库组织（019-免费码库-NME.ARC_TV_IPTV_table_V7.c.b-1.c）：
 *  - 数据表：remote_IPTV_table[][55] / tv_table[][57]
 *            条目 = {format_index(F_code 1B), keytable[50/52](每键 2B), custome_code[4](COM_CODE)}
 *  - 品牌索引表：g_remote_arc_info[][170]（空调）/ remote_IPTV_info[][21]（IPTV）/ TV_info[][15]
 *            每行首元素 = 该品牌码组数量，其后为数据表中的组序号（0 起）
 *
 * 固件内只嵌"精简索引"（品牌 → 码组号列表，见 hxd019_core）；
 * 完整码库数据表不进固件（IPTV 235 组 × 55B + TV ~200 组 × 57B ≈ 25KB+），
 * 经 hxd019_codec_register_table() 动态注册（host 侧下发/NVS 加载，core 层接线）。
 */
#ifndef HXD019_CODELIST_H
#define HXD019_CODELIST_H

#include <stdint.h>
#include <stddef.h>
#include "hxd019_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单个品牌条目（精简索引） */
typedef struct {
    const char *name;          /* 品牌名（匹配函数按 strcmp，调用方决定中文/英文形式） */
    hxd019_dev_type_t type;    /* 设备类型 */
    const uint16_t *groups;    /* 码组号列表（十进制值，如格力 830），指向常量数组 */
    size_t group_cnt;          /* 码组数量（对应索引表行首"数量"元素） */
} hxd019_brand_entry_t;

/** 非空调键码记录（完整码库数据表一组的等价形式，供动态注册） */
typedef struct {
    uint8_t fmt_index;      /* F_code：每组第一个字节 */
    uint8_t key_code[2];    /* 按键数据 2B（表内原序） */
    uint8_t com_code[4];    /* 每组最后 4 字节（COM_CODE） */
} hxd019_av_code_entry_t;

/** 动态注册的码库表（host 侧下发或 NVS 加载后传入，core 层消费） */
typedef struct {
    const hxd019_av_code_entry_t *entries;  /* 条目数组 */
    size_t cnt;                             /* 条目数 */
    hxd019_dev_type_t type;                 /* 该表对应的设备类型 */
} hxd019_av_code_table_t;

#ifdef __cplusplus
}
#endif
#endif /* HXD019_CODELIST_H */
