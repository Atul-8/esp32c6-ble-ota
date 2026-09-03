#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Serial log capture via WSL /dev/ttyACM0 pyserial (ERR-003: DTR must be off)."""
import sys
import time

import serial

PORT = "/dev/ttyACM0"
BAUD = 115200
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
OUT = sys.argv[2] if len(sys.argv) > 2 else "/tmp/serial_capture.log"

ser = serial.Serial(PORT, BAUD, timeout=1)
# ERR-003 教训：DTR/RTS 拉高会破坏 strap 进下载模式，保持 False
ser.setDTR(False)
ser.setRTS(False)

t0 = time.time()
chunks = []
while time.time() - t0 < DURATION:
    n = ser.in_waiting
    if n:
        chunks.append(ser.read(n))
    else:
        time.sleep(0.02)
ser.close()

data = b"".join(chunks)
with open(OUT, "wb") as f:
    f.write(data)
sys.stdout.write(data.decode("utf-8", errors="replace"))
