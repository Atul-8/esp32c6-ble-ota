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
- [x] **固件修复 ERR-007** ✓ 完成（embedded-firmware-engineer 2026-09-02）：ota_task.c fw_len 启动缓存已删除，数据分支内实时读取；构建零错误 bin=656,096B，已烧录（真机运行验证归 TASK-004）
- [x] **固件修复 ERR-006** ✓ 完成（embedded-firmware-engineer 2026-09-02）：ble_ota v0.1.17 vendor 至 firmware/components/ble_ota，设备名走 CONFIG_BLE_OTA_DEVICE_NAME="C6-OTA-1128"（Kconfig），app_main 事后覆盖删除；构建零错误 + 烧录成功，bin strings 仅含 C6-OTA-1128；**空口广播名待 PM 手动 RST 后 Windows 扫描确认**
- [ ] TASK-003 收尾：固件修复后跑一次真实 .bin 全链路传输（上位机侧已就绪，期望进度条+断电续传+reboot 检测全链路验证）
- [x] **TASK-003/FIX 上位机 ERR-008 修复 ✓ 完成**（pc-host-engineer 2026-09-02）：RECV_FW sector ACK 无固定头（frame[0:2]=回显 sector），host 误用 COMMAND ACK 的 0x0003 头判定导致 sector 0 全零成功帧（CRC(18×00)=0x0000 自洽）被拒。已改为 len==20+CRC 自校验 + 修正 docstring + waiter 异常回收硬化。[验证: 协议单测✓ 真机 sector 0/1/2 ACK 接受✓ presentation(CLI 实跑)✓]
- [ ] **TASK-004 全链路 BLE OTA 联调——当前阻塞项为固件侧 ERR-009**（2026-09-02）：传输推进到 sector 2-3 时设备在 sector 落盘时机硬挂死（不广播/串口静默/无 panic/WDT 不复位，仅 esptool 硬复位可恢复；两次复现，证据见 errors/raw/ERR-009.md）。上位机侧已就绪，待 embedded-firmware-engineer 排查落盘路径与断连处理
- [注意] 烧录后需手动按 RST 键启动 app（ERR-003: usbipd 虚拟复位不可信）
- [注意] GitHub 推送需走代理 `git -c http.proxy=http://127.0.0.1:7890 push github master`（系统代理 ProxyEnable=0，直连被重置）
- [注意] 上位机：ERR-006 已修复（广播名 C6-OTA-1128），待 PM 空口确认后 scan 名字过滤可恢复；联调前仍可用 `--mac 14:C1:9F:E5:11:2A`
- [注意] components/ble_ota 为本地 vendor 组件（v0.1.17 + 设备名 patch），勿再从 main/idf_component.yml 声明 espressif/ble_ota（会与本地组件重名冲突）；升级组件时重新 vendor 并重打 patch

## 上次中断点
- 文件: host/ble_ota_host.py（send_sector 帧校验，ERR-008 已修复）+ firmware 侧 ERR-009 挂死待查
- 操作: ERR-008 修复完成并真机验证（sector 0/1/2 ACK 全部接受）；ERR-009 证据收集完成（设备已 esptool 硬复位恢复广播，勿再误判设备丢失）
- 待恢复: 固件侧排查 ERR-009（sector 落盘挂死，建议顺序：任务 WDT 覆盖 → esp_ota_write 调用上下文与 flash 总线竞争 → BLE_GAP_EVENT_DISCONNECT 后是否重启广播）→ 恢复后重跑 `python ble_ota_host.py flash ../firmware/build/esp32c6_ble_ota.bin --mac 14:C1:9F:E5:11:2A`（161 sectors，上位机侧已就绪）

## 环境事实
- ESP-IDF: v6.0.2 @ WSL `/root/esp/v6.0.2/esp-idf`
- 构建: `wsl bash /tmp/wb.sh`（脚本源在 tools/wslbuild.sh，先拷 /tmp，见 ERR-002）
- 芯片: ESP32-C6 @ WSL /dev/ttyACM0（usbipd busid 1-8, VID:PID 303a:1001）
- MAC: 14:c1:9f:e5:11:28
- 上位机: Windows 侧 Python + Bleak（蓝牙适配器 1-9: 10d7:b012）
- 组件参考源码: WSL /root/bleota_ref_persist/（ble_ota v0.1.17 全量 + example）
