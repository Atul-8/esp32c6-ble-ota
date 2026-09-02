#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ble_ota_gui.py -- tkinter visualization test bench for ESP32-C6 BLE OTA.

Reuses the verified protocol core from ble_ota_host.py (BleakOtaClient +
scan_devices + crc16_ccitt) -- no protocol logic duplicated here.

Threading model (tkinter + asyncio coexistence):
  * one background thread owns an asyncio loop (runs scan / connect /
    transfer coroutines);
  * GUI thread never blocks: worker -> GUI via queue.Queue polled by
    root.after(60, ...); GUI -> worker via loop.call_soon_threadsafe;
  * all tkinter calls happen on the main thread only.

UI layout (dark engineering theme, 1180x760):
  +--------------------------------------------------------------+
  | top toolbar: [Scan] [Refresh] | device Listbox | firmware...  |
  +-------------------------+------------------------------------+
  | sector heatmap (Canvas) |  progress: bar + % + bytes + rate   |
  | 18-col grid, hover tip  |  stats: time/avg/retries/jumps/...  |
  +-------------------------+------------------------------------+
  | event log (readonly, timestamped, verbose toggle)            |
  +--------------------------------------------------------------+
  | result banner: SUCCESS / FAILED / IDLE + big buttons          |
  +--------------------------------------------------------------+

