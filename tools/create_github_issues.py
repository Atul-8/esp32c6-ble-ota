#!/usr/bin/env python3
"""create_github_issues.py — 批量创建 GitHub issues（带里程碑标签）"""
import json, sys, urllib.request, urllib.error, time

TOKEN = sys.argv[1]
OWNER, REPO = "Atul-8", "esp32c6-ble-ota"
API = f"https://api.github.com/repos/{OWNER}/{REPO}"

def api(method, path, payload=None):
    data = json.dumps(payload).encode() if payload else None
    req = urllib.request.Request(f"{API}{path}", data=data, method=method,
        headers={"Authorization": f"Bearer {TOKEN}",
                 "Accept": "application/vnd.github+json",
                 "Content-Type": "application/json; charset=utf-8"})
    try:
        return json.load(urllib.request.urlopen(req, timeout=30))
    except urllib.error.HTTPError as e:
        return {"_error": e.code, "_body": e.read().decode("utf-8","replace")[:200]}

# 1. 创建标签（label 需先存在才能打）
labels = [
    ("m1-firmware",   "0052cc", "M1 固件: ble_ota 集成/回滚/续传/CRC"),
    ("m2-host-tool",  "0e8a16", "M2 上位机: Bleak 扫描/传输/激活"),
    ("m3-e2e",        "d93f0b", "M3 全链路联调与回滚实测"),
    ("high",          "b60205", "优先级高"),
    ("medium",        "fbca04", "优先级中"),
    ("low",           "cccccc", "优先级低"),
]
existing = {l["name"] for l in api("GET", "/labels?per_page=100")}
for name, color, desc in labels:
    if name not in existing:
        r = api("POST", "/labels", {"name": name, "color": color, "description": desc})
        print(f"label {name}: {'ok' if 'name' in r else r}")

