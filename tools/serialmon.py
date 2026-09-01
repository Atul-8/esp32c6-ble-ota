#!/usr/bin/env python3
"""serialmon.py — 复位 C6 并捕获启动日志（在 WSL 用 IDF venv python 运行）"""
import sys, time, serial

PORT = "/dev/ttyACM0"

def reset_and_capture(duration=14, wait_baud=115200):
    # 用 esptool 的 USB-JTAG/Serial 复位逻辑：先以 esptool 触发 hard reset
    import subprocess
    esptool = sys.argv[2] if len(sys.argv) > 2 else "esptool"
    subprocess.run([esptool, "--chip", "esp32c6", "-p", PORT,
                    "--before", "default-reset", "--after", "hard-reset",
                    "chip-id"], capture_output=True, timeout=30)
    time.sleep(0.3)
    s = serial.Serial(PORT, wait_baud, timeout=0.5)
    end = time.time() + duration
    out = b""
    while time.time() < end:
        d = s.read(512)
        if d:
            out += d
            if b"Restarting" in out:
                # 再读 2 秒收尾
                end2 = time.time() + 2
                while time.time() < end2:
                    d = s.read(512)
                    if d: out += d
                break
    s.close()
    return out.decode(errors="replace")

if __name__ == "__main__":
    print(reset_and_capture())
