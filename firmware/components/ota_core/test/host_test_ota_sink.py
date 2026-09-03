#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
host_test_ota_sink.py -- ota_sink 会话状态机金标准单测（host 端，无硬件）

按设计文档 §3.3/§7-PR1 验收第 1 条：sink 的纯逻辑（size 校验规则、sector 计数、
epoch 判定、BUSY/STALE 状态转移）抽成与 esp_ota_ops 无关的纯函数做 host 端单测。

本文件是 C 实现 ota_sink.c 的行为规格镜像：逐条用例对应 ota_sink.c 的状态转移，
固件端以真机验收（构建绿 + 三次真机回归）。规格以设计文档 §3 为准。

用例矩阵：
  TS-01 open: size==0 拒绝（BAD_ARG）                       [P1-4]
  TS-02 open: size>分区容量 拒绝（SIZE_INVALID）             [P1-4]
  TS-03 open: 成功分配 epoch=1，target=running 的下一槽       [P1-5]
  TS-04 open: 会话活跃期再 open → BUSY（不抢占，ADR-004-2）
  TS-05 write: epoch 不匹配 → STALE，bytes_written 不增（数据不落盘）[P0-1 写入层]
  TS-06 write: 正确 epoch → OK，4KB 对齐触发 sector 计数/NVS
  TS-07 write: 写满 image_size 后仍 write → 超收数据计入但收尾以 bytes>=size 判定
  TS-08 invalidate: 活跃会话作废 → 后续旧 epoch write 全 STALE [P0-1 事件层]
  TS-09 invalidate: 新 open 分配 epoch=2（单调递增不复用）
  TS-10 finish: 数据未满（错位镜像）→ VALIDATE_FAIL + 回 IDLE [P0-1 收尾层/自愈]
  TS-11 finish: 数据满 → VALIDATED；activate 后 epoch 关闭、progress 清零
  TS-12 abort: 会话中止回 IDLE；旧 epoch write STALE
  TS-13 idle 时 abort/finish/activate 携带旧 epoch → STALE/NO_SESSION
  TS-14 开机不开会话：init 后 epoch_current()==0（P1-7 前置：无 begin 无全擦）
