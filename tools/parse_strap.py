#!/usr/bin/env python3
import struct
d = open("/tmp/strap.bin", "rb").read()
v = struct.unpack("<I", d[:4])[0]
print(f"GPIO_STRAP0 = 0x{v:08X}")
print("  GPIO9 (BOOT):", "LOW -> download mode" if (v & 0x1) == 0 else "HIGH -> normal boot")
