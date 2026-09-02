# 当前工作状态

## 项目
ESP32-C6 BLE OTA — 基于 espressif/ble_ota v0.1.17 官方组件的固件 + Windows 上位机测试工具

## 正在进行
- [TASK-001] 跑通 ESP32-C6 hello_world **✓ 完成** | 2026-09-01 16:30
- [TASK-002] BLE OTA 固件 **✓ 完成（固件侧+真机启动验证）** | 2026-09-01 19:30
  - 三层代码 + 官方组件集成，构建零告警，烧录成功
  - 真机启动锚点全命中：version=1.0.0 / run_part=ota_0 / img_state=VALID / rollback no-action / adv C6-OTA-1128 / ota_task ready target=ota_1
  - smoke-tester 独立验证 PASS-with-notes（遗留：GitHub 落后提交已补推、分区注释已修）
- [TASK-003] Windows 上位机（Bleak）| **核心完成：scan/info/dry-run 真机实测通过；真实传输被固件 ERR-007 阻塞** | pc-host-engineer | 2026-09-01
  - host/ble_ota_host.py 626 行：scan / info / flash(+--dry-run/--mac) 全实现
  - 真机验证：scan 发现 14:C1:9F:E5:11:2A（-68dBm）；info GATT 表全对（8018/8020-8023，MTU=517 协商成功）；dry-run Start ACK=03000100...56a8 / Stop ACK=03000200...1b40 逐字节符合源码契约
  - **固件阻塞项 1（ERR-007）**：ota_task.c:65 fw_len 启动缓存恒 0，真实传数据必挂——需固件侧修复后才能全链路联调
  - **固件阻塞项 2（ERR-006）**：广播名竞态，空中名为 nimble-ble-ota 非 C6-OTA-1128；上位机已加 --mac 兼容，扫描可见
  - [验证: shared✓(CRC/分包单测) core✓(协议状态机逻辑) interface✓(真机 GATT+ACK 实测) presentation✓(CLI 实跑)]

## 未完成队列
- [ ] **固件修复 ERR-007**：ota_task.c fw_len 改为循环内实时读取（阻塞 TASK-004 全链路）
- [ ] **固件修复 ERR-006**：广播名时序（name_set 挪到 host_init 前 / 同步回调内改名重开广播）；修复后上位机 scan 名字过滤自动恢复
- [ ] TASK-003 收尾：固件修复后跑一次真实 .bin 全链路传输（上位机侧已就绪，期望进度条+断电续传+reboot 检测全链路验证）
- [ ] TASK-004 全链路 BLE OTA 联调（含回滚测试：升级后验证 PENDING_VERIFY→confirmed 锚点）
- [注意] 烧录后需手动按 RST 键启动 app（ERR-003: usbipd 虚拟复位不可信）
- [注意] GitHub 推送需走代理 `git -c http.proxy=http://127.0.0.1:7890 push github master`（系统代理 ProxyEnable=0，直连被重置）
- [注意] 上位机：设备广播名当前为 nimble-ble-ota（ERR-006），用 `--mac 14:C1:9F:E5:11:2A` 或 scan 表按 MAC 识别

## 上次中断点
- 文件: host/ble_ota_host.py（完成）/ firmware/main/ota_task.c:65（待固件侧修复）
- 操作: TASK-003 上位机核心完成；dry-run 真机握手通过
- 待恢复: 固件侧修 ERR-007/ERR-006 → 重烧 → 上位机真实 .bin 全链路传输验证（TASK-004）

## 环境事实
- ESP-IDF: v6.0.2 @ WSL `/root/esp/v6.0.2/esp-idf`
- 构建: `wsl bash /tmp/wb.sh`（脚本源在 tools/wslbuild.sh，先拷 /tmp，见 ERR-002）
- 芯片: ESP32-C6 @ WSL /dev/ttyACM0（usbipd busid 1-8, VID:PID 303a:1001）
- MAC: 14:c1:9f:e5:11:28
- 上位机: Windows 侧 Python + Bleak（蓝牙适配器 1-9: 10d7:b012）
- 组件参考源码: WSL /root/bleota_ref_persist/（ble_ota v0.1.17 全量 + example）
