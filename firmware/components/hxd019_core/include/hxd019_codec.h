/**
 * @file hxd019_codec.h
 * @brief HXD019EU 码库查询 API（core 层，无 UART/日志依赖）
 *
 * 内嵌"精简索引"（体验版 4 空调品牌 + IPTV 前 8 品牌 + 常用 TV 品牌），
 * 完整码库（IPTV 235 组 / TV ~200 组，≈25KB+）不进固件，经
 * hxd019_codec_register_table() 动态注册（host 下发 / NVS 加载）。
 *
 * 码组号来源（二选一，等价）：
 *  1. 本索引按品牌/型号查询（hxd019_brand_find → hxd019_codegroup_get）
 *  2. 匹配命令（30 70 A0）后芯片从串口上传码组号（interface 层回调获得）
 */
#ifndef HXD019_CODEC_H
#define HXD019_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "hxd019_types.h"
#include "hxd019_codelist.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 按品牌名查品牌条目。
 * @param name 品牌名（如 "格力"/"Gree"/"美的"/"Midea"，见组件内索引表的别名）
 * @param type 设备类型
 * @return 条目指针（内嵌索引或已注册动态表）；未找到返回 NULL
 */
const hxd019_brand_entry_t *hxd019_brand_find(const char *name, hxd019_dev_type_t type);

/**
 * 取品牌下第 idx 个码组号。
 * @param brand hxd019_brand_find() 返回的条目
 * @param idx   组序号（0 起，< group_cnt）
 * @param out   输出码组号（如格力 830）
 * @return HXD019_OK / HXD019_ERR_ARG / HXD019_ERR_NOENT
 */
hxd019_err_t hxd019_codegroup_get(const hxd019_brand_entry_t *brand, size_t idx, uint16_t *out);

/**
 * 动态注册完整码库表（非空调：F_code/KEY/COM 条目数组）。
 * 生命周期：entries 数组由调用方持有（RAM/flash 均可），注册后全局生效。
 * @return HXD019_OK / HXD019_ERR_ARG
 */
hxd019_err_t hxd019_codec_register_table(const hxd019_av_code_table_t *table);

/**
 * 查动态注册表中第 table_idx 组记录（配合品牌索引使用）。
 * @return HXD019_OK / HXD019_ERR_ARG / HXD019_ERR_NOENT
 */
hxd019_err_t hxd019_codec_entry_get(hxd019_dev_type_t type, size_t table_idx,
                                    hxd019_av_code_entry_t *out);

#ifdef __cplusplus
}
#endif
#endif /* HXD019_CODEC_H */
