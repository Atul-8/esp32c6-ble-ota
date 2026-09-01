#!/usr/bin/env python3
"""create_issues.py — 批量创建 Gitee issues（META-052: JSON body + charset=utf-8）"""
import json, sys, urllib.request, time

TOKEN = sys.argv[1]
OWNER = "waterguy"
REPO = "esp32c6-ble-ota"
URL = f"https://gitee.com/api/v5/repos/{OWNER}/{REPO}/issues"

ISSUES = [
    # ---- M1 固件 ----
    ("[M1] 集成 espressif/ble_ota 官方组件，跑通基础 BLE OTA 收包写分区",
     "## 目标\n\n- `main/idf_component.yml` 声明 `espressif/ble_ota: ^0.1.17`\n- 分区表切换双 OTA 槽（ota_0 + ota_1 + otadata，各 0x1C0000）\n- sdkconfig：NimBLE + MTU 517 + 回滚使能\n- 以官方 example app_main 为骨架接线，广播名 `C6-OTA-xxxx`\n\n## 验收\n\n- [ ] WSL 构建零错���零分区溢出告警\n- [ ] 烧录成功，手动 RST 后广播名可见\n- [ ] 分层合规 grep：ota_core 无 ESP_LOG/printf\n\n**关联**: REQ-002, 里程碑 M1", "固件", "高"),
    ("[M1] 版本回滚：PENDING_VERIFY 自检 + mark valid + 自动回滚",
     "## 目标\n\n- `components/ota_core/ota_rollback.c`：自检（NVS 读写 + 堆内存阈值）\n- 启动时 PENDING_VERIFY → 自检过 `esp_ota_mark_app_valid_cancel_rollback()`；自检不过走 `esp_ota_mark_app_invalid_rollback_and_reboot()`\n- 日志锚点 `[BLE_OTA] rollback check: <state> -> <confirmed|would-rollback>`\n\n## 验收\n\n- [ ] 新 app 首启打印 PENDING_VERIFY → confirmed\n- [ ] 验证回滚：临时把自检改为 false，刷新版后应自动回滚到旧分区\n- [ ] PENDING_VERIFY 状态下 OTA begin 被正确拒绝（先 mark valid 再开 OTA）\n\n**关联**: REQ-002, 里程碑 M1", "固件", "高"),
    ("[M1] 断点续传：NVS 进度持久化（4KB 粒度）+ 重连续传",
     "## 目标\n\n- `components/ota_core/ota_progress_store.c`：NVS 存 {image_size, offset, crc32}\n- ble_ota 接收事件每 4KB 刷一次进度\n- 重连（不重启）后由 ble_ota ACK 期望 Sector_Index 机制自然续传\n- 重启后启动日志打印 `[OTA_RESUME] offset=xxx/size=yyy`（读 ble_ota 组件源码确认跨重启恢复能力并记录结论）\n\n## 验收\n\n- [ ] 传输中断（拔蓝牙）→ 重连 → 从上次 sector 续传\n- [ ] NVS 进度与实际写入一致性抽查\n\n**关联**: REQ-002, 里程碑 M1", "固件", "高"),
    ("[M1] CRC 校验验证：sector CRC16 + 命令包 CRC16 生效",
     "## 目标\n\n验证官方协议两层 CRC 都生效：\n- 构造坏 sector（上位机翻转一字节）→ 设备回 ACK status=0x0001 并重传该 sector\n- 构造坏命令包 → 设备拒绝\n\n## 验收\n\n- [ ] 坏数据被拒且传输恢复\n- [ ] 正常传输成功率 100%（多轮）\n\n**关联**: REQ-002, 里程碑 M1", "固件", "中"),
    # ---- M2 上位机 ----
    ("[M2] Windows 上位机：Bleak 扫描/连接/MTU 协商",
     "## 目标\n\n- `host/ble_ota_host.py`：扫描 `C6-OTA-*`、连接、请求 MTU 517、发现 OTA 服务（4 特征）\n- 订阅 RECV_FW/COMMAND Notify\n\n## 验收\n\n- [ ] `--scan` 稳定发现设备\n- [ ] 连接 + 服务发现成功，打印特征表\n\n**关联**: REQ-002, 里程碑 M2", "上位机", "高"),
    ("[M2] 上位机传输引擎：sector 分包 + CRC16 尾包 + ACK 驱动节流",
     "## 目标\n\n- 本地 .bin 按 4KB sector 切分，sector 内按 (MTU-4) 分包，Packet_Seq 递增，尾包 0xFF + sector CRC16\n- ACK 驱动：收到当前 sector ACK(0x0000) 才发下一 sector；ACK 0x0002 用期望 sector 号跳转（续传）；ACK 0x0001 重传当前 sector\n- Start(0x0001 带 size) → 数据 → End/Apply 流程\n- 进度条 + 断线自动重连续传\n\n## 验收\n\n- [ ] 对开发板完整传输一版固件并激活成功\n- [ ] 人为断线后续传成功\n\n**关联**: REQ-002, 里程碑 M2", "上位机", "高"),
    ("[M2] 上位机校验与激活：CRC 汇总 + CMD_APPLY + 重启确认新版本",
     "## 目标\n\n- 传输完读回进度/状态（PROGRESS char），确认全 sector ACK\n- 发 Apply → 等 status=0 → 触发设备重启（或让用户手动 RST，见 ERR-003）\n- 重连后读版本号确认升级成功\n\n## 验收\n\n- [ ] 一键 OTA：扫描→连接→传输→激活→确认版本闭环\n- [ ] 输出升级报告（耗时/速率/重传数）\n\n**关联**: REQ-002, 里程碑 M2", "上位机", "中"),
    # ---- M3 联调 ----
    ("[M3] 全链路联调：真机 BLE OTA 升级 + 多轮稳定性",
     "## 目标\n\n- v1.0.0（USB 首烧）→ OTA 升级到 v1.0.1（版本号打印区分）→ 再升级 v1.0.2\n- 每轮记录：传输耗时、速率、重传\n- 覆盖：正常升级、断线续传、坏包重传\n\n## 验收\n\n- [ ] 连续 3 轮 OTA 成功\n- [ ] 升级后功能自检通过（回滚确认逻辑）\n\n**关联**: REQ-002, 里程碑 M3", "联调", "高"),
    ("[M3] 回滚实战测试：刷坏固件验证自动回滚",
     "## 目标\n\n- 构造 v2.0.0-broken（自检必失败版本）通过 OTA 刷入\n- 观察设备 PENDING_VERIFY → 自动回滚到旧版本\n- 确认旧分区版本恢复运行 + BLE 可再次升级\n\n## 验收\n\n- [ ] 回滚自动发生且设备恢复可用\n- [ ] 回滚后仍能再次正常 OTA\n\n**关联**: REQ-002, 里程碑 M3", "联调", "高"),
    ("[M3] 文档收尾：使用手册 + 协议文档 + 已知问题",
     "## 目标\n\n- README 补全：上位机用法截图/示例输出、FAQ\n- 协议帧格式文档（docs/PROTOCOL.md）\n- 已知问题清单（usbipd 复位 bug ERR-003 等）\n\n## 验收\n\n- [ ] 新人按文档可复现全流程\n\n**关联**: REQ-002, 里程碑 M3", "文档", "低"),
]

ok, fail = 0, 0
for idx, (title, body, label, priority) in enumerate(ISSUES, 1):
    payload = json.dumps({
        "access_token": TOKEN,
        "repo": REPO,
        "title": title,
        "body": body,
        "labels": [label, priority],
    }, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(URL, data=payload,
        headers={"Content-Type": "application/json; charset=utf-8"}, method="POST")
    try:
        r = urllib.request.urlopen(req, timeout=30)
        d = json.load(r)
        print(f"#{d.get('number')} created: {title[:50]}")
        ok += 1
    except urllib.error.HTTPError as e:
        print(f"FAIL [{title[:40]}]: HTTP {e.code} {e.read().decode('utf-8','replace')[:150]}")
        fail += 1
    time.sleep(0.4)

print(f"\nDone: {ok} created, {fail} failed")
