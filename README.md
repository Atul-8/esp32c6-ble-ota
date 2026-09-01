# ESP32-C6 BLE OTA 全链路项目

基于 ESP-IDF v6.0.2 的 ESP32-C6 BLE OTA 完整实现：**固件端**（NimBLE + Espressif 官方 ble_ota 组件）+ **Windows 上位机**（Python Bleak），支持**断点续传、版本回滚、CRC 校验**。

## 项目背景

在 ESP32-C6 开发板上实现蓝牙 OTA 固件升级全链路：手机/PC 通过 BLE 连接设备 → 分包传输新固件 → 写入 OTA 分区 → CRC 校验 → 切换启动分区 → 失败自动回滚。

## 核心特性

| 能力 | 实现方式 |
|---|---|
| BLE 传输协议 | [espressif/ble_ota](https://components.espressif.com/components/espressif/ble_ota) v0.1.17 官方组件（每 4KB sector CRC16 + 命令包 CRC16） |
| 断点续传 | 协议 ACK 带期望 Sector_Index + IDF v6 `esp_ota_resume()` + NVS 进度持久化（4KB 粒度） |
| 版本回滚 | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + `esp_ota_mark_app_valid_cancel_rollback()`，自检不过自动回滚 |
| 上位机 | Python 3 + [Bleak](https://github.com/hbldh/bleak)（Windows BLE 栈），扫描→连接→传输→校验→激活→验证 |
| 构建环境 | WSL2 + ESP-IDF v6.0.2（EIM 安装），usbipd 透传 USB |

## 目录结构

```
firmware/                  ESP32-C6 固件（ESP-IDF 项目）
  main/                    interface 层：NimBLE/ble_ota 接线、日志
  components/ota_core/     core 层：回滚自检、NVS 进度存储（0 printf）
  components/ota_shared/   shared 层：版本宏、NVS 键名、常量
  partitions.csv           双 OTA 槽分区表���ota_0 + ota_1 + otadata）
  sdkconfig.defaults       C6 + NimBLE + 回滚配置
host/                      Windows 上位机
  ble_ota_host.py          Bleak 客户端（扫描/连接/传输/校验/激活）
tools/
  wslbuild.sh              WSL 一键构建脚本（自动同步原生路径+构建+产物回拷）
  serialmon.py             串口静默监听（复位后抓启动日志）
.ai/                       开发状态持久化（WORKSTATE/需求/错误知识库）
```

## BLE OTA 协议（espressif/ble_ota 官方协议）

- **OTA Service**：`8EC90001-F315-4F60-9FB8-838830DAEA50`（基座）
- **RECV_FW Char** `...0002`：固件数据包（Write+Notify ACK）
- **COMMAND Char** `...0003`：控制命令（Start/Stop/ACK，20B 定长 + CRC16）
- 数据包：`[Sector_Index:2][Packet_Seq:1][Payload]`，sector 尾包含 4KB CRC16
- ACK：`[Sector_Index:2][Status:2][CRC16:2]`，Status=0x0002 时附期望 Sector（续传依据）

## 快速开始

### 固件（WSL）

```bash
# 构建并烧录（脚本自动处理 WSL 原生路径同步，规避中文路径坑）
wsl -e bash -c 'cp tools/wslbuild.sh /tmp/wb.sh && bash /tmp/wb.sh'
wsl -e bash -c 'cp tools/wslbuild.sh /tmp/wb.sh && bash /tmp/wb.sh flash'

# 烧录后按板上 RST 键启动（usbipd 虚拟复位 bug，见 .ai/errors/raw/ERR-003.md）
```

### 上位机（Windows）

```bash
pip install bleak
python host/ble_ota_host.py --scan            # 扫描 C6-OTA-xxxx
python host/ble_ota_host.py --flash new.bin   # 传输+校验+激活
```

## 环境要求

- ESP32-C6 开发板（4MB flash）
- WSL2 + ESP-IDF v6.0.2 + usbipd-win（USB 透传）
- Windows + 蓝牙适配器（BLE 5）+ Python 3.10+

## 开发路线（issue 驱动）

见 Gitee/GitHub Issues：里程碑 M1 固件（ble_ota 集成/回滚/续传）→ M2 上位机 → M3 全链路联调（含回滚测试）。

## License

MIT
