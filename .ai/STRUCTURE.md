# 项目结构

## 概览
双端项目：
- `firmware/` — ESP32-C6 固件（ESP-IDF v6.0.2，WSL 构建）
- `host/` — Windows 上位机（BLE OTA 测试工具，Windows 运行）

## 分层
```
firmware/                          ESP32-C6 固件（七层分治映射见下）
  CMakeLists.txt                   顶层工程（PROJECT_VER=1.0.0 与 ota_version.h 同步）
  partitions.csv                   双 OTA 槽分区表（nvs/otadata/phy/ota_0/ota_1，无 factory）
  sdkconfig.defaults               C6 + NimBLE + MTU517 + 回滚使能 + 自定义分区表
  main/                            interface 层：BLE 接线 + OTA transport（REQ-004 PR-1 改造）
    main.c                         app_main：NVS→进度日志→版本日志→回滚确认→controller init→ble_ota_transport_init→host_init→数据回调注册→adv 锚点（设备名已下沉 vendor 组件；不再开机 esp_ota_begin）
    ble_transport.c/.h             BLE transport（vendor 桥接，REQ-004 PR-1 自 ota_task.c 迁移改造）：ringbuf 解耦 + 泵任务 lazy-open（首个数据才 begin）+ 会话边界回调对接（START=armed+invalidate+排空；STOP/DISCONNECT=清 armed）+ notify_sem 契约（META-001 非全局 static，R8 永不删除）+ open_blocked 数据源门控
    idf_component.yml              仅声明 idf（ble_ota 改本地 vendor，见 components/ble_ota）
  components/ble_ota/              ble_ota v0.1.17 本地 vendor（ERR-006）：patch nimble_ota.c 设备名走 CONFIG_BLE_OTA_DEVICE_NAME + PR-1 会话边界补丁（esp_ble_ota_set_session_cb：START/STOP/DISCONNECT 三处各一行回调，~25 行，组件保持会话无感知），自带 idf_component.yml 继续拉 cjson/esp_encrypted_img/cmake_utilities 依赖
  components/hxd019_shared/        shared 层（HXD019EU 红外遥控，TASK-004）：hxd019_protocol.h（四发码规则帧头/功能码/键名/模式常量）、hxd019_types.h（16B 帧/空调状态/错误码）、hxd019_codelist.h（码库索引类型）、src/hxd019_frame.c（帧构建纯函数：5B 简单/16B 状态 7 或 11 键/10B AV/学习匹配/校验和，零硬件依赖）、test/host_test_hxd019.py（host 金标准单测，ALL PASS）、test/test_hxd019_frame.c（组件自测，CONFIG_HXD019_SELFTEST）、Kconfig（11 键帧格式/温度编码 d/du 减 16/UART 接线/自测）、README.md（金标准对照表+datasheet 疑点清单）
  components/hxd019_core/          core 层：hxd019_codec.c（精简品牌索引：空调 4 体验版+6 常用、IPTV 前 8、TV 2 品牌；动态注册表 hxd019_codec_register_table 供 host/NVS 加载完整码库）、hxd019_session.c（绑定码组+空调状态缓存增量操作+F_code 特殊字节 TODO 钩子桩），0 个 ESP_LOG/printf/driver
  components/hxd019/               interface 层：hxd019_uart.c（uart_driver_install 57600-8N1 + RX 任务匹配应答/学习数据 + ESP_LOG 锚点 [HXD019] tx frame/match result/learn data；便捷 API hxd019_ac_power/temp/mode/fan/swing/sleep...；hxd019_match 异步回调收码组号）；未接入 main（库只交付不启用，联调时 main REQUIRES 加 hxd019）
  components/ota_shared/           shared 层：ota_version.h（版本/设备名/sector 常量）+ ota_nvs_keys.h（NVS 命名空间/键名），纯宏无代码
  components/ota_core/             core 层：ota_rollback.c（自检+PENDING_VERIFY 确认/回滚）、ota_progress_store.c（NVS blob 进度存取）、ota_sink.c/.h（REQ-004 PR-1：统一会话编排——IDLE→OPEN→WRITING→VALIDATED→ACTIVATED 状态机、epoch 会话代数 P0-1 修复、单写者互斥先到先得、延迟 abort 并发模型、选槽 P1-5/size 校验 P1-4/begin 时机 P1-7、事件回调 SESSION_START/PROGRESS/ERROR/VALIDATED/ACTIVATED；test/host_test_ota_sink.py 21 用例金标准单测），0 个 ESP_LOG/printf
host/
  ble_ota_host.py                  上位机 CLI（scan/info/flash --dry-run/--mac；BleakOtaClient 协议状态机：Start/Stop ACK、sector 分包+尾包0xFF+CRC、ACK 超时重发、0x0002 跳 sector 断点续传；协议契约以源码核实为准——ACK 20B 无 cmd_echo、CRC=XMODEM 变体 init 0x0000、特征仅 WRITE-with-response；event_cb 事件回调 + emit() 供 GUI 挂接，CLI 行为不变）
  ble_ota_gui.py                   tkinter 可视化测试台（复用 ble_ota_host 协议核心，不重复协议逻辑；OtaWorker：asyncio loop 线程持有全部 BLE I/O，事件经 queue.Queue 由 root.after(60ms) 轮询泵入 UI 线程，GUI→worker 走 call_soon_threadsafe 且参数为主线程快照纯值（ERR-011）；OtaGui：深色工程主题 1180x760——工具栏扫描/设备列表/固件选择/dry-run、161 格 sector 热力图（状态着色+悬停 tooltip+脉冲）、进度条/速率/ETA/统计（packets/跳转/重发/重连/MTU）、事件日志（时间戳+verbose）、结果横幅+开始/停止；停止为协作式取消（sector 边界检查）
tools/
  wslbuild.sh                      WSL 一键构建/烧录脚本（rsync→~/c6src 原生构建→产物回拷，规避 ERR-001）
```

