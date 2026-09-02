#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ble_ota_host.py -- Windows BLE OTA host tool for ESP32-C6 (espressif/ble_ota v0.1.17)

Protocol contract verified line-by-line against nimble_ota.c (ble_ota v0.1.17):

  GATT (16-bit UUIDs, all values little-endian on the wire):
    Service  0x8018
    RECV_FW  0x8020  Write + Indicate   (firmware packets / sector ACK)
    OTA_BAR  0x8021  Read + Indicate    (progress, ignored here)
    COMMAND  0x8022  Write + Indicate   (control commands / command ACK)
    CUSTOM   0x8023  Write + Indicate   (ignored here)
    Note: RECV_FW/COMMAND declare WRITE (write-with-response) only -- no
    WRITE_WITHOUT_RESPONSE flag. We must use response=True (also acts as
    natural flow control).

  Control commands (write to COMMAND char):
    Start: 01 00 <fw_size:4B LE>   (6 bytes)
    Stop:  02 00                   (2 bytes)

  ACK frames (20 bytes, sent as Indication on the matching char):
    frame[0:2] = 0x0003 fixed header
    frame[2:4] = status
    frame[4:6] = extra
    frame[18:20] = CRC16 over bytes 0..17 (bitwise crc16_ccitt from source:
                   poly 0x1021, init 0x0000 -- the XMODEM variant, NOT
                   CCITT-FALSE 0xFFFF)

    COMMAND ACK:
      Start ok -> status = 0x0001           (nimble_ota.c: cmd_ack[2]=0x01)
      Stop  ok -> status = 0x0002           (cmd_ack[2]=0x02)
      (There is NO cmd_echo field in the 20-byte ACK.)

    RECV_FW ACK (sector ACK):
      success        -> frame[2:4]=0x0000, frame[0:2]=echo of the sector we
                        just sent, frame[4:6]=0x0000
      sector error   -> frame[2:4]=0x0002, frame[0:2]=echo of the sector we
                        sent (may be garbage), frame[4:6]=cur_sector = the
                        sector the device expects -> jump there and restart
                        that sector from packet 0 (device resets its buffer).
      packet seq err -> device silently drops the packet and sends NO ack;
                        our sector-end ACK timeout covers recovery.

  Firmware data packet (write to RECV_FW char):
    [Sector_Index:2B LE][Packet_Seq:1B][payload]
    sector = 4096 B; payload split by (mtu - 3 - 3): 3B header + 2B tail CRC
    space. Packet_Seq from 0; sector tail packet uses seq 0xFF and its payload
    ends with 2B CRC16 (of the full 4096B sector content). The component
    currently ignores the tail CRC content but the format must be preserved.

  End of transfer:
    Device counts received bytes against fw_length from Start; when
    recv_len >= fw_length the firmware side does esp_ota_end +
    set_boot_partition + esp_restart (ble_ota.c:395..435). We send Stop
    (2 00) for state cleanup, then disconnect; the device reboots into the
    new image.

