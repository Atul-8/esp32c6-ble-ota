# 工程经验总结——ESP32-C6 BLE OTA 项目（Lessons Learned）

> 本文档沉淀项目全程的实战经验：环境搭建、联调方法论、踩坑模式。
> 完整调试过程见 `.ai/errors/raw/ERR-001~009.md`；跨项目复用规则已回流全局 META 仓库（META-189~198）。

## 一、环境搭建经验（Windows + WSL + usbipd 工作流）

### 1.1 推荐工作流架构

```
源码托管（Windows, /mnt/f/...）          ← git 仓库、IDE 编辑
      │  tools/wslbuild.sh: rsync（排除 build/sdkconfig）
      ▼
构建工作区（WSL, ~/c6src）              ← idf.py build/flash（原生 ext4）
      │  产物回拷（bin/elf/map/sdkconfig）
      ▼
产物归档（Windows, firmware/build/）    ← 版本管理、上位机读取
      │
上位机（Windows Python + Bleak）        ← 蓝牙在 Windows 侧，WSL 摸不到 BLE
```

**为什么这样分**：
- 中文/非 ASCII 路径经 /mnt（9p 挂载）直接构建 → GCC specs 路径损坏（ERR-001，META-195）
- 9p 文件系统构建速度比原生 ext4 慢 5-10 倍
- 蓝牙适配器是 USB 设备，BLE 栈在 Windows，上位机必须 Windows 运行

### 1.2 关键命令速查

```bash
# 构建（唯一合法方式——脚本内含 rsync 同步 + 原生构建 + 产物回拷）
wsl -e bash -c 'cp "/mnt/f/project/嵌入式项目/esp32c6ota/tools/wslbuild.sh" /tmp/wb.sh && bash /tmp/wb.sh'

# 烧录（同脚本带 flash 参数）
wsl -e bash -c 'cp tools/wslbuild.sh /tmp/wb.sh && bash /tmp/wb.sh flash'

# IDF 环境注入（禁止 source 激活脚本——$0 检测会自杀退出，ERR-002/META-196）
while IFS='=' read -r k v; do case "$k" in
  PATH) export PATH="$v:$PATH";;
  IDF_*) export "$k=$v";;
esac; done < <(bash /root/.espressif/tools/activate_idf_v6.0.2.sh -e)

# 串口静默监听（pyserial，open 后等 ROM 头打完再读；tools/serialmon.py、tools/capture.py）
# 复位后验证 app 启动：手动按 RST 键（ERR-003/META-197：usbipd 软件复位不可信）
```

### 1.3 usbipd 要点
- 透传：`usbipd bind --busid <id>` + `usbipd attach --wsl --busid <id>`（管理员权限）
- C6 的 USB-Serial/JTAG 在软件复位后可能掉线重枚举 → usbipd 链路 URB -104 超时 → detach/attach 重挂
- 芯片族：C3/C6/H2（USB-Serial/JTAG 外设）；经典 UART 桥（CP2102/CH340）不受 ERR-003 影响

## 二、BLE OTA 联调方法论

### 2.1 协议对接三板斧
1. **先读组件源码再写上位机**——README 协议描述不完整（如 RECV_FW ACK 无帧头、COMMAND ACK 有帧头，META-193）
2. **设备端串口日志与主机端日志并行抓取**——单侧日志会误判责任方（ERR-008 的"坏帧"实为合法全零帧）
3. **失败先看设备侧**——GATT Unreachable ≠ 设备崩溃，可能是任务冻死（ERR-009：无 panic 无 WDT 的静默挂死）

### 2.2 静默挂死的诊断路径（ERR-009 经验）
```
无 panic、无 WDT 复位、串口静默、USB 仍枚举、不广播
→ ① 查任务阻塞点：谁在等谁？（ota_task 死于自体 take，host 任务死于 ringbuf 满）
→ ② 检查同步原语计数：互斥信号量 count 脱离 0/1 值域 = 泄漏
→ ③ diff 对照官方 example：take/give 配对逐行对齐（META-194）
```

### 2.3 本项目踩坑模式总结（详见 META）

