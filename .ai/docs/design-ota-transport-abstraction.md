# OTA 传输层抽象设计（REQ-004）——统一 ota_sink + BLE/WiFi/USB 三通道

> 设计状态: 评审稿 | 2026-09-03 | 对应 REQ-004
> 关联: `.ai/docs/review-ota-20260902.md`（P0-1 会话错位链）、`.ai/errors/distilled/meta-rules.md`（META-001/004/005/006）
> 约束: 现有 BLE OTA 行为不可破坏（重构后 BLE 路径回归全绿）；小步可交付，禁止大爆炸重构；本文档只设计不改码。

---

## 0. 先决事实核查（本设计立论的硬件/SDK 证据，均已逐条核实）

| # | 事实 | 证据（IDF v6.0.2 源码） | 对设计的影响 |
|---|------|------------------------|-------------|
| F1 | **ESP32-C6 没有 USB-OTG 控制器**，只有 USB-Serial/JTAG（内置 PHY） | `soc_caps.h` 无 `SOC_USB_OTG_SUPPORTED`（S3 有=1，C3/C6 均无）；`components/esp_tinyusb` 不随 v6.0.2 分发 | USB OTA 唯一载体是 USB-Serial/JTAG，TinyUSB CDC 方案在 C6 上**物理不可行** |
| F2 | C6 USB 引脚 = **GPIO12(D-) / GPIO13(D+)**，非 GPIO18/19（18/19 是 C3 的引脚） | `components/soc/esp32c6/register/soc/io_mux_reg.h:139-140`（`USB_INT_PHY0_DM_GPIO_NUM 12`、`DP 13`；C3 同文件为 18/19） | 当前烧录口（usbipd VID:PID 303a:1001 = USB-Serial/JTAG）就是这个外设；"USB OTA 与烧录口冲突"问题真实存在，须软件策略化解 |
| F3 | IDF v6 提供 `esp_ota_resume(partition, erase_size, image_offset, out_handle)` | `app_update/include/esp_ota_ops.h:107`、`esp_ota_ops.c:207-` | WiFi Range 断点续传有官方写侧 API（v5 没有），progress NVS 从"可观测"升级为"续传坐标"的技术前提 |
| F4 | `esp_ota_begin` 已知 size 时只擦 `ALIGN_UP(size)`，`OTA_SIZE_UNKNOWN` 才全擦；不拒绝并发多 handle | `esp_ota_ops.c:186-198`（erase 计算）、`:147`（`esp_ota_init_entry` 仅查 running conflict / PENDING_VERIFY，**无全局单会话锁**） | ① 双 transport 并发 begin 不会被 IDF 拦截 → ota_sink 必须自己做单写者互斥；② begin 时机后移到"拿到 size 之后"可修复审阅报告 P1-7（当前开机即全擦 1.75MB） |
| F5 | `esp_https_ota` 内部直接调 esp_ota_begin/write/end/set_boot，**无 write 注入口** | `esp_https_ota/include/esp_https_ota.h` API 面（begin/perform/finish 闭环，无 per-chunk 回调） | WiFi 方案①无法与 ota_sink 对接（字节流绕开统一底层），详见 §5.2 |
| F6 | C6 支持 WiFi+BLE 硬件共存（coex），HP SRAM 统一池 ≈ 441.5KB | `soc_caps.h:476`（`SOC_COEX_HW_PTI=1`）；`esp_system/ld/esp32c6/memory.ld.in:25`（SRAM_SEG_END 0x4086E610 → 452,112B） | §6 内存预算的容量上限；"任务提示中的 406KB"按链接脚本实测口径修正为 441.5KB 总池 |
| F7 | USB-Serial/JTAG 驱动 API 可用，且 console 可配走 USJ | `esp_driver_usb_serial_jtag/include/driver/usb_serial_jtag.h`（install/read/write）；`esp_stdio/Kconfig:26`（`ESP_CONSOLE_USB_SERIAL_JTAG`） | PR-3 的传输载体与 console 复用路径；USB-OTG 外置 PHY 方案在 C6 无控制器可挂，直接排除 |

---

## 1. 背景与目标

### 1.1 需求
用户指示"ota 底层应该保持一致，只是传输方式做不同兼容"：新增 WiFi OTA 与 USB OTA，与 BLE OTA 共用同一套底层（esp_ota_ops 写盘、回滚确认、进度持久化、会话管理）。

### 1.2 现状资产盘点（复用，不动）
| 资产 | 层 | 位置 | 复用方式 |
|------|----|------|---------|
| esp_ota_ops 写盘（begin/write/end/set_boot） | IDF | — | ota_sink 的唯一写盘通道 |
| 回滚确认（PENDING_VERIFY→自检→mark valid/回滚） | core | `firmware/components/ota_core/src/ota_rollback.c` | 原样保留，三通道共享（重启后首启确认，与传输通道无关） |
| 进度 NVS blob（image_size/offset/sector） | core | `firmware/components/ota_core/src/ota_progress_store.c` | 保留；BLE=可观测，WiFi=续传坐标（§5.2） |
| NimBLE + vendor ble_ota v0.1.17（本地组件，可打补丁） | interface | `firmware/components/ble_ota/` | 保持不动 + 最小补丁加会话事件回调（§5.1） |
| 分区表（双 OTA 槽各 1.75MB + otadata） | — | `firmware/partitions.csv` | 不动。三通道写的是同一对槽位 |