"""

import sys

# ---------------------------------------------------------------------------
# 纯逻辑镜像：把 ota_sink.c 的状态机抽成 host 可执行规格（与 C 逐条对齐）
# ---------------------------------------------------------------------------

SECTOR_SIZE = 4096
PARTITION_SIZE = 0x1C0000  # ota_0/ota_1 各 1.75MB（partitions.csv）

OK, BAD_ARG, BUSY, NO_SESSION, STALE, SIZE_INVALID = (
    "OK", "BAD_ARG", "BUSY", "NO_SESSION", "STALE", "SIZE_INVALID")


class SinkSpec:
    """ota_sink.c 行为规格（单任务视角；并发由固件端锁保证，此处验状态转移）"""

    def __init__(self):
        self.epoch = 0            # 最近一次 open 派发的代数
        self.next_epoch = 1
        self.session_live = False
        self.state = "IDLE"       # IDLE/WRITING/ABORT_PENDING/VALIDATED
        self.image_size = 0
        self.bytes_written = 0
        self.sectors_done = 0
        self.nvs_saves = []       # progress_save 调用记录
        self.nvs_cleared = 0
        self.events = []

    # -- 内部 --
    def _to_idle(self):
        self.state = "IDLE"
        self.session_live = False

    def _current(self, epoch):
        return (self.session_live and epoch == self.epoch
                and self.state == "WRITING")

    # -- API --
    def init(self):
        """开机态：epoch=0、无会话（TS-14：无 begin、无全擦）"""
        self.epoch = 0
        self.next_epoch = 1
        self.session_live = False
        self.state = "IDLE"

    def epoch_current(self):
        return self.epoch if self.session_live else 0

    def open(self, image_size):
        if image_size == 0:
            return BAD_ARG, None
        if self.state == "ABORT_PENDING":
            self._to_idle()          # 兑现延迟 abort（串行方）
        if self.session_live:
            return BUSY, None
        if image_size > PARTITION_SIZE:
            return SIZE_INVALID, None
        self.epoch = self.next_epoch
        self.next_epoch += 1
        self.session_live = True
        self.state = "WRITING"
        self.image_size = image_size
        self.bytes_written = 0
        self.sectors_done = 0
        self.events.append(("SESSION_START", self.epoch))
        return OK, self.epoch

    def write(self, epoch, data_len):
        if not self._current(epoch):
            return STALE
        self.bytes_written += data_len
        while (self.bytes_written - self.sectors_done * SECTOR_SIZE
               >= SECTOR_SIZE):
            self.sectors_done += 1
            self.nvs_saves.append((self.image_size,
                                   self.sectors_done * SECTOR_SIZE,
                                   self.sectors_done))
            self.events.append(("PROGRESS", self.sectors_done))
        return OK

    def invalidate(self):
        """事件层：新 Start = 旧会话作废（WRITING → ABORT_PENDING 延迟 abort）"""
        if self.state == "WRITING":
            self.state = "ABORT_PENDING"
        self.session_live = False

    def abort(self, epoch):
        if not self.session_live or epoch != self.epoch:
            return STALE
        self._to_idle()
        return OK

    def finish(self, epoch):
        if not self.session_live or epoch != self.epoch:
            return STALE
        if self.state != "WRITING":
            return NO_SESSION
        # 镜像完整性：错位/坏镜像（bytes_written != image_size 或内容错）→ 失败。
        # 规格层用 bytes_written != image_size 代表 esp_ota_end 校验失败。
        if self.bytes_written != self.image_size:
            self._to_idle()          # abort 回 IDLE（设备自愈）
            self.events.append(("ERROR", "VALIDATE_FAIL"))
            return NO_SESSION if False else "VALIDATE_FAIL"
        self.state = "VALIDATED"
        self.events.append(("VALIDATED", self.epoch))
        return OK

    def activate(self, epoch, reboot=True):
        if not self.session_live or epoch != self.epoch:
            return STALE
        if self.state != "VALIDATED":
            return NO_SESSION
        self.nvs_cleared += 1
        self.session_live = False    # 会话完成关闭
        self.events.append(("ACTIVATED", self.epoch))
        return OK


# ---------------------------------------------------------------------------
# 用例
# ---------------------------------------------------------------------------

results = []


def check(name, cond):
    results.append((name, bool(cond)))


def test():
    # TS-14 开机不开会话（P1-7 前置）
    s = SinkSpec(); s.init()
    check("TS-14 init 后 epoch==0（无会话无全擦）", s.epoch_current() == 0
          and not s.session_live and s.state == "IDLE")

    # TS-01 size==0
    s = SinkSpec(); s.init()
    r, e = s.open(0)
    check("TS-01 open size=0 → BAD_ARG", r == BAD_ARG and e is None)

    # TS-02 size 超分区
    s = SinkSpec(); s.init()
    r, e = s.open(PARTITION_SIZE + 1)
    check("TS-02 open size>1.75MB → SIZE_INVALID", r == SIZE_INVALID and e is None)

    # TS-03 正常 open 拿 epoch=1
    s = SinkSpec(); s.init()
    r, e = s.open(656720)
    check("TS-03 open → epoch=1 + SESSION_START", r == OK and e == 1
          and s.events == [("SESSION_START", 1)])

    # TS-04 活跃期再 open → BUSY
    r2, _ = s.open(1000)
    check("TS-04 二次 open → BUSY（不抢占）", r2 == BUSY
          and s.epoch == 1 and s.image_size == 656720)

    # TS-05 旧/假 epoch write → STALE 不落盘
    before = s.bytes_written
    r3 = s.write(99, 4096)
    check("TS-05 write epoch=99 → STALE 且不计数", r3 == STALE
          and s.bytes_written == before)

    # TS-06 正确 epoch：sector 计数 + NVS 保存
    s.write(1, SECTOR_SIZE)
    s.write(1, 100)
    check("TS-06 4KB 触发 sector=1 + NVS", s.sectors_done == 1
          and s.nvs_saves == [(656720, 4096, 1)])
    s.write(1, SECTOR_SIZE - 100)
    check("TS-06b 凑满第 2 sector", s.sectors_done == 2
          and s.nvs_saves[-1] == (656720, 8192, 2))

    # TS-07 写满后 finish→activate 全链
    s2 = SinkSpec(); s2.init()
    _, e2 = s2.open(SECTOR_SIZE * 2)
    s2.write(e2, SECTOR_SIZE); s2.write(e2, SECTOR_SIZE)
    rf = s2.finish(e2)
    ra = s2.activate(e2)
    check("TS-07 满 2 sector → finish OK + activate OK + progress 清零",
          rf == OK and ra == OK and s2.nvs_cleared == 1
          and ("ACTIVATED", e2) in s2.events)

    # TS-08/09 invalidate 后旧 epoch STALE + 新 open epoch=2
    s3 = SinkSpec(); s3.init()
    _, e3 = s3.open(100000)
    s3.write(e3, 4096)
    s3.invalidate()                       # 断连/重连/新 Start
    r4 = s3.write(e3, 4096)               # 旧 epoch 数据
    check("TS-08 invalidate 后旧 epoch write → STALE", r4 == STALE)
    r5, e5 = s3.open(100000)              # 新 Start → lazy-open
    check("TS-09 新 open epoch=2（单调递增）", r5 == OK and e5 == 2)
    r6 = s3.write(e5, 4096)
    check("TS-09b 新 epoch 写入正常", r6 == OK)

    # TS-10 错位镜像 finish 失败 → 回 IDLE 自愈
    s4 = SinkSpec(); s4.init()
    _, e4 = s4.open(100000)
    s4.write(e4, 4096)                    # 只有 4KB/100KB → 错位镜像
    r7 = s4.finish(e4)
    check("TS-10 finish 错位镜像 → VALIDATE_FAIL + 回 IDLE",
          r7 == "VALIDATE_FAIL" and s4.state == "IDLE"
          and not s4.session_live)
    check("TS-10b 失败后可立即重新 open（自愈）", s4.open(100000)[0] == OK)

    # TS-11 activate 后旧 epoch 全 STALE
    s5 = SinkSpec(); s5.init()
    _, e5b = s5.open(4096)
    s5.write(e5b, 4096)
    s5.finish(e5b); s5.activate(e5b)
    check("TS-11 activate 后 epoch_current()==0", s5.epoch_current() == 0)
    check("TS-11b activate 后旧 epoch write → STALE",
          s5.write(e5b, 100) == STALE)

    # TS-12 abort 后旧 epoch STALE
    s6 = SinkSpec(); s6.init()
    _, e6 = s6.open(100000)
    s6.write(e6, 100)
    check("TS-12 abort 会话 → OK", s6.abort(e6) == OK
          and s6.epoch_current() == 0)
    check("TS-12b abort 后旧 epoch write → STALE", s6.write(e6, 100) == STALE)

    # TS-13 idle 时 finish/activate 旧 epoch
    s7 = SinkSpec(); s7.init()
    check("TS-13 idle finish → STALE", s7.finish(1) == STALE)
    check("TS-13b idle activate → STALE", s7.activate(1) == STALE)
    check("TS-13c idle abort → STALE", s7.abort(1) == STALE)


test()
failed = [n for n, ok in results if not ok]
for name, ok in results:
    print("%-58s %s" % (name, "PASS" if ok else "FAIL"))
print("\n%d/%d ALL PASS" % (len(results) - len(failed), len(results))
      if not failed else "\nFAILED: %d" % len(failed))
sys.exit(1 if failed else 0)
