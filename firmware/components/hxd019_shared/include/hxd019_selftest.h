/**
 * @file hxd019_selftest.h
 * @brief 组件内建自测（金标准帧构建检查，无硬件依赖）
 *
 * 开启 CONFIG_HXD019_SELFTEST 后编译，需在 main 中显式调用 hxd019_selftest_run()。
 * 未开启该选项时实现文件不参与编译，不进正式链接产物。
 */
#ifndef HXD019_SELFTEST_H
#define HXD019_SELFTEST_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_HXD019_SELFTEST

/**
 * 运行帧构建金标准自检（printf 输出 PASS/FAIL 到 console，仅自测用途）。
 * @return 失败项数（0 = 全过）
 */
int hxd019_selftest_run(void);

#endif /* CONFIG_HXD019_SELFTEST */

#ifdef __cplusplus
}
#endif
#endif /* HXD019_SELFTEST_H */