### 1.3 要顺带解决的问题
审阅报告 P0-1（会话错位链）的根因是：**"会话"这个概念目前分裂在两处**——vendor 组件内部（cur_sector/fw_buf，Start/Stop/断连时重置）与 ota_task（recv_len/esp_ota handle，对此一无所知）。因此本设计的核心动作不是"给 ota_task 换个名字"，而是**把会话所有权收拢到 ota_sink 单一处**，让所有传输通道的会话边界（开始/中止/结束）都变成 sink 的显式事件。P0-1 的修复是抽象的副产品，不是额外的补丁。

---

## 2. 分层架构图

```
firmware/ 分层落点（七层分治映射）
================================================================================
transport 层（interface 层，三选一可共存，互不依赖）
  ┌────────────────────┐  ┌────────────────────┐  ┌────────────────────┐
  │ ble_transport      │  │ ota_wifi            │  │ ota_usb             │
  │ (main/, 改造自     │  │ (components/ota_wifi│  │ (components/ota_usb │
  │  ota_task.c)       │  │  新组件)             │  │  新组件)             │
  │ vendor ble_ota 桥接│  │ esp_http_client 流式│  │ USB-Serial/JTAG 帧协议│
  │ ringbuf+停止式泵   │  │ +Range 续传         │  │ +console 共存策略    │
  └─────────┬──────────┘  └─────────┬──────────┘  └─────────┬──────────┘
            │ recv_fw_cb / 会话事件    │ HTTP chunk             │ 帧
            ▼                        ▼                        ▼
  ═══════════════════ ota_sink（ota_core 新增，唯一会话所有者）═══════════════════
  会话状态机: IDLE → OPEN(size 校验/选槽) → WRITING → VALIDATED → ACTIVATED
  单写者互斥 + 会话代数 epoch（P0-1 修复点）+ 进度事件回调 + progress NVS 编排
            │
            ▼
  底层资产（现状原样复用）
  esp_ota_ops(begin/write/end/set_boot/resume) │ ota_rollback │ ota_progress_store
================================================================================
依赖方向: transport → ota_core(ota_sink) → ota_shared；ota_core 禁止 include
任何 transport 头（NimBLE/esp_http_client/usb_serial_jtag），延续现有 grep 自查纪律
```

要点：
- **ota_task.c 不再是"BLE 落盘任务"，其通用部分（选槽/begin/write/progress/end/set_boot）全部下沉为 ota_sink；其 BLE 特有部分（ringbuf、recv_fw_cb、notify_sem 契约）留在 ble_transport。**
- ringbuf 只属于 BLE 桥接（NimBLE host 回调上下文不能同步落盘）。WiFi 的 HTTP 循环在自有任务里顺序 read→write，天然无需 ringbuf；USB 帧协议每 sector ACK 即天然流控，同样无需 ringbuf。
- ota_sink 不感知"重启"策略：`ACTIVATED` 事件发出后由各 transport 决定 reboot 时机（BLE 保持现状即重启；WiFi/USB 可先回 ACK 再重启，避免上位机把"重启瞬间无应答"误判为失败——审阅报告 P0-2 的设备侧诱因之一）。

---

## 3. ota_sink 接口设计（C 头文件级伪代码）

落点：`firmware/components/ota_core/include/ota_sink.h` + `src/ota_sink.c`。