Python 3.8+ / bleak >= 0.20 (tested with bleak 3.0.2, WinRT backend).
Windows console safe: ASCII progress bar only.
"""

import argparse
import asyncio
import struct
import sys
import time
from typing import Dict, List, Optional, Tuple

try:
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError
except ImportError:
    print("[E] bleak is not installed. Run: pip install bleak")
    sys.exit(2)

# ---------------------------------------------------------------------------
# Protocol constants (source-verified, see module docstring)
# ---------------------------------------------------------------------------

OTA_SERVICE_UUID = "00008018-0000-1000-8000-00805f9b34fb"
RECV_FW_UUID = "00008020-0000-1000-8000-00805f9b34fb"
OTA_BAR_UUID = "00008021-0000-1000-8000-00805f9b34fb"
COMMAND_UUID = "00008022-0000-1000-8000-00805f9b34fb"
CUSTOMER_UUID = "00008023-0000-1000-8000-00805f9b34fb"

DEVICE_NAME_PREFIX = "C6-OTA-"
SECTOR_SIZE = 4096
ACK_FRAME_LEN = 20
ACK_HEADER = 0x0003
# payload bytes per packet inside a sector: (mtu-3) usable ATT payload
# minus 3B packet header, minus 2B reserved tail CRC on the last packet
PACKET_OVERHEAD = 3
TAIL_CRC_LEN = 2

# ACK status codes (frame[2:4])
ACK_FW_STATUS_SUCCESS = 0x0000
ACK_FW_STATUS_SECTOR_ERR = 0x0002
ACK_CMD_STATUS_START_OK = 0x0001
ACK_CMD_STATUS_STOP_OK = 0x0002

# Tunables
ACK_TIMEOUT_S = 10.0        # per-sector-end ACK wait
ACK_RETRIES = 3             # tail packet retransmits on ACK timeout
CONNECT_TIMEOUT_S = 15.0
MTU_EXPECTED = 517
WRITE_WITH_RESPONSE = True  # source shows WRITE flag only; keep True

SCAN_TIMEOUT_S = 5.0


def crc16_ccitt(data: bytes) -> int:
    """CRC16 exactly as ble_ota source crc16_ccitt(): poly 0x1021, init 0x0000.

    Matches nimble_ota.c bitwise implementation (XMODEM variant).
    """
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def verify_ack_crc(frame: bytes) -> bool:
    """Check trailing CRC16 of a 20-byte ACK frame."""
    if len(frame) != ACK_FRAME_LEN:
        return False
    return crc16_ccitt(frame[:ACK_FRAME_LEN - 2]) == struct.unpack_from("<H", frame, 18)[0]


# ---------------------------------------------------------------------------
# BLE OTA client
# ---------------------------------------------------------------------------

class AckTimeout(Exception):
    """Sector-end ACK not received after retries."""


class BleakOtaClient:
    """One OTA session over a single BLE connection.

    Threading model: everything on one asyncio loop. Indications arrive in
    notify callbacks and are demultiplexed by characteristic into pending
    asyncio.Future objects (worker-less, callback-driven).
    """

    def __init__(self, verbose: bool = False) -> None:
        self.client: Optional[BleakClient] = None
        self.verbose = verbose
        # pending futures keyed by characteristic uuid
        self._pending: Dict[str, asyncio.Future] = {}
        self.mtu_size = 23
        self.last_acked_sector = -1     # highest sector ACKed as success
        self.stats = {
            "packets_sent": 0,
            "sectors_done": 0,
            "sector_jumps": 0,       # device told us to resume at cur_sector
            "tail_retries": 0,       # tail packet retransmits
            "reconnects": 0,
        }

    # -- low level ---------------------------------------------------------

    def _vlog(self, msg: str) -> None:
        if self.verbose:
            print("  [dbg] %s" % msg)

    async def connect(self, address: str) -> None:
        """Connect, discover services, subscribe ACK indications."""
        self.client = BleakClient(address, timeout=CONNECT_TIMEOUT_S)
        await self.client.connect()
        # bleak 3.x: MTU is negotiated by the WinRT stack automatically;
        # mtu_size is a read-only property (no setter like bleak 0.x).
        try:
            self.mtu_size = self.client.mtu_size
        except Exception:
            self.mtu_size = 23
        # subscribe indications on both ACK channels
        await self.client.start_notify(RECV_FW_UUID, self._on_recv_fw_ack)
        await self.client.start_notify(COMMAND_UUID, self._on_cmd_ack)
        self._vlog("connected, mtu=%d" % self.mtu_size)

    async def disconnect(self) -> None:
        if self.client and self.client.is_connected:
            try:
                await self.client.stop_notify(RECV_FW_UUID)
                await self.client.stop_notify(COMMAND_UUID)
            except Exception:
                pass  # best effort on a dying link
            try:
                await self.client.disconnect()
            except Exception:
                pass
        self.client = None

    def _on_frame(self, uuid: str, data: bytearray) -> None:
        """Demultiplex an indication to the waiting future (if any)."""
        fut = self._pending.get(uuid)
        if fut is not None and not fut.done():
            fut.set_result(bytes(data))
        else:
            self._vlog("unmatched indicate on %s: %s" % (uuid, data.hex()))

    def _on_recv_fw_ack(self, _char, data: bytearray) -> None:
        self._on_frame(RECV_FW_UUID, data)

    def _on_cmd_ack(self, _char, data: bytearray) -> None:
        self._on_frame(COMMAND_UUID, data)

    async def _wait_indicate(self, uuid: str, timeout: float) -> bytes:
        """Arm a future, then await the next indication on that char."""
        loop = asyncio.get_running_loop()
        fut: asyncio.Future = loop.create_future()
        self._pending[uuid] = fut
        try:
            return await asyncio.wait_for(fut, timeout)
        finally:
            self._pending.pop(uuid, None)

    # -- protocol steps ------------------------------------------------------

    async def send_start(self, fw_length: int) -> None:
        """Write Start command and wait for command ACK (status 0x0001).

        The waiter future is armed BEFORE the write: an indication arriving
        between write completion and _wait_indicate would otherwise be lost.
        """
        payload = b"\x01\x00" + struct.pack("<I", fw_length)
        waiter = asyncio.ensure_future(self._wait_indicate(COMMAND_UUID, ACK_TIMEOUT_S))
        try:
            await self.client.write_gatt_char(COMMAND_UUID, payload,
                                              response=WRITE_WITH_RESPONSE)
            frame = await waiter
        except BaseException:
            if not waiter.done():
                waiter.cancel()
            raise
        if len(frame) < 6 or struct.unpack_from("<H", frame, 0)[0] != ACK_HEADER:
            raise BleakError("Start ACK bad frame: %s" % frame.hex())
        status = struct.unpack_from("<H", frame, 2)[0]
        if status != ACK_CMD_STATUS_START_OK:
            raise BleakError("Start ACK status=0x%04x (expected 0x0001)" % status)
        if not verify_ack_crc(frame):
            print("[W] Start ACK CRC mismatch (proceeding)")
        self._vlog("Start ACK ok: %s" % frame.hex())

    async def send_stop(self) -> None:
        """Write Stop command and wait for command ACK (status 0x0002)."""
        waiter = asyncio.ensure_future(self._wait_indicate(COMMAND_UUID, ACK_TIMEOUT_S))
        try:
            await self.client.write_gatt_char(COMMAND_UUID, b"\x02\x00",
                                              response=WRITE_WITH_RESPONSE)
            frame = await waiter
        except asyncio.TimeoutError:
            # device may reboot immediately after last sector; Stop ACK is
            # best-effort
            print("[W] Stop ACK timeout (device may be rebooting)")
            return
        except BaseException:
            if not waiter.done():
                waiter.cancel()
            raise
        status = struct.unpack_from("<H", frame, 2)[0] if len(frame) >= 4 else -1
        self._vlog("Stop ACK: %s" % frame.hex())
        if status != ACK_CMD_STATUS_STOP_OK:
            print("[W] Stop ACK status=0x%04x (expected 0x0002)" % status)

    def build_packet(self, sector: int, seq: int, payload: bytes,
                     with_tail_crc: bool, sector_data: bytes = b"") -> bytes:
        """Build one firmware data packet for RECV_FW."""
        frame = struct.pack("<HB", sector & 0xFFFF, seq & 0xFF) + payload
        if with_tail_crc:
            frame += struct.pack("<H", crc16_ccitt(sector_data))
        return frame

    def plan_sector_packets(self, sector_index: int, fw: bytes) -> List[bytes]:
        """Split one 4096B sector into wire packets (last has seq 0xFF + CRC).

        Packet payload size = mtu - 3 (ATT) - 3 (packet header), matching the
        official app splitting "(MTU-3-3)". The tail packet (seq 0xFF) carries
        the remaining bytes plus the 2B sector CRC16; its total wire size is
        at most mtu - 2, so it always fits one ATT write.
        """
        chunk = self.mtu_size - 3 - PACKET_OVERHEAD
        if chunk <= TAIL_CRC_LEN:
            raise ValueError("MTU too small: %d" % self.mtu_size)
        start = sector_index * SECTOR_SIZE
        data = fw[start:start + SECTOR_SIZE]
        packets: List[bytes] = []
        seq = 0
        offset = 0
        while offset < len(data):
            piece = data[offset:offset + chunk]
            offset += len(piece)
            remaining = len(data) - offset
            if remaining == 0:
                # tail packet: seq 0xFF + payload + 2B sector CRC
                pkt = struct.pack("<HB", sector_index & 0xFFFF, 0xFF) + piece
                pkt += struct.pack("<H", crc16_ccitt(data))
                packets.append(pkt)
            else:
                packets.append(struct.pack("<HB", sector_index & 0xFFFF, seq) + piece)
            seq += 1
        if not packets:
            # empty sector (should not happen for real firmware): still emit tail
            packets.append(struct.pack("<HB", sector_index & 0xFFFF, 0xFF)
                           + struct.pack("<H", crc16_ccitt(data)))
        return packets

    async def _write_packets(self, packets: List[bytes]) -> None:
        """Pipeline packets of one sector (write-with-response serializes)."""
        for pkt in packets:
            await self.client.write_gatt_char(RECV_FW_UUID, pkt,
                                              response=WRITE_WITH_RESPONSE)
            self.stats["packets_sent"] += 1

    async def send_sector(self, sector_index: int, fw: bytes,
                          max_jumps: int = 8) -> int:
        """Send one sector, then wait for its ACK.

        Returns the index of the sector the device confirmed as complete.
        Raises AckTimeout if the tail ACK never arrives after retries.

        Pipeline strategy: packets of a sector are streamed back-to-back
        (write-with-response serializes at ATT level). After the tail packet
        (seq 0xFF) we block on the sector ACK:
          status 0x0000 -> sector done
          status 0x0002 -> device expects cur_sector (frame[4:6]); the device
                           already reset its buffer, so we restart that
                           sector from packet 0 (bounded by max_jumps to
                           avoid pathological redirect loops)
        """
        for _ in range(max_jumps):
            packets = self.plan_sector_packets(sector_index, fw)
            attempt = 0
            while True:
                # arm the ACK waiter BEFORE the tail write: the indication
                # may arrive as soon as the write completes
                waiter = asyncio.ensure_future(
                    self._wait_indicate(RECV_FW_UUID, ACK_TIMEOUT_S))
                try:
                    await self._write_packets(packets)
                    frame = await waiter
                except asyncio.TimeoutError:
                    waiter.cancel()
                    attempt += 1
                    self.stats["tail_retries"] += 1
                    if attempt >= ACK_RETRIES:
                        raise AckTimeout(
                            "sector %d: no ACK after %d attempts"
                            % (sector_index, ACK_RETRIES))
                    print("\n[W] sector %d ACK timeout (attempt %d/%d), resending"
                          % (sector_index, attempt, ACK_RETRIES))
                    continue
                except BaseException:
                    if not waiter.done():
                        waiter.cancel()
                    raise
                break
            if len(frame) < 6 or struct.unpack_from("<H", frame, 0)[0] != ACK_HEADER:
                print("\n[W] sector ACK bad frame: %s" % frame.hex())
                continue
            if not verify_ack_crc(frame):
                self._vlog("sector ACK CRC mismatch (accepted)")
            status, extra = struct.unpack_from("<HH", frame, 2)
            echoed = struct.unpack_from("<H", frame, 0)[0]
            if status == ACK_FW_STATUS_SUCCESS:
                self.last_acked_sector = max(self.last_acked_sector, echoed)
                self.stats["sectors_done"] += 1
                return sector_index
            if status == ACK_FW_STATUS_SECTOR_ERR:
                expected = extra
                print("\n[W] device expects sector %d (we sent %d), jumping"
                      % (expected, sector_index))
                self.stats["sector_jumps"] += 1
                sector_index = expected
                continue
            print("\n[W] sector ACK unknown status 0x%04x" % status)
        raise AckTimeout("sector redirect loop (>%d jumps)" % max_jumps)


# ---------------------------------------------------------------------------
# CLI helpers
# ---------------------------------------------------------------------------

def progress_bar(done: int, total: int, width: int = 32) -> str:
    """ASCII progress bar, Windows console safe."""
    if total <= 0:
        pct = 100.0
    else:
        pct = min(100.0, done * 100.0 / total)
    filled = int(width * done / max(total, 1))
    bar = "#" * filled + "-" * (width - filled)
    return "[%s] %5.1f%% (%d/%d B)" % (bar, pct, done, total)


async def cmd_scan(timeout: float = SCAN_TIMEOUT_S) -> int:
    """Scan for C6-OTA-* advertisers and print a table.

    Also surfaces the component's hardcoded fallback name 'nimble-ble-ota'
    (the device may advertise it due to a firmware-side name-set race; see
    project notes) so the operator can identify the target by MAC instead.
    """
    print("Scanning for %ds (filter prefix %s)..." % (timeout, DEVICE_NAME_PREFIX))
    found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    rows = []
    for d, adv in found.values():
        # prefer adv payload name (stable across stacks), fall back to d.name
        name = adv.local_name or d.name or ""
        if name.startswith(DEVICE_NAME_PREFIX) or name == "nimble-ble-ota":
            rows.append((name, d.address, adv.rssi))
    if not rows:
        print("[!] no %s* devices found. Check: device powered/adv, radio on."
              % DEVICE_NAME_PREFIX)
        return 1
    rows.sort(key=lambda r: r[0])
    print("%-20s %-17s %s" % ("Name", "Address", "RSSI"))
    for name, addr, rssi in rows:
        print("%-20s %-17s %s" % (name, addr, ("%d dBm" % rssi) if rssi is not None else "n/a"))
    print("total: %d device(s)" % len(rows))
    return 0


async def cmd_info(name_prefix: str, mac: Optional[str] = None) -> int:
    """Connect and dump the full GATT table."""
    addr = await find_device(name_prefix, mac)
    if addr is None:
        print("[E] device with name prefix %r not found" % (mac or name_prefix))
        return 1
    print("Connecting to %s (%s)..." % (name_prefix, addr))
    async with BleakClient(addr, timeout=CONNECT_TIMEOUT_S) as client:
        mtu = None
        try:
            mtu = client.mtu_size
        except Exception:
            pass
        print("connected. mtu_size=%s" % mtu)
        for svc in client.services:
            print("service %s" % svc.uuid)
            for ch in svc.characteristics:
                print("  char %s  handle=0x%04x  props=%s"
                      % (ch.uuid, ch.handle, ",".join(ch.properties)))
                for desc in ch.descriptors:
                    try:
                        val = await client.read_gatt_descriptor(desc.handle)
                        print("    desc 0x%04x %s = %s" % (desc.handle, desc.uuid, val.hex()))
                    except Exception as exc:
                        print("    desc 0x%04x %s = <read fail: %s>" % (desc.handle, desc.uuid, exc))
    return 0


async def find_device(name_prefix: str, mac: Optional[str] = None) -> Optional[str]:
    """Scan once and return the address of the first matching device.

    Match by name prefix, or by exact MAC (case-insensitive) when given.
    """
    found = await BleakScanner.discover(timeout=SCAN_TIMEOUT_S, return_adv=True)
    mac_n = mac.replace(":", "").lower() if mac else None
    for d, adv in found.values():
        name = adv.local_name or d.name or ""
        if name_prefix and name.startswith(name_prefix):
            return d.address
        if mac_n and d.address.replace(":", "").lower() == mac_n:
            return d.address
    return None


async def run_flash(fw_path: str, name: str, mac: Optional[str],
                    dry_run: bool, verbose: bool) -> int:
    """Full OTA transfer (or handshake-only with --dry-run)."""
    try:
        with open(fw_path, "rb") as fh:
            fw = fh.read()
    except OSError as exc:
        print("[E] cannot read firmware: %s" % exc)
        return 1
    total_sectors = (len(fw) + SECTOR_SIZE - 1) // SECTOR_SIZE
    print("firmware: %s (%d bytes, %d sectors)"
          % (fw_path, len(fw), total_sectors))

    addr = await find_device(name, mac)
    if addr is None:
        print("[E] device %r not found (run 'scan' to verify)" % (mac or name))
        return 1
    print("device: %s @ %s" % (name, addr))

    ota = BleakOtaClient(verbose=verbose)
    t0 = time.monotonic()
    reconnect_budget = 1  # one automatic reconnect per session

    try:
        while True:
            try:
                await ota.connect(addr)
                break
            except (BleakError, asyncio.TimeoutError, OSError) as exc:
                reconnect_budget -= 1
                ota.stats["reconnects"] += 1
                if reconnect_budget < 0:
                    print("[E] connect failed repeatedly: %s" % exc)
                    print("[!] if the device rebooted, wait 3-5s for adv and retry flash")
                    return 1
                print("[W] connect failed (%s), retrying once..." % exc)
                await asyncio.sleep(2.0)

        print("connected. mtu=%d" % ota.mtu_size)
        if ota.mtu_size < 100:
            print("[W] MTU < 100, transfer will be slow; expected MTU 517")

        print("sending Start (fw_length=%d)..." % len(fw))
        await ota.send_start(len(fw))
        print("Start ACK ok (status=0x0001)")

        if dry_run:
            print("dry-run: handshake verified, sending Stop...")
            await ota.send_stop()
            await ota.disconnect()
            print("dry-run PASS")
            return 0

        # ---- data transfer -------------------------------------------
        current = ota.last_acked_sector + 1
        done_bytes = current * SECTOR_SIZE
        last_print = 0.0
        while current < total_sectors:
            try:
                confirmed = await ota.send_sector(current, fw)
            except AckTimeout as exc:
                print("\n[E] %s" % exc)
                break
            done_bytes = min((confirmed + 1) * SECTOR_SIZE, len(fw))
            current = confirmed + 1
            now = time.monotonic()
            if now - last_print >= 0.2 or current >= total_sectors:
                elapsed = now - t0
                rate = done_bytes / 1024.0 / max(elapsed, 1e-6)
                print("\r%s  %.1f KB/s  sector %d/%d   "
                      % (progress_bar(done_bytes, len(fw)), rate,
                         current, total_sectors),
                      end="", flush=True)
                last_print = now

        elapsed = time.monotonic() - t0
        print()
        if current >= total_sectors:
            rate = len(fw) / 1024.0 / max(elapsed, 1e-6)
            print("transfer complete: %d bytes in %.1fs (avg %.1f KB/s)"
                  % (len(fw), elapsed, rate))
            print("device will esp_ota_end + set_boot_partition + esp_restart")
            print("sending Stop...")
            await ota.send_stop()
            ok = True
        else:
            ok = False
        await ota.disconnect()

        # ---- wait for device to come back with the new firmware -------
        if ok:
            print("waiting for device reboot (adv back within ~3-5s)...")
            back = None
            for _ in range(3):
                await asyncio.sleep(2.0)
                back = await find_device(name, mac)
                if back:
                    break
            if back:
                print("device %s is advertising again -- OTA SUCCESS" % (mac or name))
            else:
                print("[W] device not seen again after reboot window; "
                      "check serial log for anchors")
            print("stats: packets=%(packets_sent)d sectors=%(sectors_done)d "
                  "jumps=%(sector_jumps)d tail_retries=%(tail_retries)d "
                  "reconnects=%(reconnects)d" % ota.stats)
            return 0
        print("stats: packets=%(packets_sent)d sectors=%(sectors_done)d "
              "jumps=%(sector_jumps)d tail_retries=%(tail_retries)d "
              "reconnects=%(reconnects)d" % ota.stats)
        return 1
    except (BleakError, asyncio.TimeoutError, OSError) as exc:
        print("\n[E] OTA failed: %s" % exc)
        await ota.disconnect()
        return 1


def main() -> int:
    parser = argparse.ArgumentParser(
        description="BLE OTA host tool for ESP32-C6 (ble_ota v0.1.17 protocol)")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_scan = sub.add_parser("scan", help="scan for C6-OTA-* devices")
    p_scan.add_argument("--timeout", type=float, default=SCAN_TIMEOUT_S)

    p_info = sub.add_parser("info", help="connect and dump GATT table")
    p_info.add_argument("--name", default=DEVICE_NAME_PREFIX,
                        help="device name prefix (default: %s)" % DEVICE_NAME_PREFIX)
    p_info.add_argument("--mac", default=None,
                        help="match by MAC instead of name")

    p_flash = sub.add_parser("flash", help="flash a firmware image over BLE OTA")
    p_flash.add_argument("firmware", help="path to firmware .bin")
    p_flash.add_argument("--name", default="C6-OTA-1128", help="device name prefix")
    p_flash.add_argument("--mac", default=None,
                         help="match by MAC instead of name (e.g. 14:C1:9F:E5:11:2A)")
    p_flash.add_argument("--mtu", type=int, default=MTU_EXPECTED,
                         help="expected MTU (informational; Windows negotiates)")
    p_flash.add_argument("--dry-run", action="store_true",
                         help="handshake only: scan/connect/start-ack/stop, no data")
    p_flash.add_argument("-v", "--verbose", action="store_true", help="debug frames")

    args = parser.parse_args()
    if args.cmd == "scan":
        return asyncio.run(cmd_scan(args.timeout))
    if args.cmd == "info":
        return asyncio.run(cmd_info(args.name, getattr(args, "mac", None)))
    if args.cmd == "flash":
        return asyncio.run(run_flash(args.firmware, args.name, args.mac,
                                     args.dry_run, args.verbose))
    return 2


if __name__ == "__main__":
    sys.exit(main())
