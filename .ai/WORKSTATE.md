# 当前工作状态

## 项目
ESP32-C6 BLE OTA — 基于 IDF hello_world 的 BLE OTA 固件 + Windows 上位机测试工具

## 正在进行
- [TASK-001] 跑通 ESP32-C6 hello_world **✓ 完成** | 2026-09-01 16:30
  - 构建✓ 烧录✓ 运行✓（两轮 Hello world! + 自动重启确认）
  - [验证: shared✓ core✓ interface✓ presentation✓]
- [TASK-002] BLE OTA 固件开发 进度: 55% | embedded-firmware-engineer 执行中 | 2026-09-01
  - 已完成：代码全部就位（ota_shared/ota_core/main 分层 + partitions.csv + sdkconfig.defaults + idf_component.yml）
  - 正在做：WSL 拉取 ble_ota 组件源码 → 读源码确认续传支持 → 构建验证 → 烧录
  - [验证: shared✗ core✗ interface✗ presentation✗]（构建通过后逐层标记）

## 未完成队列
- [ ] TASK-002 剩余：构建零告警验证 + flash 烧录 + 分层合规 grep + STRUCTURE.md/LOG.md 更新
- [ ] TASK-003 Windows 上位机（Bleak，扫描-连接-传输-校验-激活）
- [ ] TASK-004 全链路 BLE OTA 联调（含回滚测试）
- [注意] 烧录后需手动按 RST 键启动 app（ERR-003: usbipd 虚拟复位不可信）

## 上次中断点
- 文件: firmware/（代码全部就位，等待 WSL 构建验证）
- 操作: TASK-002 编码完成，进入 G2.5 验证阶段
- 待恢复: wsl -e bash -c 'cp "/mnt/f/project/嵌入式项目/esp32c6ota/tools/wslbuild.sh" /tmp/wb.sh && bash /tmp/wb.sh'，成功后 flash

## 环境事实
- ESP-IDF: v6.0.2 @ WSL `/root/esp/v6.0.2/esp-idf`
- 构建: `wsl bash /tmp/wb.sh`（脚本源在 tools/wslbuild.sh，先拷 /tmp，见 ERR-002）
- 芯片: ESP32-C6 @ WSL /dev/ttyACM0（usbipd busid 1-8, VID:PID 303a:1001）
- MAC: 14:c1:9f:e5:11:28
- 上位机: Windows 侧 Python + Bleak（蓝牙适配器 1-9: 10d7:b012）