```c
/*
 * core 层：OTA 会话与写盘统一编排（ota_sink）
 * 规则：禁止 ESP_LOG/printf（错误经返回值/事件上报，延续 ota_core 现有纪律）；
 *       禁止 include 任何 transport 头（NimBLE/http_client/usb_serial_jtag）。
 */

/* ---------- 错误码 ---------- */
typedef enum {
    OTA_SINK_OK = 0,
    OTA_SINK_ERR_BAD_ARG,        /* 参数非法（size==0 等） */
    OTA_SINK_ERR_BUSY,           /* 已有活动会话（单写者互斥拒绝，不抢占） */
    OTA_SINK_ERR_NO_SESSION,     /* write/abort/finish 无活动会话 */
    OTA_SINK_ERR_SESSION_STALE,  /* 写入时检测到会话代数已过期（P0-1 兜底信号） */
    OTA_SINK_ERR_SIZE_INVALID,   /* image_size==0 或 > 目标分区容量（审阅报告 P1-4 设备侧校验收拢于此） */
    OTA_SINK_ERR_PARTITION,      /* 选槽失败 / 目标即 running 槽（顺带修 P1-5：选槽基准改用 running partition） */
    OTA_SINK_ERR_BEGIN_FAIL,     /* esp_ota_begin/resume 失败（含 PENDING_VERIFY 拒绝，打印 esp_err_to_name 修 P2-9） */
    OTA_SINK_ERR_WRITE_FAIL,     /* esp_ota_write 失败 */
    OTA_SINK_ERR_VALIDATE_FAIL,  /* esp_ota_end 镜像校验失败（= 会话错位/坏镜像的最终暴露点） */
    OTA_SINK_ERR_NVS,            /* progress 持久化失败（不致命，会话继续） */
} ota_sink_err_t;

/* ---------- 事件 ---------- */
typedef enum {
    OTA_SINK_EVT_SESSION_START,  /* esp_ota_begin/resume 成功，开始写盘 */
    OTA_SINK_EVT_PROGRESS,       /* 每写满 1 sector(4096B) 一次，data=ota_sink_progress_t* */
    OTA_SINK_EVT_ERROR,          /* 任一 OTA_SINK_ERR_*，data=ota_sink_error_t* */
    OTA_SINK_EVT_VALIDATED,      /* esp_ota_end 校验通过（镜像落盘完整） */
    OTA_SINK_EVT_ACTIVATED,      /* set_boot 完成（reboot 与否由 transport 决定） */
} ota_sink_event_t;

typedef struct {
    uint32_t image_size;      /* 会话声明的固件总长 */
    uint32_t bytes_written;   /* 已写入字节 */
    uint32_t sectors_done;    /* 已完成 sector 数 */
    bool     resumed;         /* 本次是否为续传会话 */
} ota_sink_progress_t;

typedef struct {
    ota_sink_err_t code;
    int32_t        idf_err;   /* 底层 esp_err_t 原值（供 interface 层打日志） */
} ota_sink_error_t;

typedef void (*ota_sink_event_cb_t)(ota_sink_event_t evt, const void *data, void *user_arg);

/* ---------- 会话配置 ---------- */
typedef struct {
    uint32_t   image_size;    /* 必须非 0 且 ≤ 目标分区 size（三通道均有来源：BLE Start 帧 / HTTP Content-Length / USB START 帧） */
    const char *source_tag;   /* "ble" / "wifi" / "usb"，仅用于事件日志 */
    bool       resume;        /* true: 读 progress NVS，esp_ota_resume 续写（WiFi 用）；
                                  false: 全新 esp_ota_begin（按已知 size 部分擦除，修 P1-7） */
} ota_sink_session_cfg_t;

/* ---------- 生命周期 ---------- */
void     ota_sink_init(ota_sink_event_cb_t cb, void *user_arg);   /* app_main 早期注册一次 */

/* 打开会话：选槽(基于 running partition, 修 P1-5) → size 校验 → 单写者互斥(否则 BUSY) →
 * begin/resume。仅在收到 transport 的 START 语义时调用——开机不再预 begin（修 P1-7）。 */
ota_sink_err_t ota_sink_session_open(const ota_sink_session_cfg_t *cfg, uint32_t *out_epoch);

/* 顺序字节流写入。内部完成：sector 计数 → progress NVS（每 sector）→ 完成长度判定。 */
ota_sink_err_t ota_sink_write(uint32_t epoch, const void *data, size_t len);

/* 中止当前会话：esp_ota_abort + 进度保留（可观测/可续传）+ 回 IDLE。失败自愈路径用。 */
ota_sink_err_t ota_sink_session_abort(uint32_t epoch);

/* esp_ota_end 镜像校验。失败时内部 abort 并回 IDLE（EVT_ERROR+VALIDATE_FAIL）。 */
ota_sink_err_t ota_sink_finish(uint32_t epoch);

/* set_boot 激活 + progress 清零。reboot=true 时 esp_restart（BLE 现状语义）。 */
ota_sink_err_t ota_sink_activate(uint32_t epoch, bool reboot);

/* ---------- 会话代数（P0-1 修复核心） ---------- */
/* transport 的"新会话开始/对端中止/链路断开"事件（BLE: vendor 补丁回调；USB: ABORT 帧；
 * WiFi: HTTP 请求中止）必须调用 ota_sink_epoch_invalidate()。此后所有携带旧 epoch 的
 * write 返回 OTA_SINK_ERR_SESSION_STALE——错位数据在进入 esp_ota_write 之前被拦截，
 * 而不是像现状那样写到 flash 偏移 N*4096 之后由 esp_ota_end 才暴露。 */
void     ota_sink_epoch_invalidate(void);
uint32_t ota_sink_epoch_current(void);
```

### 3.1 单写者互斥与仲裁策略

- **互斥实现**：sink 内部一把 mutex + `active_epoch`（0 = 空闲）。`session_open` 拿锁检查 active_epoch：非 0 → 返回 `BUSY`（拒绝，不抢占）。write/abort/finish/activate 校验 `epoch == active_epoch`，不匹配返回 `STALE`/`NO_SESSION`。
- **仲裁策略 = 先到先得，不做抢占**：抢占会让正在进行的传输静默死掉（复杂度与故障面都不值得）。BLE 常驻广播可连，WiFi/USB 会话开始前先 open，拿到 BUSY 即向上报"设备正忙"（上位机可提示等待/稍后重试）。对真实场景（同一台设备不会同时被两个通道推固件）这是足够的。
- **IDF 不兜底**（事实 F4）：esp_ota_begin 允许多 handle 并发 open 同一分区，sink 互斥是唯一防线——这也是"写盘统一收拢"的必要性论据之一。
- **状态查询与锁粒度**：`write` 一次 4KB 同步落盘可达百 ms 级，互斥锁只保护 open/abort/finish/activate 等状态转换；BUSY 探测在 open 的锁内完成，写路径持权后不再反复锁全流程。