## 固件依赖方向（严格单向）
main → ota_core → ota_shared；main → ota_shared；main → ble_ota 组件（本地 vendor firmware/components/ble_ota，main/CMakeLists.txt 显式 REQUIRES）
ota_core 禁止 include NimBLE/ble_ota/esp_log 头（构建后 grep 自查通过）；ota_sink 事件回调上抛给 transport，依赖方向保持单向
hxd019 → hxd019_core → hxd019_shared（TASK-004，未接 main）；hxd019_private→esp_driver_uart（IDF v6 driver 拆分）；hxd019_shared/core 零 ESP_LOG/printf/driver 头（grep 自查通过，自测文件 Kconfig 关闭不编译）

## 关键机制
- 回滚：CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE，新镜像首启 PENDING_VERIFY → ota_rollback_confirm()（BLE 初始化前，NVS 读写回路 + 堆>50KB 自检）→ 标记 VALID 或回滚重启
- OTA 会话（REQ-004 PR-1）：唯一所有者=ota_sink；transport 桥接 vendor 组件——START 回调=armed+ringbuf 排空+epoch_invalidate（旧会话 ABORT_PENDING 延迟到泵任务串行 abort）；泵任务首个 chunk lazy-open（选槽/size 校验/begin）；数据收满即 finish+activate(reboot)；Stop/断连只清 armed（收尾竞态：尾 ACK 后 Stop 可抢在 esp_restart 前返回），流死亡由泵任务 100ms 超时路径 abort（"session abort by ble"）。详见 ble_transport.c 文件头 + ERR-013
- 进度持久化：ota_sink 每收满 1 sector（4096B）→ ota_progress_save（NVS blob ota_prog/progress）→ 激活后 clear
- 组件隐式契约：ble_ota 组件 Stop 处理器 extern 引用 app 全局 `notify_sem`（见 ERR-005/META-001），ble_transport.c 中必须非 static（初值 1=Stop 令牌，永不删除 R8/P2-10）
- IDF v6 controller 归属：esp_ble_ota_host_init 只初始化 host，controller init/enable 由 app_main 显式做（META-002）
- 设备名（ERR-006 修复后）：vendor 组件在 esp_ble_ota_host_init() 内、host task 启动前 ble_svc_gap_device_name_set(CONFIG_BLE_OTA_DEVICE_NAME)；sdkconfig.defaults 配置 "C6-OTA-1128"，sync_cb 构造广播字段读到的即最终值，无竞态窗口。app_main 不再事后覆盖（ota_version.h 的 OTA_BLE_DEVICE_NAME 宏保留，仅作 shared 层常量）
- 命名红线：main 内 transport 初始化函数必须带 ble_ota 前缀（ble_ota_transport_init）——NimBLE host 自带 nimble/transport.h 的 ble_transport_init(void)，裸用该名链接期冲突
- host 成功判定（P0-2 修复）：广播空窗观测（wait_reboot_with_gap）——真实 esp_restart 必然使广播消失 1.5-2.5s；9s 窗口出现空窗=PASS，持续可见=FAIL；Stop ACK 降级信息性事件（P2-14：3s 短超时）

## 关键决策
- 上位机用 Python + Bleak（跨 Windows BLE 栈最稳，pip 即装）
- 固件用 espressif/ble_ota v0.1.17 官方组件 NimBLE 模式（协议带 sector CRC16 + Indicate ACK 断点续传），不自研 GATT profile；v0.1.17 已 vendor 本地化（ERR-006），不再走组件 registry
- 分区表：ota_0 + ota_1 双 OTA 槽（各 1.75MB）+ otadata，无 factory（首发烧 ota_0）
- 跨重启续传边界：组件重启后 cur_sector 归零，不支持跨重启续传（源码确认）；本固件交付进度落��� + 可观测性，续传由上位机配合 Indicate ACK 实现
