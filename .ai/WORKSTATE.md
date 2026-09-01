# 当前工作状态

## 项目
ESP32-C6 BLE OTA — 基于 espressif/ble_ota v0.1.17 官方组件的固件 + Windows 上位机测试工具

## 正在进行
- [TASK-001] 跑通 ESP32-C6 hello_world **✓ 完成** | 2026-09-01 16:30
  - 构建✓ 烧录✓ 运行✓（两轮 Hello world! + 自动重启确认）
  - [验证: shared✓ core✓ interface✓ presentation✓]
- [TASK-002] BLE OTA 固件开发 进度: 100%（固件侧）| embedded-firmware-engineer | 2026-09-01 18:40
  - 纠偏记录：此前 WORKSTATE 称"代码全部就位"与实际不符（两侧均 hello_world 基线），本轮从零编写
  - 完成：三层代码（ota_shared/ota_core/main）+ partitions.csv + sdkconfig.defaults + idf_component.yml
  - 构建零告警 ✓，app bin 656864B（ota_0 1.75MB 槽占用 36%）✓，flash 4 镜像 hash verified ✓
  - 分层 grep 自查 ✓（core 层 0 个 ESP_LOG/printf，core 无 NimBLE/ble_ota 头，shared 纯宏）
  - 错误沉淀：ERR-005（组件 extern notify_sem 隐式契约）+ META-001/META-002
  - [验证: shared✓ core✓ interface✓ presentation✓]（presentation=固件侧构建产物验证；运行时行为验证归 TASK-004 联调）

## 未完成队列
- [ ] TASK-002 收尾：设备上手动按 RST 启动 app，PM 编排运行验证（锚点：`[BLE_OTA] version=1.0.0, run_part=ota_0`；ERR-003: 不做软件复位）
- [ ] TASK-003 Windows 上位机（Bleak，扫描-连接-传输-校验-激活；协议要点与组件契约见 STRUCTURE.md 关键机制节）
- [ ] TASK-004 全链路 BLE OTA 联调（含回滚测试：升级后验证 PENDING_VERIFY→confirmed 锚点）
- [注意] 烧录后需手动按 RST 键启动 app（ERR-003: usbipd 虚拟复位不可信）

## 上次中断点
- 文件: firmware/（TASK-002 固件侧完结，已烧录）
- 操作: 等待手动 RST 后的启动日志验证
- 待恢复: TASK-003 上位机开发

## 环境事实
- ESP-IDF: v6.0.2 @ WSL `/root/esp/v6.0.2/esp-idf`
- 构建: `wsl bash /tmp/wb.sh`（脚本源在 tools/wslbuild.sh，先拷 /tmp，见 ERR-002）
- 芯片: ESP32-C6 @ WSL /dev/ttyACM0（usbipd busid 1-8, VID:PID 303a:1001）
- MAC: 14:c1:9f:e5:11:28
- 上位机: Windows 侧 Python + Bleak（蓝牙适配器 1-9: 10d7:b012）
- 组件参考源码: WSL /root/bleota_ref_persist/（ble_ota v0.1.17 全量 + example）