### 3.2 会话代数（epoch）机制——P0-1 的修复原理

现状故障链（审阅报告 P0-1，逐字保留其触发路径）：
> BLE 断开→重连→Start：组件重置 cur_sector/fw_buf，但 ota_task 的 recv_len、esp_ota 写偏移、saved_sectors 一概不知道新会话开始 → 上位机从 sector 0 重发 → ota_task 把 sector 0 数据写到 flash 偏移 N*4096（esp_ota_write 纯顺序写）→ 镜像整体错位 → esp_ota_end 失败 → ota_task 自删，设备永久失去 OTA 能力，且上位机报"成功"。

抽象后的修复（三层防御，全部落在统一位置）：
1. **事件层**：vendor 补丁给 Start/Stop/断连加 app 回调（§5.1），ble_transport 收到即 `ota_sink_epoch_invalidate()`。USB/WiFi 同理（各自会话边界调用同一 API）。
2. **写入层**：`ota_sink_write(epoch, ...)` 发现 epoch 过期 → 返回 `STALE`，**数据不落盘**。ble_transport 的泵任务收到 STALE → abort 旧会话（esp_ota_abort 在泵任务里做，不在 NimBLE 回调里）→ 以新 epoch 重新 open → 从新会话的字节流继续。错位写入从机制上消失。
3. **收尾层**：`ota_sink_finish` 失败时不再"任务自删"（现状 `OTA_ERROR → vTaskDelete` 是设备失去 OTA 能力的直接原因），而是 abort + 回 IDLE + `EVT_ERROR`，sink 常驻等下一个会话——设备自愈。

### 3.3 ota_core 可独立测试（core 层纪律延续）

sink 的纯逻辑（size 校验规则、sector 计数、epoch 判定、BUSY/STALE 状态转移）抽成与 esp_ota_ops 无关的纯函数，做 host 端 Python/C 金标准单测（复用 hxd019_shared 的 `test/host_test_*.py` 模式，`.ai/STRUCTURE.md` 已有先例）。esp_ota_ops 依赖层在固件端以真机验收。

---

## 4. Kconfig 与编译组织

```
firmware/components/
  ota_core/                 # core：现有 ota_rollback/ota_progress_store + 新增 ota_sink.c/.h
  ota_wifi/                 # interface：wifi_sta.c + ota_wifi.c（Kconfig 独立开关）
  ota_usb/                  # interface：ota_usb.c（帧协议 + USJ 接线）
  ble_ota/                  # interface：vendor 不动（仅 §5.1 会话回调补丁）
firmware/main/
  ble_transport.c/.h        # 从 ota_task.c 改造（ringbuf/notify_sem/桥接）
  ota_task.c → 删除（职责拆入 ota_sink + ble_transport）
  main.c                    # app_main 按 Kconfig 分支接线
```

```kconfig
# ota_core/Kconfig（新增）
config OTA_TRANSPORT_BLE
    bool "BLE OTA transport (vendor ble_ota bridge)"
    default y
    select BT_NIMBLE_ENABLED
config OTA_TRANSPORT_WIFI
    bool "WiFi OTA transport (esp_http_client streaming)"
    default n
    depends on ESP_WIFI_ENABLED
config OTA_TRANSPORT_USB
    bool "USB OTA transport (USB-Serial/JTAG frame protocol)"
    default n
config OTA_PROGRESS_INTERVAL_SECTORS   # sink 每多少 sector 落一次 NVS（P2-16 缓解）
    int
    default 1      # BLE 保持现状行为；WiFi/USB 续传场景建议 4
config OTA_WIFI_TLS                    # WiFi 是否启用 TLS（内存代价见 §6）
    bool
    default n
```

- 三个 transport 组件**互不 REQUIRES**，各自只 REQUIRES `ota_core ota_shared`；ble_transport 在 main 内，main 按现状显式 REQUIRES ble_ota。
- main.c 接线模板：`ota_sink_init()` → `rollback_check()`（不变，先于任何通道 init）→ `#if CONFIG_OTA_TRANSPORT_BLE` ble_transport_init `#endif` → `#if CONFIG_OTA_TRANSPORT_WIFI` ota_wifi_init `#endif` → `#if CONFIG_OTA_TRANSPORT_USB` ota_usb_init `#endif`。
- 一期交付策略：`BLE=y` 默认不变；WIFI/USB 编译开关默认关，各 PR 打开后验收，保证任意时刻主工程可构建、BLE 路径可回归。

---

## 5. 三个 Transport 的技术选型与接缝

### 5.1 BLE——vendor 组件不动 + 最小补丁桥接（确定性最高）

