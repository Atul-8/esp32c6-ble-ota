# 当前工作状态

## 项目
ESP32-C6 BLE OTA — 基于 espressif/ble_ota v0.1.17 官方组件的固件 + Windows 上位机测试工具

## 任务状态（全部完成 ✓）
- [TASK-004@任务表] HXD019EU 红外遥控芯片 UART 驱动库封装 **✓** | 2026-09-03:
  三组件 hxd019_shared/hxd019_core/hxd019（REQ-003）；host 金标准 7/7 ALL PASS
  （规则二 16B chk 推导 0x97、IPTV 0x2F、接口表 B 0x2E 复核）；主工程构建绿；
  WSL 临时自测工程三组件编译链接全过（IDF v6 esp_driver_uart 适配，ERR-012/META-009）；
  分层 grep 零违规；未接 main 未烧录，联调疑点 5 条见 hxd019_shared/README.md
- [TASK-002@任务表] GUI 可视化测试工具（host/ble_ota_gui.py）**✓** | 2026-09-02:
  修复中断残缺（_hide_tip 残行 / :171 坏 event_cb 表达式 / 补齐缺失的 OtaGui 类）；
  真机 headless 全流程 PASS：scan→select→flash 161 sectors / 1443 packets / 44.7s /
  0 重传 0 跳转 0 重连，reboot_ok 确认；CLI --help/scan/dry-run 回归全绿
- [TASK-001] hello_world 跑通 **✓** | 2026-09-01：构建烧录运行全过
- [TASK-002] BLE OTA 固件 **✓** | 2026-09-02：三层代码 + vendor 组件 + 回滚 + 进度持久化；真机锚点全命中
- [TASK-003] Windows 上位机 **✓** | 2026-09-02：Bleak CLI（scan/info/flash），真机 dry-run + 真实传输全过
- [TASK-004] 全链路联调 **✓** | 2026-09-02：**BLE OTA 真机升级成功**——1.0.0(ota_0) → 1.0.1(ota_1)，
  161 sectors / 656,720B / 45.9s / 0 重传 / 0 跳 sector / 0 重连；升级后 img_state=VALID（回滚确认过），
  槽位交替 target=ota_0 正常；进度持久化完成即清零

## 遗留优化项（非阻塞，留档）
- [ ] 传输速率优化：当前 14 KB/s（esp_ota_write 每包同步落盘 + NVS 每 sector commit），理论有余量
- [ ] issue #9 回滚实战测试（刷自检失败版本验证自动回滚）——机制代码已就位，实战未跑
- [ ] issue #10 文档收尾（docs/PROTOCOL.md、已知问题清单）
- [注意] 烧录后需手动按 RST 键启动 app（ERR-003: usbipd 虚拟复位不可信；但 flash 后 hard reset 有时也能自启）
- [注意] GitHub 推送需走代理 `git -c http.proxy=http://127.0.0.1:7890 push github master`（系统代理 ProxyEnable=0）
- [注意] components/ble_ota 为本地 vendor 组件（v0.1.17 + 设备名 Kconfig patch），勿在 main/idf_component.yml 重新声明 espressif/ble_ota（重名冲突）；升级组件需重新 vendor

## 正在进行
- 无（TASK-004 收口）

## 未完成队列
- [ ] HXD019EU 真机联调（下轮）：接线 UART1 GPIO4/5 ↔ 芯片 57600-8N1；确认 README 疑点 2/3/4
      （d/du 温度编码、匹配应答帧格式、F_code 特殊字节），确认后收紧 RX 解析与钩子实现
- [ ] 传输速率优化（遗留）：当前 14 KB/s（esp_ota_write 每包同步落盘 + NVS 每 sector commit）
- [ ] issue #9 回滚实战测试（遗留）
- [ ] issue #10 文档收尾（遗留）

## 上次中断点
- 文件: firmware/components/hxd019/（TASK-004 全部收口）
- 操作: 三组件交付完毕，验证全绿（host 单测/主工程构建/临时自测工程编译链接/分层 grep），.ai 已同步
- 待恢复: 无阻塞。HXD019EU 真机接线联调见"未完成队列"首项

## 环境事实
- ESP-IDF: v6.0.2 @ WSL `/root/esp/v6.0.2/esp-idf`
- 构建: `wsl bash /tmp/wb.sh`（脚本源 tools/wslbuild.sh，先拷 /tmp，见 ERR-002）
- 组件自测工程（TASK-004）: WSL ~/hxd_selftest_proj（三组件 hxd019* + selftest main；未烧录，防覆盖 OTA 固件）
- HXD019EU 协议要点: 四发码规则+学习匹配，金标准对照与 5 条联调疑点见 firmware/components/hxd019_shared/README.md
- 芯片: ESP32-C6 @ WSL /dev/ttyACM0（usbipd busid 1-8, VID:PID 303a:1001）
- MAC: 14:c1:9f:e5:11:28（BLE: 14:c1:9f:e5:11:2a，广播名 C6-OTA-1128）
- 上位机: Windows 侧 Python + Bleak 3.0.2（蓝牙适配器 1-9: 10d7:b012）
- 组件参考源码: WSL /root/bleota_ref_persist/（ble_ota v0.1.17 全量 + example）
- 当前设备状态: v1.0.1 运行于 ota_1（VALID），下一轮 OTA 目标 ota_0

## 错误索引（.ai/errors/raw/）
- ERR-001 中文路径+9p 构建 specs 损坏 → WSL 原生路径构建
- ERR-002 EIM 激活脚本 source 自杀 → -e 模式逐行 export
- ERR-003 usbipd 虚拟复位时序破坏 boot strap → 手动 RST
- ERR-004 Gitee PAT 缺 issues scope（404 伪装）→ issues 放 GitHub
- ERR-005 组件 extern notify_sem 隐式契约（META-001）
- ERR-006 广播名竞态 → vendor 组件 + Kconfig 设备名
- ERR-007 ota_task fw_len 启动缓存恒 0 → 循环内实时读
- ERR-008 上位机误判 sector 0 全零成功 ACK（META-005: ACK 合法性=自校验判定）
- ERR-009 ota_task 丢失循环末尾无条件 give → 信号量泄漏流水线冻结（META-006）
- ERR-012 IDF v6 driver 组件拆分/uart API 更名/no-op 语句三连 -Werror（META-009）
