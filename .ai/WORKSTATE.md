# 当前工作状态

## 项目
ESP32-C6 BLE OTA — 基于 espressif/ble_ota v0.1.17 官方组件的固件 + Windows 上位机测试工具

## 正在进行
- [TASK-001] 跑通 ESP32-C6 hello_world **✓ 完成** | 2026-09-01 16:30
- [TASK-002] BLE OTA 固件 **✓ 完成（固件侧+真机启动验证）** | 2026-09-01 19:30
  - 三层代码 + 官方组件集成，构建零告警，烧录成功
  - 真机启动锚点全命中：version=1.0.0 / run_part=ota_0 / img_state=VALID / rollback no-action / adv C6-OTA-1128 / ota_task ready target=ota_1
  - smoke-tester 独立验证 PASS-with-notes（遗留：GitHub 落后提交已补推、分区注释已修）
- [TASK-003] Windows 上位机（Bleak）| 开工 | pc-host-engineer
  - [验证: shared✗ core✗ interface✗ presentation✗]

## 未完成队列
- [ ] TASK-003 Windows 上位机（Bleak，扫描-连接-传输-校验-激活；协议要点与组件契约见 STRUCTURE.md 关键机制节）
- [ ] TASK-004 全链路 BLE OTA 联调（含回滚测试：升级后验证 PENDING_VERIFY→confirmed 锚点）
- [注意] 烧录后需手动按 RST 键启动 app（ERR-003: usbipd 虚拟复位不可信）
- [注意] GitHub 推送需走代理 `git -c http.proxy=http://127.0.0.1:7890 push github master`（系统代理 ProxyEnable=0，直连被重置）

## 上次中断点
- 文件: host/（待创建）
- 操作: TASK-002 完结，TASK-003 开工
- 待恢复: 调度 pc-host-engineer 写 Bleak 上位机

## 环境事实
- ESP-IDF: v6.0.2 @ WSL `/root/esp/v6.0.2/esp-idf`
- 构建: `wsl bash /tmp/wb.sh`（脚本源在 tools/wslbuild.sh，先拷 /tmp，见 ERR-002）
- 芯片: ESP32-C6 @ WSL /dev/ttyACM0（usbipd busid 1-8, VID:PID 303a:1001）
- MAC: 14:c1:9f:e5:11:28
- 上位机: Windows 侧 Python + Bleak（蓝牙适配器 1-9: 10d7:b012）
- 组件参考源码: WSL /root/bleota_ref_persist/（ble_ota v0.1.17 全量 + example）
