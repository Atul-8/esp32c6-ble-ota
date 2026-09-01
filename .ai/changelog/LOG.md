# 操作履历

- 15:50 [决策] 环境确认：WSL ESP-IDF v6.0.2 (EIM 安装)，C6 @ /dev/ttyACM0，esptool chip-id 通过，MAC 14:c1:9f:e5:11:28
- 15:50 [决策] 技术选型：固件 NimBLE GATT OTA；上位机 Python Bleak（Windows 蓝牙栈）；分区表 ota_0/ota_1/otadata
- 15:50 [完成] .ai/ 协调层初始化
- 15:52 [修复] ERR-001 中文路径+9p 构建 specs 损坏 → tools/wslbuild.sh 源码同步 ~/c6src 原生构建
- 15:55 [修复] ERR-002 EIM 激活脚本 source 自杀 → 改 -e 模式逐行 export
- 15:58 [完成] hello_world 构建成功 (552/552)，产物回拷 Windows 目录
- 16:00 [完成] 烧录成功 (hash verified, 0x10000 镜像 0xE9 有效)
- 16:05-16:25 [阻塞] ERR-003 复位后 strap=0 恒进下载模式：7+ 种复位序列穷举、eFuse 排查、load_ram 绕过尝试均无效 → 结论：硬件 BOOT strap 问题，待用户断电重启/查 GPIO9
- 16:30 [完成] 用户手动 RST 后 app 正常启动，两轮 Hello world! 确认 → TASK-001 完结。ERR-003 根因修正：usbipd 虚拟复位时序问题（非硬件），后续烧录后手动 RST
- 16:30 [决策] 开工 TASK-002 BLE OTA 固件，调度 embedded-firmware-engineer
- 16:45 [决策] 用户指示用开源 OTA 库 → 调研结论：采用 espressif/ble_ota v0.1.17 官方组件（协议带 sector CRC16+断点续传），否决 fbiego（无 CRC 无续传已停更）；回滚用 IDF v6 原生 ROLLBACK_ENABLE + esp_ota_resume()；REQ-002 记录，手搓任务书作废