| 模式 | 一句话规则 | META |
|---|---|---|
| 隐式契约 | 组件 extern 引用 app 全局 = 要求你提供符号，禁 static 化 | 189 |
| 框架异步回调 | 回调读的配置，写入必须先于 init（广播名竞态） | 191 |
| 跨任务参数 | 命令注入型参数禁启动缓存，实时读取 | 192 |
| ACK 校验 | 合法性=CRC 自校验，全零可能是成功帧 | 193 |
| 信号量配对 | 循环末尾无条件 give，任何路径丢失=流水线冻结 | 194 |

## 三、BLE OTA 协议速查（espressif/ble_ota v0.1.17 NimBLE 模式，逐行核对源码）

### 3.1 GATT 表
| 特征 | UUID | 属性 | 用途 |
|---|---|---|---|
| OTA Service | 0x8018 | — | 服务 |
| RECV_FW | 0x8020 | write + indicate | 固件数据包 ↓ / sector ACK ↑ |
| OTA_BAR | 0x8021 | read + indicate | 进度 |
| COMMAND | 0x8022 | write + indicate | 控制命令 ↓ / 命令 ACK ↑ |
| CUSTOM | 0x8023 | write + indicate | 自定义 |

### 3.2 帧格式（CCITT-XMODEM CRC16：poly 0x1021，init 0x0000）
- **数据包**（→0x8020）：`[Sector_Index:2 LE][Packet_Seq:1][payload(MTU-3-3)]`；Packet_Seq=0xFF 表 sector 尾包，payload 尾带 2B sector CRC16；每 sector 4096B
- **sector ACK**（0x8020 ↑，20B，**无固定头**）：`[回显sector:2][status:2][期望sector:2][零填充...][CRC16:2]`
  - status 0x0000 成功 / 0x0002 sector 错（期望值在 bytes[4:6] = 续传依据）
  - **sector 0 成功 ACK = 20 字节全零（CRC(18×00)=0），合法帧**
- **命令**（→0x8022）：Start `01 00 <size:4 LE>` / Stop `02 00`
- **命令 ACK**（0x8022 ↑，20B，**有固定头 0x03 0x00**）：`[03 00][status:2][零填充...][CRC16:2]`，Start 成功 status=0x0001

### 3.3 断点续传语义
- 4KB sector 粒度；上位机流水线发 sector 内包，**sector 尾包后必须等 ACK** 再发下一 sector
- ACK 0x0002 → 直接跳到期望 sector（设备已重置内部缓冲，从 packet 0 重收该 sector）
- 组件重启后 cur_sector 归零 = 不支持跨重启续传；重连续传靠上位机记住最后成功 sector + 重走 Start
- 设备端进度持久化（NVS，本项目 ota_core/ota_progress_store）只做可观测性 + 完成清零

## 四、验证锚点（grep 友好）

固件启动日志（main.c/ota_task.c 固定格式）：
```
[BLE_OTA] version=1.0.1, run_part=ota_0, img_state=VALID
[BLE_OTA] rollback check: PENDING_VERIFY -> confirmed
[BLE_OTA] adv started, name=C6-OTA-1128
[BLE_OTA] ota_task ready, target=ota_1, wait Start cmd
[BLE_OTA] progress saved: sector=N, offset=bytes
[BLE_OTA] resume info: offset=x/size=y (from last session)
```

上位机成功判据：进度条 100% + `OTA SUCCESS` + 退出码 0；设备 3s 内重新广播。

## 五、已知限制与优化方向

1. **传输速率 14 KB/s**：esp_ota_write 每包同步落盘 + NVS 每 sector commit；可改批量写/降低 commit 频率
2. **跨重启续传不支持**：组件 cur_sector 不持久化（vendor patch 可解，成本高）；当前依赖上位机侧续传
3. **回滚实战未跑**：机制就位（PENDING_VERIFY→自检→confirm/回滚），构造"自检失败版"的实测留待 issue #9
4. **vendor 组件维护**：components/ble_ota 是 v0.1.17 + 设备名 Kconfig patch，升级组件版本需重新 vendor 并重打 patch（勿在 idf_component.yml 重新声明 espressif/ble_ota，会重名冲突）