**接缝分析**（`firmware/components/ble_ota/src/nimble_ota.c` 逐行核实）：
| 组件内位置 | 现状行为 | 与 sink 的接缝 |
|-----------|---------|---------------|
| `ble_ota_start_write_chr` 0x01 分支（:515-546） | Start：解析 fw_length，重置 cur_sector/cur_packet/fw_buf，回 ACK | **会话边界 ①**：补丁回调 `SESSION_START` → transport 等收到首个 sector 数据后再 `session_open(size)`（避免在 NimBLE 回调里同步做 esp_ota_begin 全擦） |
| `esp_ble_ota_recv_fw_handler`（:435-442） | 每 sector（4096B）调 `recv_fw_cb` | 不变：`recv_fw_cb` → ringbuf → 泵任务 → `ota_sink_write` |
| Stop 分支（:547-576） | 重置组件状态；**extern notify_sem 隐式契约**（META-001） | **会话边界 ②**：`SESSION_STOP` → epoch_invalidate。notify_sem 保留为 ble_transport 内非 static 全局，take/give 配对逐路径保全（META-006） |
| `esp_ble_ota_gap_event` DISCONNECT（:982-992） | 仅恢复广播，无 app 通知 | **会话边界 ③（P0-1 主触发器）**：补丁回调 `SESSION_DISCONNECT` → epoch_invalidate |
| `esp_ble_ota_get_fw_length()`（:429） | Start 命令写入 ota_total_len | open 时经 `ota_sink_session_cfg_t.image_size` 传入（保持 META-004：实时读取，不启动时缓存） |

**vendor 补丁设计**（组件已本地化，成本低）：
```c
/* nimble_ota.c 增量（约 25 行） */
typedef enum { ESP_BLE_OTA_SESSION_START, ESP_BLE_OTA_SESSION_STOP,
               ESP_BLE_OTA_SESSION_DISCONNECT } esp_ble_ota_session_evt_t;
typedef void (*esp_ble_ota_session_cb_t)(esp_ble_ota_session_evt_t evt);
void esp_ble_ota_set_session_cb(esp_ble_ota_session_cb_t cb);
/* 三个会话边界处各插一行 if (s_session_cb) s_session_cb(...); */
```

**notify_sem 契约安置**：随 ota_task.c 改造迁入 `ble_transport.c`，保持非 static、文件头注释声明契约来源（META-001），链接期 grep 自查纳入分层检查项。

**开发量**：vendor 补丁 ~25 行；`ble_transport.c` ~180 行（ota_task.c 改造，ringbuf/泵任务/epoch 重开逻辑）；main.c 接线 ~30 行。**上位机配套：零**（协议完全不变；PR-1 顺带可做审阅报告 P0-2 的 host 成功判定反转，~20 行，独立提交）。

### 5.2 WiFi——esp_http_client 流式 + ota_sink（推荐）｜三个候选的结论

| 候选 | 结论 | 理由 |
|------|------|------|
| ① esp_https_ota 官方组件 | **否决** | 事实 F5：其内部直接调 esp_ota_begin/write/end/set_boot，无 per-chunk 注入口——用它=WiFi 通道完全绕开 ota_sink（底层分叉，违背"底层一致"需求本体）；裁剪/fork 它改内部循环，维护成本远高于自研 ~300 行流式循环。可留作通路验证的对照实现 |
| ② esp_http_client 流式 + ota_sink | **推荐** | 字节流显式过 sink（底层唯一）；Range 续传与 esp_ota_resume（事实 F3）组合是三通道中唯一能做真断点续传的；esp-tls 可选（Kconfig），内网明文省内存 |
| ③ 自研 TCP 推流协议 | 缓做 | 内存最省（lwip socket 级）、协议最可控，但上位机需全新配套 + TLS 自理；等 ② 落地后如需产线批量推流再演进（sink 层不变，只换 transport） |

**方案 ② 设计要点**：
- 流程：`ota_wifi_start(url)` → STA 连接（STA 管理 ~120 行）→ `esp_http_client_open/read` 循环 → **Content-Length 必须存在**（服务器不给长度则拒绝：sink 的 size 校验要求已知总长）→ `ota_sink_write` → `finish` → `activate(reboot=false)` → 回 ACK/状态（预留上位机握手）→ `esp_restart`。
- **Range 断点续传（progress NVS 在 WiFi 场景的语义升级）**：
  - 回答任务问题："WiFi 场景还需要 progress NVS 持久化吗？"——**需要，且从"纯可观测"升级为"续传坐标"**。HTTP 可以 Range 跳偏移，但写侧必须知道"从哪个 flash 偏移继续"，这个坐标只能由 sink 落盘（事实 F3：`esp_ota_resume(partition, erase_size, image_offset, &handle)` 支持从 image_offset 恢复且不整擦）。
  - 流程：open 时 `cfg.resume=true` → sink 读 progress blob → offset>0 则 `esp_ota_resume` + HTTP 带 `Range: bytes=offset-` → 校验响应 206 且 `Content-Range` 起点一致 → 续写；服务器不支持 206 或校验失败 → 降级全新 `begin`（offset 归零）。
  - 可靠性边界：progress 仅在 esp_ota_write 成功后按 sector 对齐记录，续传只信任完整 sector 的 offset；镜像尾部不完整由 `esp_ota_end` 校验兜底（VALIDATE_FAIL → 自动降级全量重试一次）。这同时兑现审阅报告 P1-6 要求的"名实相符"：BLE=可观测、WiFi=真续传，文档分别声明。