ISSUES = [
    ("[M1] 集成 espressif/ble_ota 官方组件，跑通基础 BLE OTA 收包写分区",
     """## 目标
- `main/idf_component.yml` 声明 `espressif/ble_ota: ^0.1.17`
- 分区表切换双 OTA 槽（ota_0 + ota_1 + otadata，各 0x1C0000）
- sdkconfig：NimBLE + MTU 517 + 回滚使能
- 以官方 example app_main 为骨架接线，广播名 `C6-OTA-xxxx`

## 验收
- [ ] WSL 构建零错误零分区溢出告警
- [ ] 烧录成功，手动 RST 后广播名可见
- [ ] 分层合规 grep：ota_core 无 ESP_LOG/printf

**关联**: REQ-002, 里程碑 M1""",
     ["m1-firmware", "high"]),

    ("[M1] 版本回滚：PENDING_VERIFY 自检 + mark valid + 自动回滚",
     """## 目标
- `components/ota_core/ota_rollback.c`：自检（NVS 读写 + 堆内存阈值）
- 启动时 PENDING_VERIFY → 自检过 `esp_ota_mark_app_valid_cancel_rollback()`；不过走 `esp_ota_mark_app_invalid_rollback_and_reboot()`
- 日志锚点 `[BLE_OTA] rollback check: <state> -> <confirmed|would-rollback>`

## 验收
- [ ] 新 app 首启打印 PENDING_VERIFY → confirmed
- [ ] 回滚实测：自检置 false，刷新版后自动回滚旧分区
- [ ] PENDING_VERIFY 状态下 OTA begin 被正确拒绝（先 mark valid 再开 OTA）

**关联**: REQ-002, 里程碑 M1""",
     ["m1-firmware", "high"]),

    ("[M1] 断点续传：NVS 进度持久化（4KB 粒度）+ 重连续传",
     """## 目标
- `components/ota_core/ota_progress_store.c`：NVS 存 {image_size, offset, crc32}
- ble_ota 接收事件每 4KB 刷一次进度
- 重连（不重启）后由 ble_ota ACK 期望 Sector_Index 机制自然续传
- 重启后启动日志打印 `[OTA_RESUME] offset=xxx/size=yyy`（读 ble_ota 组件源码确认跨重启恢复能力并记录结论）

## 验收
- [ ] 传输中断（断蓝牙）→ 重连 → 从上次 sector 续传
- [ ] NVS 进度与实际写入一致性抽查

**关联**: REQ-002, 里程碑 M1""",
     ["m1-firmware", "high"]),

    ("[M1] CRC 校验验证：sector CRC16 + 命令包 CRC16 生效",
     """## 目标
验证官方协议两层 CRC 生效：
- 构造坏 sector（上位机翻转一字节）→ 设备回 ACK status=0x0001 并重传该 sector
- 构造坏命令包 → 设备拒绝

## 验收
- [ ] 坏数据被拒且传输自动恢复
- [ ] 正常传输多轮成功率 100%

**关联**: REQ-002, 里程碑 M1""",
     ["m1-firmware", "medium"]),

    ("[M2] Windows 上位机：Bleak 扫描/连接/MTU 协商",
     """## 目标
- `host/ble_ota_host.py`：扫描 `C6-OTA-*`、连接、请求 MTU 517、发现 OTA 服务（4 特征）
- 订阅 RECV_FW/COMMAND Notify

## 验收
- [ ] `--scan` 稳定发现设备
- [ ] 连接 + 服务发现成功，打印特征表

**关联**: REQ-002, 里程碑 M2""",
     ["m2-host-tool", "high"]),

    ("[M2] 上位机传输引擎：sector 分包 + CRC16 尾包 + ACK 驱动节流",
     """## 目标
- 本地 .bin 按 4KB sector 切分，sector 内按 (MTU-4) 分包，Packet_Seq 递增，尾包 0xFF + sector CRC16
- ACK 驱动：当前 sector ACK(0x0000) 才发下一 sector；ACK 0x0002 用期望 sector 号跳转（续传）；ACK 0x0001 重传当前 sector
- Start(0x0001 带 size) → 数据 → End/Apply 流程
- 进度条 + 断线自动重连续传

## 验收
- [ ] 对开发板完整传输一版固件并激活成功
- [ ] 人为断线后续传成功

**关联**: REQ-002, 里程碑 M2""",
     ["m2-host-tool", "high"]),

    ("[M2] 上位机校验与激活：CRC 汇总 + Apply + 重启确认新版本",
     """## 目标
- 传输完读回进度/状态（PROGRESS char），确认全 sector ACK
- 发 Apply → 等 status=0 → 设备重启（或手动 RST，见 ERR-003）
- 重连后读版本号确认升级成功

## 验收
- [ ] 一键 OTA：扫描→连接→传输→激活→确认版本闭环
- [ ] 输出升级报告（耗时/速率/重传数）

**关联**: REQ-002, 里程碑 M2""",
     ["m2-host-tool", "medium"]),

    ("[M3] 全链路联调：真机 BLE OTA 升级 + 多轮稳定性",
     """## 目标
- v1.0.0（USB 首烧）→ OTA 升级 v1.0.1 → 再升级 v1.0.2（版本号打印区分）
- 每轮记录：传输耗时、速率、重传
- 覆盖：正常升级、断线续传、坏包重传

## 验收
- [ ] 连续 3 轮 OTA 成功
- [ ] 升级后功能自检通过（回滚确认逻辑）

**关联**: REQ-002, 里程碑 M3""",
     ["m3-e2e", "high"]),

    ("[M3] 回滚实战测试：刷坏固件验证自动回滚",
     """## 目标
- 构造 v2.0.0-broken（自检必失败版本）通过 OTA 刷入
- 观察设备 PENDING_VERIFY → 自动回滚旧版本
- 确认旧分区版本恢复运行 + BLE 可再次升级

## 验收
- [ ] 回滚自动发生且设备恢复可用
- [ ] 回滚后仍能再次正常 OTA

**关联**: REQ-002, 里程碑 M3""",
     ["m3-e2e", "high"]),

    ("[M3] 文档收尾：使用手册 + 协议文档 + 已知问题",
     """## 目标
- README 补全：上位机用法示例输出、FAQ
- 协议帧格式文档（docs/PROTOCOL.md）
- 已知问题清单（usbipd 复位 bug ERR-003 等）

## 验收
- [ ] 新人按文档可复现全流程

**关联**: REQ-002, 里程碑 M3""",
     ["m3-e2e", "low"]),
]

ok = 0
for title, body, issue_labels in ISSUES:
    r = api("POST", "/issues", {"title": title, "body": body, "labels": issue_labels})
    if "number" in r:
        print(f"#{r['number']} created: {title[:48]}")
        ok += 1
    else:
        print(f"FAIL: {title[:40]} -> {r}")
    time.sleep(0.3)

print(f"\nDone: {ok}/{len(ISSUES)}")