Python 3.8+, tkinter, bleak. Windows-first.
"""

import asyncio
import os
import queue
import struct
import sys
import threading
import time
import tkinter as tk
from tkinter import filedialog, font as tkfont, messagebox, ttk

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ble_ota_host as host  # noqa: E402  (protocol core, single source)

try:
    from bleak.exc import BleakError
except ImportError:
    print("[E] bleak is not installed. Run: pip install bleak")
    sys.exit(2)

APP_TITLE = "ESP32-C6 BLE OTA 测试台"
GEOMETRY = "1180x760"
SECTOR_COLUMNS = 18
CELL = 22           # heatmap cell size (px)
CELL_GAP = 3
POLL_MS = 60        # GUI queue poll period
RATE_WINDOW = 3.0   # sliding rate window (s)

# ---------------------------------------------------------------------------
# dark theme palette
# ---------------------------------------------------------------------------

C_BG = "#14171c"        # window background
C_PANEL = "#1c2128"     # panel background
C_PANEL2 = "#222933"    # panel alt / entries
C_FG = "#d8dee6"        # normal text
C_DIM = "#8b98a5"       # dim text
C_ACCENT = "#4fc3f7"    # accent blue
C_OK = "#66bb6a"        # green
C_WARN = "#ffa726"      # orange
C_ERR = "#ef5350"       # red
C_RUN = "#42a5f5"       # active/running blue
C_IDLE = "#2a323d"      # empty cell dark gray

CELL_COLORS = {
    "idle": C_IDLE,          # not sent
    "sent": "#1565c0",       # sent, waiting ack (pulsing blue)
    "ok": C_OK,              # ACKed success
    "jump": C_WARN,          # ACK 0x0002 -> device redirected
    "retry": C_ERR,          # ack timeout, resend
}


# ---------------------------------------------------------------------------
# worker: asyncio thread + OTA session
# ---------------------------------------------------------------------------

class OtaWorker:
    """Owns the asyncio loop thread and bridges protocol events to a queue.

    Protocol events are posted as tuples (kind, payload-dict); GUI polling
    consumes them. All BLE I/O stays on the loop thread.
    """

    def __init__(self, out_q: "queue.Queue") -> None:
        self.out_q = out_q
        self.loop: asyncio.AbstractEventLoop = None
        self._thread = threading.Thread(target=self._run, daemon=True,
                                        name="ble-ota-loop")
        self._thread.start()
        self._ota: host.BleakOtaClient = None
        self._cancel = threading.Event()

    # -- loop thread --------------------------------------------------------

    def _run(self) -> None:
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.loop.run_forever()
        self.loop.close()

    def stop(self) -> None:
        def _shutdown():
            for t in asyncio.all_tasks(self.loop):
                t.cancel()
            self.loop.stop()
        try:
            self.loop.call_soon_threadsafe(_shutdown)
        except RuntimeError:
            pass  # loop already dead

    # -- GUI -> worker (thread-safe posts) -----------------------------------

    def post(self, coro_factory) -> None:
        """Schedule a coroutine on the loop; coro_factory(worker) is awaited."""
        self.loop.call_soon_threadsafe(self._spawn, coro_factory)

    def request_cancel(self) -> None:
        """Cooperative cancel: checked at sector boundaries / connect retry.

        Does not interrupt a blocking send_sector() await -- the in-flight
        sector finishes (or ACK-timeouts) first, then the loop aborts.
        """
        self._cancel.set()

    def _spawn(self, coro_factory) -> None:
        self.loop.create_task(self._run_task(coro_factory))

    async def _run_task(self, coro_factory) -> None:
        try:
            await coro_factory(self)
        except (asyncio.CancelledError):
            self._push("cancelled", {})
        except Exception as exc:  # noqa: BLE001  last-ditch guard
            self._push("error", {"message": "%s: %s"
                                 % (type(exc).__name__, exc)})

    def _push(self, kind: str, payload: dict) -> None:
        self.out_q.put((time.monotonic(), kind, payload))

    # -- event bridge --------------------------------------------------------

    def make_event_cb(self):
        """Build the BleakOtaClient event hook -> queue bridge."""
        def cb(event: str, data: dict) -> None:
            self.out_q.put((time.monotonic(), "ev:" + event, dict(data)))
        return cb

    # -- high-level operations (coroutines, run on loop) ---------------------

    async def op_scan(self, timeout: float = 6.0) -> None:
        self._push("scan_start", {"timeout": timeout})
        try:
            rows = await host.scan_devices(timeout)
            self._push("scan_done", {"devices": rows})
        except Exception as exc:  # noqa: BLE001
            self._push("scan_done", {"devices": [], "error": str(exc)})

    async def op_flash(self, fw_path: str, mac: str, dry_run: bool) -> None:
        """Full OTA (or handshake-only). Cooperative cancel via check."""
        with open(fw_path, "rb") as fh:
            fw = fh.read()
        total_sectors = (len(fw) + host.SECTOR_SIZE - 1) // host.SECTOR_SIZE
        self._push("fw_loaded", {"size": len(fw), "sectors": total_sectors,
                                 "path": fw_path})
        self._push("phase", {"name": "connect"})
        self._ota = host.BleakOtaClient(
            verbose=False, event_cb=self.make_event_cb())
        ota = self._ota
        reconnect_budget = 1
        while True:
            if self._cancel.is_set():
                self._push("cancelled", {})
                return
            try:
                await ota.connect(mac)
                break
            except (BleakError, asyncio.TimeoutError, OSError) as exc:
                reconnect_budget -= 1
                ota.stats["reconnects"] += 1
                if reconnect_budget < 0:
                    self._push("fail", {"reason": "连接失败: %s" % exc,
                                        "stats": dict(ota.stats)})
                    return
                self._push("log", {"text": "连接失败（%s），2s 后重试" % exc})
                await asyncio.sleep(2.0)

        try:
            self._push("phase", {"name": "handshake"})
            self._push("mtu", {"mtu": ota.mtu_size})
            await ota.send_start(len(fw))
            self._push("start_ok", {})

            if dry_run:
                await ota.send_stop()
                self._push("done", {"ok": True, "dry": True,
                                    "stats": dict(ota.stats)})
                return

            # ---- data transfer loop ------------------------------------
            self._push("phase", {"name": "transfer"})
            current = ota.last_acked_sector + 1
            done_bytes = current * host.SECTOR_SIZE
            t_start = time.monotonic()
            samples = [(t_start, done_bytes)]  # sliding rate window
            while current < total_sectors:
                if self._cancel.is_set():
                    await self._abort(ota)
                    return
                self._push("sector_current", {"sector": current})
                try:
                    confirmed = await ota.send_sector(current, fw)
                except host.AckTimeout as exc:
                    self._push("fail", {"reason": str(exc),
                                        "stats": dict(ota.stats)})
                    return
                done_bytes = min((confirmed + 1) * host.SECTOR_SIZE, len(fw))
                current = confirmed + 1
                now = time.monotonic()
                samples.append((now, done_bytes))
                while samples and now - samples[0][0] > RATE_WINDOW:
                    samples.pop(0)
                span = now - samples[0][0]
                rate = ((done_bytes - samples[0][1]) / 1024.0 / span
                        if span > 0.2 else 0.0)
                eta = (total_sectors * host.SECTOR_SIZE - done_bytes) / 1024.0 \
                    / rate if rate > 0.01 else None
                self._push("progress", {
                    "done_bytes": done_bytes, "total": len(fw),
                    "sector": current, "sectors": total_sectors,
                    "rate": rate, "eta": eta, "elapsed": now - t_start,
                    "stats": dict(ota.stats)})
            # ---- finish -------------------------------------------------
            self._push("phase", {"name": "stop"})
            await ota.send_stop()
            stats = dict(ota.stats)
            self._push("done", {"ok": True, "dry": False,
                                "elapsed": time.monotonic() - t_start,
                                "stats": stats})
            # P1: poll for the device to come back after reboot
            await self._poll_reboot(fw_path, mac)
        finally:
            await ota.disconnect()

    async def _abort(self, ota) -> None:
        """User pressed Stop: best-effort Stop cmd, mark stopped."""
        try:
            await ota.send_stop()
        except Exception:  # noqa: BLE001
            pass
        self._push("stopped", {"stats": dict(ota.stats)})

    async def _poll_reboot(self, fw_path: str, mac: str) -> None:
        self._push("phase", {"name": "reboot"})
        for _ in range(3):
            if self._cancel.is_set():
                return
            await asyncio.sleep(2.0)
            try:
                rows = await host.scan_devices(3.0)
            except Exception:  # noqa: BLE001
                rows = []
            if any(r["address"].replace(":", "").lower()
                   == mac.replace(":", "").lower() for r in rows):
                self._push("reboot_ok", {})
                return
        self._push("reboot_timeout", {})


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------

class Heatmap(tk.Canvas):
    """Sector grid: one cell per 4096B sector, state colored, hover tooltip."""

    def __init__(self, master, n_sectors: int, **kw) -> None:
        super().__init__(master, bg=C_PANEL, highlightthickness=0, **kw)
        self.n = n_sectors
        self.states = ["idle"] * n_sectors
        self.attempts = [0] * n_sectors
        self._rects: list = []
        self._tip = None
        self.bind("<Motion>", self._on_motion)
        self.bind("<Leave>", lambda e: self._hide_tip())
        self.bind("<Configure>", lambda e: self.redraw())
        self._pulse_phase = 0

    def set_n(self, n: int) -> None:
        self.n = n
        self.states = ["idle"] * n
        self.attempts = [0] * n
        self._rects = []
        self.redraw()

    def set_state(self, sector: int, state: str) -> None:
        if 0 <= sector < self.n:
            self.states[sector] = state
            self._paint_cell(sector)

    def bump_attempt(self, sector: int) -> None:
        if 0 <= sector < self.n:
            self.attempts[sector] += 1

    def reset(self) -> None:
        self.states = ["idle"] * self.n
        self.attempts = [0] * self.n
        self.redraw()

    # -- painting ---------------------------------------------------------

    def redraw(self) -> None:
        self.delete("all")
        self._rects = []
        if self.n <= 0:
            return
        cols = SECTOR_COLUMNS
        cell_w = CELL
        cell_h = CELL
        rows = (self.n + cols - 1) // cols
        for i in range(self.n):
            r, c = divmod(i, cols)
            x = 8 + c * (cell_w + CELL_GAP)
            y = 8 + r * (cell_h + CELL_GAP)
            rect = self.create_rectangle(x, y, x + cell_w, y + cell_h,
                                         fill=CELL_COLORS["idle"], outline="",
                                         tags=("cell%d" % i,))
            self._rects.append(rect)
            self._paint_cell(i)
        self.create_text(8, 8 + rows * (cell_h + CELL_GAP) + 6, anchor="nw",
                         text="sector 热力图（每格 4KB，共 %d 格）" % self.n,
                         fill=C_DIM, font=("Microsoft YaHei UI", 8))
        self._draw_legend(8 + cols * (cell_w + CELL_GAP) + 16, 8)

    def _paint_cell(self, i: int) -> None:
        if i >= len(self._rects):
            return
        color = CELL_COLORS.get(self.states[i], C_IDLE)
        if self.states[i] == "sent" and self._pulse_phase:
            color = "#64b5f6"  # pulse lighter blue
        self.itemconfigure(self._rects[i], fill=color)

    def pulse(self) -> None:
        """Toggle the 'currently sending' highlight; called from GUI tick."""
        self._pulse_phase ^= 1
        for i, s in enumerate(self.states):
            if s == "sent":
                self._paint_cell(i)

    def _draw_legend(self, x: int, y: int) -> None:
        items = [("未发", C_IDLE), ("已ACK", C_OK), ("跳转", C_WARN),
                 ("重发", C_ERR), ("传输中", C_RUN)]
        yy = y
        for label, color in items:
            self.create_rectangle(x, yy, x + 14, yy + 14, fill=color,
                                  outline="")
            self.create_text(x + 20, yy + 7, anchor="w", text=label,
                             fill=C_DIM, font=("Microsoft YaHei UI", 8))
            yy += 20

    # -- tooltip -----------------------------------------------------------

    def _on_motion(self, ev) -> None:
        cols = SECTOR_COLUMNS
        c = (ev.x - 8) // (CELL + CELL_GAP)
        r = (ev.y - 8) // (CELL + CELL_GAP)
        idx = int(r * cols + c)
        if 0 <= idx < self.n and ev.x >= 8 and ev.y >= 8:
            self._show_tip(ev, idx)
        else:
            self._hit = -1
            self._hide_tip()

    def _show_tip(self, ev, idx: int) -> None:
        if getattr(self, "_hit", -1) == idx and self._tip:
            self._tip_place(ev)
            return
        self._hide_tip()
        self._hit = idx
        state_names = {"idle": "未发送", "sent": "传输中", "ok": "已ACK",
                       "jump": "跳转目标", "retry": "已重发"}
        txt = "sector %d  %s  尝试 %d 次" % (
            idx, state_names.get(self.states[idx], self.states[idx]),
            self.attempts[idx])
        self._tip = tk.Toplevel(self)
        self._tip.wm_overrideredirect(True)
        self._tip.wm_attributes("-topmost", True)
        lbl = tk.Label(self._tip, text=txt, bg="#000000", fg=C_FG,
                       font=("Microsoft YaHei UI", 9), padx=6, pady=2)
        lbl.pack()
        self._tip_place(ev)

    def _tip_place(self, ev) -> None:
        if self._tip:
            self._tip.wm_geometry("+%d+%d" % (ev.x + self.winfo_rootx() + 14,
                                              ev.y + self.winfo_rooty() + 10))

    def _hide_tip(self) -> None:
        if self._tip is not None:
            try:
                self._tip.destroy()
            except tk.TclError:
                pass  # already destroyed with its parent
            self._tip = None
        self._hit = -1


class OtaGui:
    """Tkinter front end. UI thread owns Tk; worker thread owns BLE I/O.

    Event flow: worker -> self.q -> _poll() (root.after) -> _dispatch().
    Headless drivable: button callbacks (on_scan/on_start/on_stop) and
    _dispatch are plain methods; self.seen records the full event stream
    for smoke tests that pump root.update() instead of mainloop().
    """

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.seen: list = []            # (t, kind, payload) transcript
        self.q: "queue.Queue" = queue.Queue()
        self.worker = OtaWorker(self.q)
        self.devices: list = []         # scan_devices() rows
        self.heat: "Heatmap" = None     # created in _build_ui
        self.fw_var = tk.StringVar()
        self.dry_var = tk.BooleanVar(value=False)
        self.verbose_var = tk.BooleanVar(value=False)
        self._busy = False
        self._pulse_tick = 0
        self._build_style()
        self._build_ui()
        self._log("就绪：扫描 → 选择设备 → 选择固件 → 开始烧录", "dim")
        root.protocol("WM_DELETE_WINDOW", self.on_close)
        root.after(POLL_MS, self._poll)

    # ---------------------------------------------------------------- UI

    def _build_style(self) -> None:
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure("ota.Horizontal.TProgressbar",
                        background=C_RUN, troughcolor=C_PANEL2,
                        bordercolor=C_PANEL, lightcolor=C_RUN,
                        darkcolor=C_RUN, thickness=14)

    def _btn(self, master, text, cmd, bg=C_PANEL2, fg=C_FG) -> tk.Button:
        return tk.Button(master, text=text, command=cmd, bg=bg, fg=fg,
                         activebackground=C_ACCENT, activeforeground="#000000",
                         relief="flat", padx=12, pady=3,
                         font=("Microsoft YaHei UI", 9), cursor="hand2")

    def _build_ui(self) -> None:
        self.root.title(APP_TITLE)
        self.root.geometry(GEOMETRY)
        self.root.configure(bg=C_BG)
        f_mono = ("Consolas", 9)
        f_ui = ("Microsoft YaHei UI", 9)
        f_small = ("Microsoft YaHei UI", 8)

        # -- toolbar ----------------------------------------------------
        bar = tk.Frame(self.root, bg=C_PANEL)
        bar.grid(row=0, column=0, sticky="ew", padx=6, pady=(6, 3))
        bar.columnconfigure(4, weight=1)
        self.btn_scan = self._btn(bar, "① 扫描设备", self.on_scan, bg="#2a3b4d")
        self.btn_scan.grid(row=0, column=0, padx=(2, 4), pady=4)
        tk.Label(bar, text="设备:", bg=C_PANEL, fg=C_DIM, font=f_ui)\
            .grid(row=0, column=1, sticky="w")
        self.dev_list = tk.Listbox(bar, height=4, bg=C_PANEL2, fg=C_FG,
                                   selectbackground=C_ACCENT,
                                   selectforeground="#000000",
                                   relief="flat", font=f_mono,
                                   exportselection=False)
        self.dev_list.grid(row=0, column=2, rowspan=2, padx=4, pady=2,
                           sticky="nw")
        self.dev_list.bind("<<ListboxSelect>>", lambda e: self._on_pick())
        self.btn_fw = self._btn(bar, "② 选择固件...", self.on_pick_fw)
        self.btn_fw.grid(row=0, column=3, padx=4, sticky="w")
        self.lbl_fw = tk.Label(bar, text="（未选择固件）", bg=C_PANEL,
                               fg=C_DIM, font=f_small, anchor="w")
        self.lbl_fw.grid(row=1, column=3, columnspan=2, sticky="ew", padx=4)
        tk.Label(bar, text="dry-run", bg=C_PANEL, fg=C_DIM, font=f_ui)\
            .grid(row=0, column=4, sticky="e")
        tk.Checkbutton(bar, variable=self.dry_var, bg=C_PANEL,
                       activebackground=C_PANEL, command=self._refresh_btns)\
            .grid(row=0, column=5, sticky="w")

        # -- body: heatmap | info --------------------------------------
        body = tk.Frame(self.root, bg=C_BG)
        body.grid(row=1, column=0, sticky="nsew", padx=6)
        self.root.rowconfigure(1, weight=1)
        self.root.columnconfigure(0, weight=1)
        body.columnconfigure(1, weight=1)
        body.rowconfigure(0, weight=1)

        left = tk.LabelFrame(body, text="sector 热力图", bg=C_PANEL, fg=C_FG)
        left.grid(row=0, column=0, sticky="nw", padx=(0, 6))
        self.heat = Heatmap(left, 0)
        self.heat.pack(padx=4, pady=4)

        right = tk.LabelFrame(body, text="进度 / 统计", bg=C_PANEL, fg=C_FG)
        right.grid(row=0, column=1, sticky="nsew")
        right.columnconfigure(1, weight=1)
        self.pbar = ttk.Progressbar(right, style="ota.Horizontal.TProgressbar",
                                    maximum=100.0, value=0)
        self.pbar.grid(row=0, column=0, columnspan=3, sticky="ew",
                       padx=10, pady=(10, 2))
        self.lbl_pct = tk.Label(right, text="0.0%", bg=C_PANEL, fg=C_ACCENT,
                                font=("Consolas", 22, "bold"))
        self.lbl_pct.grid(row=1, column=0, sticky="w", padx=10)
        self.lbl_bytes = tk.Label(right, text="0 / 0 B", bg=C_PANEL, fg=C_FG,
                                  font=f_mono)
        self.lbl_bytes.grid(row=1, column=1, sticky="w")
        self.lbl_rate = tk.Label(right, text="0.0 KB/s", bg=C_PANEL, fg=C_FG,
                                 font=("Consolas", 14))
        self.lbl_rate.grid(row=1, column=2, sticky="e", padx=10)
        self.lbl_eta = tk.Label(right, text="ETA --", bg=C_PANEL, fg=C_DIM,
                                font=f_small)
        self.lbl_eta.grid(row=2, column=2, sticky="e", padx=10)

        grid = tk.Frame(right, bg=C_PANEL)
        grid.grid(row=3, column=0, columnspan=3, sticky="ew", padx=10, pady=8)
        self.stat = {}
        stat_rows = [
            ("sector", "sector: --"), ("packets", "packets: 0"),
            ("elapsed", "耗时: 0.0s"), ("jumps", "跳转: 0"),
            ("retries", "重发: 0"), ("reconnects", "重连: 0"),
            ("mtu", "MTU: --"), ("fw", "固件: --"),
        ]
        for i, (key, text) in enumerate(stat_rows):
            lbl = tk.Label(grid, text=text, bg=C_PANEL, fg=C_DIM, font=f_mono)
            lbl.grid(row=i // 4, column=i % 4, sticky="w", padx=8, pady=2)
            self.stat[key] = lbl

        # -- event log ---------------------------------------------------
        logf = tk.LabelFrame(self.root, text="事件日志", bg=C_PANEL, fg=C_FG)
        logf.grid(row=2, column=0, sticky="ew", padx=6, pady=3)
        self.txt = tk.Text(logf, height=9, bg=C_PANEL, fg=C_FG, relief="flat",
                           font=f_mono, state="disabled", wrap="none")
        self.txt.pack(side="left", fill="both", expand=True, padx=(4, 0),
                      pady=4)
        sb = tk.Scrollbar(logf, command=self.txt.yview)
        sb.pack(side="left", fill="y", pady=4)
        self.txt["yscrollcommand"] = sb.set
        for tag, color in (("ok", C_OK), ("warn", C_WARN), ("err", C_ERR),
                           ("acc", C_ACCENT), ("dim", C_DIM)):
            self.txt.tag_configure(tag, foreground=color)
        tk.Checkbutton(logf, text="verbose", variable=self.verbose_var,
                       bg=C_PANEL, fg=C_DIM, activebackground=C_PANEL,
                       selectcolor=C_PANEL2)\
            .pack(side="right", anchor="s", padx=4, pady=4)

        # -- result banner + big buttons ---------------------------------
        banner = tk.Frame(self.root, bg=C_PANEL)
        banner.grid(row=3, column=0, sticky="ew", padx=6, pady=(3, 6))
        banner.columnconfigure(0, weight=1)
        self.lbl_banner = tk.Label(banner, text="IDLE", bg=C_PANEL, fg=C_DIM,
                                   font=("Consolas", 15, "bold"), anchor="w")
        self.lbl_banner.grid(row=0, column=0, sticky="w", padx=10, pady=6)
        self.btn_start = self._btn(banner, "③ 开始烧录", self.on_start,
                                   bg="#1b5e20")
        self.btn_start.grid(row=0, column=1, padx=4, pady=6)
        self.btn_stop = self._btn(banner, "停止", self.on_stop, bg="#8e2b28")
        self.btn_stop.grid(row=0, column=2, padx=4, pady=6)
        self.btn_stop.config(state="disabled")
        self._refresh_btns()

    # ------------------------------------------------------------ actions

    def on_scan(self) -> None:
        """Scan button: refresh the device list (also the Refresh action)."""
        if self._busy:
            return
        self._busy = True
        self._refresh_btns()
        self.worker.post(lambda w: w.op_scan(6.0))

    def _on_pick(self) -> None:
        self._log("选中: %s" % self.selected_device_line(), "dim")

    def selected_device(self) -> dict:
        """Row dict of the current Listbox selection (None if none)."""
        sel = self.dev_list.curselection()
        if not sel:
            return None
        return self.devices[sel[0]]

    def selected_device_line(self) -> str:
        d = self.selected_device()
        return ("%s @ %s" % (d["name"], d["address"])) if d else "（未选）"

    def select_device_mac(self, mac: str) -> bool:
        """Select the listbox row whose address matches mac. Test/driver aid."""
        mac_n = mac.replace(":", "").lower()
        for i, row in enumerate(self.devices):
            if row["address"].replace(":", "").lower() == mac_n:
                self.dev_list.selection_clear(0, "end")
                self.dev_list.selection_set(i)
                return True
        return False

    def on_pick_fw(self) -> None:
        path = filedialog.askopenfilename(
            title="选择固件镜像",
            filetypes=[("固件镜像", "*.bin"), ("所有文件", "*.*")])
        if path:
            self.set_firmware(path)

    def set_firmware(self, path: str) -> None:
        self.fw_var.set(path)
        self.lbl_fw.config(text=path, fg=C_FG)
        try:
            size = os.path.getsize(path)
            self.stat["fw"].config(
                text="固件: %d B (%d sectors)"
                     % (size, (size + host.SECTOR_SIZE - 1) // host.SECTOR_SIZE))
        except OSError:
            pass

    def on_start(self) -> None:
        if self._busy:
            return
        dev = self.selected_device()
        if dev is None:
            messagebox.showwarning(APP_TITLE, "请先扫描并选择设备")
            return
        fw = self.fw_var.get()
        if not fw or not os.path.isfile(fw):
            messagebox.showwarning(APP_TITLE, "请先选择固件 .bin 文件")
            return
        self.heat.reset()
        self._busy = True
        self._refresh_btns()
        # capture tkinter variables NOW (main thread): the worker lambda runs
        # on the asyncio thread where dry_var.get() would raise
        # "main thread is not in main loop" (ERR-011)
        dry = bool(self.dry_var.get())
        mac = dev["address"]
        self.worker.post(lambda w: w.op_flash(fw, mac, dry))

    def on_stop(self) -> None:
        self.worker.request_cancel()
        self._log("已请求停止（当前 sector 完成后中止）", "warn")

    def on_close(self) -> None:
        self.worker.stop()
        self.root.destroy()

    # ------------------------------------------------------------- polling

    def _poll(self) -> None:
        """Drain the worker queue on the UI thread; reschedule itself."""
        try:
            while True:
                t, kind, payload = self.q.get_nowait()
                self.seen.append((t, kind, payload))
                try:
                    self._dispatch(kind, payload)
                except Exception as exc:  # noqa: BLE001  UI must survive
                    self._log("dispatch %s 失败: %r" % (kind, exc), "err")
        except queue.Empty:
            pass
        self._pulse_tick += 1
        if self._pulse_tick % 4 == 0 and self.heat is not None:
            self.heat.pulse()
        self.root.after(POLL_MS, self._poll)

    def _dispatch(self, kind: str, p: dict) -> None:
        verbose = bool(self.verbose_var.get())
        if kind.startswith("ev:"):
            self._dispatch_proto(kind[3:], p, verbose)
            return
        if kind == "scan_start":
            self._banner("扫描中...", C_ACCENT)
            self._log("扫描设备（%.0fs）..." % p.get("timeout", 6.0), "dim")
        elif kind == "scan_done":
            self._busy = False
            self._refresh_btns()
            self.devices = p.get("devices", [])
            self.dev_list.delete(0, "end")
            for row in self.devices:
                rssi = row.get("rssi")
                self.dev_list.insert(
                    "end", "%-18s %-18s %s"
                    % (row["name"], row["address"],
                       ("%d dBm" % rssi) if rssi is not None else "n/a"))
            if p.get("error"):
                self._banner("扫描失败", C_ERR)
                self._log("扫描出错: %s" % p["error"], "err")
            elif self.devices:
                self._banner("已发现 %d 台设备，选择后开始" % len(self.devices),
                             C_OK)
                self._log("发现 %d 台设备" % len(self.devices), "ok")
            else:
                self._banner("未发现设备", C_WARN)
                self._log("未发现 C6-OTA-* 设备（检查供电/广播/适配器）", "warn")
        elif kind == "fw_loaded":
            self.heat.set_n(p["sectors"])
            self.stat["fw"].config(text="固件: %d B (%d sectors)"
                                        % (p["size"], p["sectors"]))
            self._log("固件载入: %s（%d B / %d sectors）"
                      % (os.path.basename(p["path"]), p["size"], p["sectors"]),
                      "acc")
        elif kind == "phase":
            names = {"connect": "连接中...", "handshake": "握手中...",
                     "transfer": "传输中...", "stop": "发送 Stop...",
                     "reboot": "等待设备重启..."}
            self._banner(names.get(p["name"], p["name"]), C_RUN)
            self._log("阶段: %s" % names.get(p["name"], p["name"]), "dim")
        elif kind == "mtu":
            self.stat["mtu"].config(text="MTU: %d" % p.get("mtu", 0))
        elif kind == "sector_current":
            self.stat["sector"].config(text="sector: %s" % p.get("sector"))
        elif kind == "progress":
            self._on_progress(p)
        elif kind == "start_ok":
            self._log("Start ACK ok（status=0x0001）", "ok")
        elif kind == "done":
            self._busy = False
            self._refresh_btns()
            self._apply_stats(p.get("stats", {}))
            if p.get("dry"):
                self._banner("DRY-RUN 通过（握手验证，未传数据）", C_OK)
                self._log("dry-run PASS", "ok")
            else:
                self.lbl_pct.config(text="100%", fg=C_OK)
                self._banner("SUCCESS — 升级成功，设备重启中", C_OK)
                self._log("OTA 完成: %.1fs" % p.get("elapsed", 0.0), "ok")
        elif kind == "fail":
            self._busy = False
            self._refresh_btns()
            self._apply_stats(p.get("stats", {}))
            self._banner("FAILED — %s" % p.get("reason", "未知错误"), C_ERR)
            self._log("失败: %s" % p.get("reason"), "err")
        elif kind in ("cancelled", "stopped"):
            self._busy = False
            self._refresh_btns()
            self._apply_stats(p.get("stats", {}))
            self._banner("已停止", C_WARN)
            self._log("用户中止", "warn")
        elif kind == "log":
            self._log(p.get("text", ""), "warn")
        elif kind == "reboot_ok":
            self._banner("SUCCESS — 设备已重启上线", C_OK)
            self._log("设备重新广播，重启确认 OK", "ok")
        elif kind == "reboot_timeout":
            self._log("重启窗口内未扫到设备广播（可手动复查串口锚点）", "warn")
        elif kind == "error":
            self._log("worker 异常: %s" % p.get("message"), "err")
            self._banner("ERROR — worker 异常", C_ERR)
            self._busy = False
            self._refresh_btns()
        elif verbose:
            self._log("%s %s" % (kind, p), "dim")

    def _dispatch_proto(self, ev: str, p: dict, verbose: bool) -> None:
        """Protocol-level events bridged from BleakOtaClient.emit()."""
        if ev == "sector_sent":
            self.heat.set_state(p["sector"], "sent")
            if verbose:
                self._log("sector %d 发送（%d 包）" % (p["sector"], p["packets"]),
                          "dim")
        elif ev == "sector_ack":
            self.heat.set_state(p["sector"], "ok")
            if verbose:
                self._log("sector %d ACK" % p["sector"], "dim")
        elif ev == "sector_jump":
            self.heat.set_state(p["sent"], "jump")
            self._log("设备跳转: 收到 %d，期望 %d（断点续传）"
                      % (p["sent"], p["expect"]), "warn")
        elif ev == "ack_timeout":
            self.heat.bump_attempt(p["sector"])
            self.heat.set_state(p["sector"], "retry")
            self._log("sector %d ACK 超时（%d/%d），重发"
                      % (p["sector"], p["attempt"], p["retries"]), "warn")
        elif ev == "ack_bad_frame":
            self._log("sector %d ACK 坏帧: %s" % (p["sector"], p["frame"]), "err")
        elif ev == "ack_crc_mismatch":
            self._log("CRC 校验失败: kind=%s %s" % (p.get("kind"), p.get("frame")),
                      "err")
        elif ev == "ack_unknown":
            self._log("sector %d 未知状态 0x%04x" % (p["sector"], p["status"]),
                      "err")
        elif ev == "start_ack":
            self._log("Start ACK 帧收到", "ok")
        elif ev == "stop_ack":
            self._log("Stop ACK（status=0x%04x）" % p.get("status", -1), "ok")
        elif ev == "stop_timeout":
            self._log("Stop ACK 超时（设备可能在重启，可忽略）", "warn")
        elif ev == "connect_fail_retry":
            self._log("连接失败，准备重试: %s" % p.get("error"), "warn")
        elif ev == "reconnect_exhausted":
            self._log("重连预算耗尽: %s" % p.get("error"), "err")
        elif ev == "mtu":
            self.stat["mtu"].config(text="MTU: %d" % p.get("mtu", 0))
        elif verbose:
            self._log("ev %s %s" % (ev, p), "dim")

    # ------------------------------------------------------------- helpers

    def _on_progress(self, p: dict) -> None:
        total = max(p.get("total", 1), 1)
        done = p.get("done_bytes", 0)
        pct = min(done * 100.0 / total, 100.0)
        self.pbar.config(value=pct)
        self.lbl_pct.config(text="%.1f%%" % pct)
        self.lbl_bytes.config(text="%d / %d B" % (done, total))
        rate = p.get("rate", 0.0)
        self.lbl_rate.config(text="%.1f KB/s" % rate)
        eta = p.get("eta")
        self.lbl_eta.config(text=("ETA %.0fs" % eta) if eta else "ETA --")
        self.stat["elapsed"].config(text="耗时: %.1fs" % p.get("elapsed", 0.0))
        self._apply_stats(p.get("stats", {}))

    def _apply_stats(self, stats: dict) -> None:
        if not stats:
            return
        self.stat["packets"].config(
            text="packets: %d" % stats.get("packets_sent", 0))
        self.stat["jumps"].config(text="跳转: %d" % stats.get("sector_jumps", 0))
        self.stat["retries"].config(
            text="重发: %d" % stats.get("tail_retries", 0))
        self.stat["reconnects"].config(
            text="重连: %d" % stats.get("reconnects", 0))

    def _banner(self, text: str, color: str) -> None:
        self.lbl_banner.config(text=text, fg=color)

    def _refresh_btns(self) -> None:
        self.btn_start.config(state="disabled" if self._busy else "normal")
        self.btn_scan.config(state="disabled" if self._busy else "normal")
        self.btn_stop.config(state="normal" if self._busy else "disabled")

    def _log(self, text: str, tag: str = "") -> None:
        stamp = time.strftime("%H:%M:%S")
        self.txt.config(state="normal")
        self.txt.insert("end", "[%s] %s\n" % (stamp, text), (tag,) if tag else ())
        if int(self.txt.index("end-1c").split(".")[0]) > 800:
            self.txt.delete("1.0", "200.0")   # cap log growth
        self.txt.see("end")
        self.txt.config(state="disabled")


def main() -> int:
    root = tk.Tk()
    app = OtaGui(root)
    root.protocol("WM_DELETE_WINDOW", app.on_close)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