- 安全：内网/实验室明文 HTTP（`CONFIG_OTA_WIFI_TLS=n`）；启用 TLS 时 `OTA_WIFI_TLS=y` + mbuf/缓冲降配（§6），生产外网必开。
- **开发量**：~420 行（wifi_sta.c ~130 + ota_wifi.c ~250 + Kconfig ~40）。**上位机配套**：~100 行 Python（`host/ota_wifi_host.py`：requests 流式 PUT/POST + Range 续传测试；或直接用任意静态 HTTP 服务器 + curl 断点验证，最低 0 行）。
- **与 sink 耦合点**：仅 `session_open(size, resume)/write/finish/activate` + epoch（HTTP 请求中止时 invalidate）。

### 5.3 USB——USB-Serial/JTAG 帧协议（推荐）｜候选与硬件事实

**硬件事实先纠偏**（事实 F1/F2，与任务输入中"GPIO18/19 vs 内置"的说法不同）：
- C6 **没有 USB-OTG 控制器**，只有 USB-Serial/JTAG（内置 PHY，固定 GPIO12=D-/GPIO13=D+；GPIO18/19 是 C3 的引脚）。因此：
  - 候选① **TinyUSB CDC 二次枚举：物理不可行，直接排除**（无 OTG 控制器可挂，`esp_tinyusb` 也不随 v6.0.2 分发）。"与 console 共存"的出路不是双枚举，而是帧协议与 console 文本流的路由策略（下述）。
  - 候选②③ 的载体唯一：**USB-Serial/JTAG，即当前烧录/console 同一个口**（usbipd 303a:1001）。

| 候选 | 结论 | 理由 |
|------|------|------|
| ① TinyUSB CDC | **否决（C6 无 OTG）** | 见上 |
| ② Ymodem/Xmodem 标准协议 | 备选 | 上位机零开发（pyserial/securecrt 原生）；但 Ymodem 二进制帧流与 console 日志同口混流必须静默期隔离，且 1KB 块无自定义元数据、错误恢复语义弱、与 BLE sector 心智不一致 |
| ③ 自定义帧协议（复用 ble_ota sector/ACK 设计） | **推荐** | sector 4096 + 帧序号 + CRC16 + 每 sector ACK：与 BLE 协议心智、progress NVS 字段、sink 的 sector 计数完全对齐；ACK 即天然流控（USJ FIFO 小，事实 F7）；上位机 pyserial ~250 行 |

**方案 ③ 协议骨架**（字节级协议在 PR-3 细化，此处定字段级契约）：
```
帧: [MAGIC 0xC6][CMD 1B][SEQ 2B][LEN 2B][PAYLOAD ≤512B][CRC16 2B]
CMD: ENTER(0x01) / READY(0x02) / START(size 4B, 0x03) / DATA(0x04) /
     SECTOR_ACK(0x05, payload=sector idx) / EOF(0x06) / RESULT(0x07) / ABORT(0x08)
会话边界: 设备收到 ENTER → 静默 console + epoch_invalidate + 回 READY → 才接受 START
```
- **console/烧录口冲突策略**（选型必答项）：
  - 与烧录不冲突：下载模式下 USJ 由 ROM 接管、app 不运行，帧协议自然失效；反之 OTA 会话期间不能烧录（Windows COM 口独占，天然互斥）。
  - 与 console 冲突：日志字节会污染帧流。解法 = **ENTER 门控 + 会话期静默**：收到 ENTER 后 `esp_log_level_set("*", ESP_LOG_NONE)` 静默（会话结束/失败恢复），且上位机解析器按 MAGIC+CRC 同步扫描、容忍前导垃圾字节。monitor 与 OTA 工具不可同时持有 COM 口（串口独占，互斥天然成立）。
- **开发量**：~380 行（ota_usb.c：帧收发状态机 ~200 + USJ 驱动接线 ~80 + 会话编排 ~100）。**上位机配套**：`host/ota_usb_host.py` ~250 行（pyserial，复用 ble_ota_host.py 的 sector/进度/重发骨架）。
- **与 sink 耦合点**：同 WiFi——open/write/finish/activate + ABORT 帧/ENTER 时 invalidate。

---

## 6. 内存预算（共存粗估，验收以真机 `esp_get_minimum_free_heap_size` 为准）

容量口径（事实 F6）：C6 HP SRAM 统一池 ≈ 441.5KB（IRAM+DRAM 共享，链接脚本实测值；任务提示中的"406KB"按此修正）。现状 BLE 固件真机启动后典型空闲堆约 230-280KB（粗估，PR-1 增加启动锚点日志实测固化）。

| 组合 | 增量占用（粗估） | 余量判断 |
|------|----------------|---------|
| BLE（现状基线） | NimBLE controller+host+mbuf ≈ 80-95KB | 已真机验证 |
| + USB 帧协议 | +3-5KB（USJ 驱动缓冲 + rx 任务栈） | **恒可行**，可长期共存编译 |
| + WiFi 明文（STA+lwip+http_client） | +90-120KB | 与 BLE 共存可行（coex 硬件支持，事实 F6），预估剩 60-120KB |
| + WiFi TLS（mbedTLS 默认 16KB×2 IO + 会话） | 再 +35-45KB | **峰值场景，紧**：必须真机实测；逃生舱 ①`CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN/OUT` 降 4KB（OTA 镜像无需大 TLS 帧）②OTA 会话期间 `esp_bt_controller_disable()`/停广播（Kconfig 策略项，牺牲 BLE 可用性）③ 一期若实测不过则 TLS 场景文档声明"WiFi OTA 与 BLE OTA 编译期二选一" |
| ota_sink 本体 | +1-2KB（状态机+mutex，无大缓冲） | 可忽略 |

