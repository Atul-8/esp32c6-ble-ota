# hxd019eu 组件族（hxd019_shared / hxd019_core / hxd019）

HXD019EU 红外遥控芯片 UART 驱动库（ESP32-C6，ESP-IDF v6.0.2），按七层分治封装。
本组件族当前**未接入 main**（不接线不启用），验证靠 host 单测 + 组件自测 + 构建。

## 分层与文件

| 层 | 组件 | 内容 |
| --- | --- | --- |
| shared | `hxd019_shared/` | `hxd019_protocol.h`（帧头/功能码/键名/模式常量）、`hxd019_types.h`（帧/状态/错误码）、`hxd019_codelist.h`（码库索引类型）、`src/hxd019_frame.c`（帧构建纯函数，零硬件依赖）、`test/host_test_hxd019.py`（host 金标准单测）、`test/test_hxd019_frame.c`（组件内自测，CONFIG_HXD019_SELFTEST） |
| core | `hxd019_core/` | `hxd019_codec.c`（精简品牌索引 + 动态注册表）、`hxd019_session.c`（绑定码组 + 空调状态缓存 + F_code 特殊字节 TODO 钩子） |
| interface | `hxd019/` | `hxd019_uart.c`（uart_driver 57600-8N1 + RX 任务 + ESP_LOG 锚点 + 便捷 API） |

依赖方向严格单向：`hxd019 → hxd019_core → hxd019_shared`；shared/core 零 ESP_LOG/printf/driver 头（自测文件除外，Kconfig 关闭不编译）。

## 协议速查（全部金标准已 host 复算）

| 规则 | 帧长 | 格式 | 金标准 |
| --- | --- | --- | --- |
| 一 空调简单 | 5B | `30 06 [码组2B大端] [功能1B]`（无校验） | `30 06 03 3E 81`（格力830开机） |
| 二 空调完整 7 键 | 16B | `30 01 [码组2B] [温度 风量 手向 自向 开关 键名 模式] 03 00 00 FF [校验]` | 范例172行推导校验和 `0x97`（datasheet 留空） |
| 三 空调完整 11 键 | 16B | `30 01 [码组2B] [状态11B：+睡眠 辅热 灯光 节能] [校验]` | 无数值范例，结构自洽 |
| 四 非空调 | 10B | `30 00 [F_code] [KEY2B] [COM4B] [校验]` | `30 00 01 0F F0 01 FE 00 00 2F` ✓ |
| 学习 | 3B | `30 20 50` | ✓ |
| 匹配 | 3B | `30 70 A0` → 芯片上传码组号 | ✓ |

校验和 = 校验字节之前全部字节之和低 8 位（规则一/学习/匹配无校验字节）。

### 金标准对照表（host_test_hxd019.py，ALL PASS）

| 用例 | 构建帧 | datasheet | 结论 |
| --- | --- | --- | --- |
| ac_simple off/on/tmp+ 830 | `30 06 03 3E 80/81/97` | 接口表+V6 规则一 | 一致 |
| ac_state7（规则二 172 行） | `30 01 03 3E 1B 01 02 01 01 01 02 03 00 00 FF 97` | 前 15B 一致；校验和未给→推导 0x97 | 推导自洽 |
| av iptv#2 power | `30 00 01 0F F0 01 FE 00 00 2F` | 码库 V7 头注释+V6 规则四 | 一致（chk 0x2F 复核过） |
| av iface-B | `30 00 01 02 FD 01 FE 77 88 2E` | 接口表 B 范例 | 一致（chk 0x2E 复核过） |
| learn / match | `30 20 50` / `30 70 A0` | 接口表 C | 一致 |

### datasheet 疑点（汇报要点，联调前复核）

1. **规则二范例 172 行校验和缺省**：原文以 `[1B]` 占位。按"前面所有数据之和低 8 位"推导 = 0x97（前 15B 累加）。范例中还有两处笔误痕迹：`0x03e`（应为 0x3E）与"手动风向 02 但范例值 01"（按字段表默认 02 构帧，范例以 01 计同样自洽——不影响实现，温度/键名/模式逐字节对齐）。
2. **温度编码库版本差异**：7 键帧表值 = ℃（19→0x13…30→0x1E）；11 键帧 V7/新库 = ℃（16→0x10…31→0x1F），但 datasheet 注释"如果是用 d，du 的库，这个数据要减 16"。默认实现按表值直编（字节=℃），d/du 库差异做成编译期选项 `CONFIG_HXD019_TEMP_ENC_MINUS16`（依赖 11 键格式 `CONFIG_HXD019_STATE_FMT_11KEY`），真机联调确认前保持默认关。
3. **匹配应答帧格式未定义**：datasheet 只说"上传码库号 830"。RX 任务按松散扫描（2B 大端 + 值域过滤）实现并留 TODO 锚点，真机抓包后收紧。
4. **F_code 特殊字节**：规则四要求查 `custom_send_command_data()`（remote_data_drv3010_V1.2.c），datasheet 未附实现。core 会话层留钩子 `hxd019_fcode_hook_t`（当前桩：全部按普通字节直发）；码库中 F_code≠0x01 的组（0x3C/0x64/0x72 等）联调时优先怀疑。
5. **码库表 "55/57" 列宽**：实为 1+2×25+4=55（IPTV）、1+2×26+4+2 填充=57（TV），结构体 `keytable[50]/[52]` 与之对应；固件只存精简索引，完整表经 `hxd019_codec_register_table()` 动态注册（host/NVS）。

## 使用示例（联调时接线后）

```c
#include "hxd019.h"
#include "hxd019_codec.h"

hxd019_init();
// 方式 A：品牌索引
const hxd019_brand_entry_t *b = hxd019_brand_find("格力", HXD019_DEV_AC);
uint16_t g; hxd019_codegroup_get(b, 0, &g);   // 830
hxd019_bind_group(g);
hxd019_ac_power(true);
hxd019_ac_temp(26);
// 方式 B：遥控器对码
hxd019_match(on_match_cb, NULL);              // 回调里 hxd019_bind_group()
```

## Kconfig

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `HXD019_STATE_FMT_11KEY` | n | 规则三 11 键帧（含睡眠/辅热/灯光/节能） |
| `HXD019_TEMP_ENC_MINUS16` | n | 11 键帧温度字节=℃-16（d/du 库） |
| `HXD019_UART_TX/RX/NUM` | 4/5/1 | UART 接线 |
| `HXD019_SELFTEST` | n | 组件自测入口 `hxd019_selftest_run()`（需 main 显式调用） |

## 验证状态

- host 单测：`python firmware/components/hxd019_shared/test/host_test_hxd019.py` → ALL PASS（7 用例）
- 固件构建：WSL `tools/wslbuild.sh`（组件未被 main 引用不参与链接，构建全绿即头/依赖无误）
- 真机联调：下轮（需接 HXD019EU 芯片 UART，确认疑点 2/3/4）
