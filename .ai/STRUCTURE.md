# 项目结构

## 概览
双端项目：
- `firmware/` — ESP32-C6 固件（ESP-IDF v6.0.2，WSL 构建）
- `host/` — Windows 上位机（BLE OTA 测试工具，Windows 运行）

## 分层
```
firmware/            interface+core 合一（小体量固件，按 ESP-IDF 组件习惯组织）
  main/              应用入口 + BLE OTA 服务实现
  partitions.csv     OTA 双分区表
  sdkconfig.defaults C6 + NimBLE 配置
host/
  ble_ota.py         上位机主程序（扫描/连接/传输/校验/激活）
tools/
  build.sh           WSL 一键构建脚本
  flash.sh           烧录脚本
```

## 关键决策
- 上位机用 Python + Bleak（跨 Windows BLE 栈最稳，pip 即装）
- 固件用 NimBLE（IDF v6 默认蓝牙栈，GATT 服务自定义 OTA UUID）
- 分区表：ota_0 + ota_1 双 OTA 槽 + otadata