结论：**默认交付组合 = BLE + USB 常驻共存（内存无忧），WiFi 作为 Kconfig 可选通道**（明文先行，TLS 降配后二期开）。三通道同时编译是一个明确的验收目标而非隐含假设。

---

## 7. 实施计划（3 个 PR，每个独立可用、可回归）

### PR-1：ota_sink 抽象 + BLE 桥接迁移 + P0-1 修复（地基，先行）
- **文件清单**：
  - 新增 `firmware/components/ota_core/include/ota_sink.h`、`src/ota_sink.c`（~420 行，含注释）
  - 新增 `firmware/main/ble_transport.c/.h`（~180 行，自 ota_task.c 改造：ringbuf/notify_sem/泵任务/STALE 重开）
  - 删除 `firmware/main/ota_task.c/.h`；改 `firmware/main/main.c`（接线 ~30 行 diff）、`firmware/main/CMakeLists.txt`
  - 补丁 `firmware/components/ble_ota/src/nimble_ota.c` + 头文件（~30 行，会话回调）
  - 新增 `firmware/components/ota_core/Kconfig`（BLE 开关 + progress 间隔）
  - 新增 host 侧 sink 状态机金标准单测 `firmware/components/ota_core/test/host_test_ota_sink.py`（~150 行）
  - （可选顺带）`host/ble_ota_host.py` P0-2 成功判定反转（~20 行，独立 commit）
- **验收标准**：
  1. host 单测 ALL PASS（BUSY/STALE/size 校验/sector 计数状态机）；
  2. 主工程构建绿 + 分层 grep 零违规（ota_core 无 NimBLE 头、notify_sem 契约符号仍在）；
  3. **BLE 真机全流程回归**：1.0.1→新版本升级成功、161 sectors、进度落盘、回滚确认锚点全命中（对齐 WORKSTATE 真机基线）；
  4. **P0-1 专项**（审阅报告 Top5 测试 1/3）：传输 1/3 处杀上位机 → 重连重试 → 设备日志出现 epoch 失效→abort→rebegin 链（无错位写入）、esp_ota_end 不再失败；空文件/超分区 size → Start 后立即 OTA_SINK_ERR_SIZE_INVALID 拒绝（P1-4/P1-7/P1-5/P2-9 一并覆盖）；
  5. 开机不再执行全擦（P1-7 验收：启动日志无 esp_ota_begin 调用，时延下降）。
- **预估**：固件 ~660 行 + host ~170 行。

### PR-2：WiFi 通道（esp_http_client 流式 + Range 续传）
- **文件清单**：新增 `firmware/components/ota_wifi/`（`ota_wifi.c` ~250、`wifi_sta.c` ~130、`include/ota_wifi.h` ~40、Kconfig ~40）；改 `firmware/main/main.c`（+15 行接线）、`sdkconfig.defaults`（WiFi/coex 配置段）；新增 `host/ota_wifi_host.py`（~100 行，或复用静态服务器 + curl 零上位机开发）。
- **验收标准**：
  1. `CONFIG_OTA_TRANSPORT_WIFI=y` 构建绿，BLE 仍可回归（组合编译）；
  2. 真机 HTTP 明文全量升级成功（对端用简单 HTTP 服务器）；
  3. **断点续传**：传输 1/3 处断网 → 恢复重试 → 日志命中 `esp_ota_resume + Range 206` 路径，无需全量重传完成升级（对 BLE 全量重传形成对照，兑现 P1-6 名实相符）；
  4. 服务器不支持 206 → 自动降级全量，升级仍成功；
  5. BLE 会话进行中发起 WiFi → 上位机收到 BUSY 语义；
  6. 共存内存实测：BLE+WiFi 明文同时使能，`esp_get_minimum_free_heap_size` 锚点日志记录在档（§6 预算修正依据）。
- **预估**：固件 ~475 行 + host ≤100 行。

### PR-3：USB 通道（USJ 帧协议 + 上位机工具）
- **文件清单**：新增 `firmware/components/ota_usb/`（`ota_usb.c` ~380：帧状态机/USJ 接线/会话编排 + Kconfig ~30）；改 `firmware/main/main.c`（+10 行）；新增 `host/ota_usb_host.py`（~250 行，pyserial）+ 帧协议文档段（并入 docs 层 issue #10 的 PROTOCOL.md）。
- **验收标准**：
  1. 组合编译（BLE+USB）构建绿 + BLE 回归通过；
  2. 真机 USB 口全量升级成功（Windows pyserial → usbipd COM 口，与当前烧录同一物理口）；
  3. ENTER 门控验收：ENTER 前日志可正常输出，ENTER 后静默、传输零污染、结束恢复；
  4. 中途 ABORT 帧 → 设备 epoch 失效 + abort + 回 IDLE，可立刻重新 START（P0-1 同机制跨通道验证）；
  5. monitor 持有 COM 口时 OTA 工具打开失败并给出明确提示（串口独占互斥）；
  6. 烧录模式（download mode）不受影响（OTA 会话外普通 idf.py flash 正常）。
