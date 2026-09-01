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
  main/                            interface 层：BLE 接线 + OTA 落盘
    main.c                         app_main：NVS→进度日志→版本日志→回滚确认→controller init→host_init→设备名覆盖→回调注册→ota_task→adv 锚点
    ota_task.c/.h                  ringbuf 解耦收包与落盘；每 sector 进度保存；esp_ota_begin/write/end→set_boot_partition→restart
    idf_component.yml              声明 espressif/ble_ota ^0.1.17（拉 cjson/esp_encrypted_img/cmake_utilities）
  components/ota_shared/           shared 层：ota_version.h（版本/设备名/sector 常量）+ ota_nvs_keys.h（NVS 命名空间/键名），纯宏无代码
  components/ota_core/             core 层：ota_rollback.c（自检+PENDING_VERIFY 确认/回滚）、ota_progress_store.c（NVS blob 进度存取），0 个 ESP_LOG/printf
host/
  ble_ota.py                       上位机主程序（扫描/连接/传输/校验/激活）（TASK-002 待开发）
tools/
  wslbuild.sh                      WSL 一键构建/烧录脚本（rsync→~/c6src 原生构建→产物回拷，规避 ERR-001）
```

## 固件依赖方向（严格单向）
main → ota_core → ota_shared；main → ota_shared；main → ble_ota 组件（managed_components，禁改）
ota_core 禁止 include NimBLE/ble_ota/esp_log 头（构建后 grep 自查通过）

## 关键机制
- 回滚：CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE，新镜像首启 PENDING_VERIFY → ota_rollback_confirm()（BLE 初始化前，NVS 读写回路 + 堆>50KB 自检）→ 标记 VALID 或回滚重启
- 进度持久化：ota_task 每收满 1 sector（4096B）→ ota_progress_save（NVS blob ota_prog/progress）→ 激活后 clear
- 组件隐式契约：ble_ota 组件 Stop 处理器 extern 引用 app 全局 `notify_sem`（见 ERR-005/META-001），ota_task.c 中必须非 static
- IDF v6 controller 归属：esp_ble_ota_host_init 只初始化 host，controller init/enable 由 app_main 显式做（META-002）
- 设备名：组件硬编码 "nimble-ble-ota"，app_main 在 host_init 返回后立即 ble_svc_gap_device_name_set("C6-OTA-1128") 覆盖（广播 fields 构造时实时读取，时序安全）

## 关键决策
- 上位机用 Python + Bleak（跨 Windows BLE 栈最稳，pip 即装）
- 固件用 espressif/ble_ota v0.1.17 官方组件 NimBLE 模式（协议带 sector CRC16 + Indicate ACK 断点续传），不自研 GATT profile
- 分区表：ota_0 + ota_1 双 OTA 槽（各 1.75MB）+ otadata，无 factory（首发烧 ota_0）
- 跨重启续传边界：组件重启后 cur_sector 归零，不支持跨重启续传（源码确认）；本固件交付进度落��� + 可观测性，续传由上位机配合 Indicate ACK 实现
