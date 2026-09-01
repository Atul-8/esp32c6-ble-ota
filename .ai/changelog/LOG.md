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
- 17:15 [完成] git 仓库初始化 + README/.gitignore + 首次提交（hello_world 基线 + 构建脚本 + .ai 协调层）
- 17:20 [完成] Gitee 建仓 waterguy/esp32c6-ble-ota + 推送（合并远程 LICENSE 初始提交）
- 17:25 [完成] GitHub 建仓 Atul-8/esp32c6-ble-ota + 推送 + 10 个 issues（M1×4 固件 / M2×3 上位机 / M3×3 联调文档）+ 6 标签
- 17:25 [记录] ERR-004: Gitee PAT 缺 issues scope，POST issue 404（伪装），issues 主战场放 GitHub，Gitee 做代码镜像

- 18:40 [完成] TASK-002 固件编码+构建+烧录：ble_ota v0.1.17 组件接入（main+ota_core+ota_shared 三层），构建零告警，app bin 656864B（ota_0 槽占用 36%），flash 4 镜像 hash verified（涉及文件: firmware/main/*, firmware/components/*, firmware/partitions.csv, firmware/sdkconfig.defaults）
- 18:40 [修复] ERR-005: ble_ota 组件 extern 引用 app 全局 notify_sem（隐式契约），static 化导致链接失败 → 恢复非 static 全局（META-001）；另核实 IDF v6 下 host_init 不含 controller init，app_main 显式补齐（META-002）
- 18:40 [记录] 跨重启续传结论：ble_ota 组件重启后 cur_sector 归零不支持跨重启续传；固件交付 sector 级进度落盘（ota_prog/progress）+ resume info 日志，续传由上位机配合 Indicate ACK 实现
- 19:17 [完成] TASK-002 固件侧交付：smoke-tester 独立验证 PASS-with-notes（产物/偏移/分层/ai 一致性全过）
- 19:25 [修复] partitions.csv 注释偏移修正（app 对齐 0x10000，ota_0 实际 0x20000）+ GitHub 补推（走 7890 代理）
- 19:30 [完成] 真机启动验证：全日志锚点命中（v1.0.0/ota_0/VALID/no-action/C6-OTA-1128/ota_task target=ota_1）→ TASK-002 完结，开工 TASK-003