- **预估**：固件 ~410 行 + host ~250 行。

> P0-1 修复归属：**PR-1**（会话代数机制是 ota_sink 抽象的组成部分，不是独立补丁）。PR-2/PR-3 的 transport 复用同一机制，天然继承。

---

## 8. 风险清单

### 从审阅报告继承（本设计的处置）
| 风险 | 处置 |
|------|------|
| P0-1 会话错位链 | PR-1 epoch 机制根除（§3.2），三通道共用 |
| P0-2 上位机假成功 | PR-1 可选 commit（Stop ACK 正常返回=FAIL 信号反转）；WiFi/USB 通道上位机从第一版就带设备侧 RESULT 语义 |
| P0-3 跳 sector 死代码 | 不修（协议预留）；文档明确"BLE 恢复策略=全量重传，WiFi=Range 续传"（P1-6 名实相符） |
| P1-5 选槽基准 | sink 选槽改用 `esp_ota_get_running_partition()` |
| P1-7 开机全擦 | sink begin 时机后移 + 已知 size 部分擦除 |
| P2-9/P2-11/P2-16 | sink begin 失败打印 esp_err_to_name；progress blob 补 CRC（PR-1 顺手，+15 行）；progress 间隔 Kconfig 化 |

### 新增风险
| # | 风险 | 概率/影响 | 缓解 |
|---|------|----------|------|
| R1 | USB 与烧录口/console 同口冲突（物理同一 USJ） | 低/中 | ENTER 门控+会话静默+COM 独占互斥（§5.3）；下载模式天然隔离 |
| R2 | BLE+WiFi(TLS) 共存内存不足（§6 峰值场景） | 中/中 | 一期 TLS 默认关；降配逃生舱 + 兜底"编译期二选一"声明；验收必测 minimum free heap |
| R3 | epoch 失效后重开全擦窗口（esp_ota_abort→begin 1-2s）期间对端已开始重发，ringbuf 堆积 | 中/低 | ringbuf 8KB 满即丢新数据由 ACK 缺失触发对端重发（BLE 现有机制自愈）；WiFi/USB 有明确流控，无此问题 |
| R4 | WiFi Range 续传的 offset 超前（progress 记录与 flash 实际写入窗口错位） | 低/中 | 仅按 sector 对齐记录 + 只信任完整 sector offset + end 校验兜底 + 降级全量重试一次（§5.2） |
| R5 | esp_ota_resume 为 IDF v6 新 API，示例少、组合行为（erase_size=0 + PENDING_VERIFY）未文档化 | 中/低 | PR-2 先行验证脚本探边界（含 PENDING_VERIFY 拒绝路径——由 rollback_check 先于通道 init 的启动顺序设计性消除，现状不变） |
| R6 | 三通道并发会话在开放期（如 USB 会话中 BLE Start 到达）被 BUSY 拒绝后，上位机协议无"忙"语义（BLE 协议仅 ACK 0x0001/0x0002） | 中/低 | 一期 BLE 协议不动（Start ACK 返回成功但 sink 拒绝会在数据阶段暴露 STALE→对端重试路径）；文档标注"BLE 通道忙语义为 v1.1 协议扩展候选" |
| R7 | progress NVS 写放大随通道数增长（P2-16 残留） | 低/低 | OTA_PROGRESS_INTERVAL_SECTORS 可配；WiFi/USB 默认 4 |
| R8 | `vSemaphoreDelete(notify_sem)` 竞态（P2-10）随迁移丢失修复机会 | 低/中 | PR-1 迁移 ble_transport 时一并改为"任务退出不删信号量，仅初始化一次"（生命周期归 transport） |

---

## 9. 决策留痕（ADR 摘要）

- **ADR-004-1 会话所有权收拢到 ota_sink**：备选=vendor 组件内修会话检测（审阅报告建议的"组件补丁直改 ota_task 检测"）；决策=sink 集中。理由：P0-1 根因是会话分裂，三通道统一后组件补丁退化为纯事件���报（25 行），WiFi/USB 零成本继承；代价=一次 ota_task.c 结构迁移（可回归覆盖）。
- **ADR-004-2 单写者=先到先得不抢占**：抢占需要会话优先级与中断恢复语义，故障面大于收益；场景上单设备双通道同时推固件不是真实需求。
- **ADR-004-3 WiFi 用 esp_http_client 流式而非 esp_https_ota**（事实 F5）：统一底层是需求本体；esp_https_ota 无注入口，fork 维护成本 > 自研 300 行。
- **ADR-004-4 USB 用 USJ 自定义帧协议而非 Ymodem/TinyUSB**（事实 F1/F2）：C6 无 OTG（TinyUSB 物理不可行）；自定义帧与 sink sector 语义/progress 字段/BLE 心智三方对齐，Ymodem 仅当"上位机零开发"权重最高时启用（备选保留）。
- **ADR-004-5 reboot 决策权下放 transport**：BLE 保持"完成即重启"现状；WiFi/USB 先回 RESULT 再重启——设备侧消除"重启瞬间无应答"（P0-2 的诱因之一），协议可观测性更好。
