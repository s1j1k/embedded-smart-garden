#!/usr/bin/env python3
"""Copy a LittleFS log off the ESP32 over USB serial, verified.

Firmware 1.8.0+ streams DUMP output a few lines per loop pass (pumps keep
running) and frames each line as  ~<line no>:<crc16>:<text>  because log
lines can land between them. This script checks every line number and CRC,
asks again from the first gap (DUMP ... FROM n), and writes the file only
when every line from 1 to the last arrived intact.

The port is opened with DTR/RTS low so the board is not reset.

    python scripts/serial_dump.py sensors out/sensor_readings.csv
    python scripts/serial_dump.py ctrl out/control_events.csv --port COM3
    python scripts/serial_dump.py prev out/sensor_readings_prev.csv
"""

from __future__ import annotations

import argparse
import re
import sys
import time
import zlib
from pathlib import Path

import serial

FILES = {"sensors": "SENSORS", "ctrl": "CTRL", "prev": "PREV", "outbox": "OUTBOX"}
FRAME = re.compile(r"^~(\d+):([0-9a-f]{4}):(.*)$")
END = re.compile(r"\[DUMP\] END (\S+) lines=(\d+) last=(\d+) crc32=([0-9a-f]{8})")


def crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def open_port(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False  # no auto-reset
    ser.rts = False
    ser.open()
    time.sleep(0.3)
    ser.reset_input_buffer()
    return ser


def run_dump(ser: serial.Serial, which: str, from_line: int, timeout_s: float):
    """One DUMP pass. Returns ({line_no: text}, end_info or None, bad_count)."""
    ser.write(b"\n")  # clear any partial command on the device
    time.sleep(0.2)
    ser.reset_input_buffer()
    ser.write(f"DUMP {which} FROM {from_line}\n".encode())
    lines: dict[int, str] = {}
    bad = 0
    buf = b""
    began = False
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        chunk = ser.read(8192)
        if not chunk:
            continue
        deadline = time.monotonic() + timeout_s  # still streaming
        buf += chunk
        *complete, buf = buf.split(b"\n")
        for raw in complete:
            text = raw.decode("utf-8", "replace").rstrip("\r")
            if text.startswith("[DUMP] BEGIN"):
                began = True
                continue
            if text.startswith("[DUMP] no file") or text.startswith("[DUMP] open failed"):
                raise SystemExit(text)
            m = END.search(text)
            if m and began:
                return lines, {"emitted": int(m.group(2)), "last": int(m.group(3)),
                               "crc32": m.group(4)}, bad
            m = FRAME.match(text)
            if not m or not began:
                continue
            no, crc, body = int(m.group(1)), int(m.group(2), 16), m.group(3)
            if crc16_ccitt(body.encode("utf-8")) == crc:
                lines[no] = body
            else:
                bad += 1
    return lines, None, bad


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("file", choices=sorted(FILES))
    ap.add_argument("out", type=Path)
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--retries", type=int, default=5)
    ap.add_argument("--idle-timeout", type=float, default=20.0,
                    help="give up a pass after this many seconds without data")
    args = ap.parse_args()

    ser = open_port(args.port, args.baud)
    got: dict[int, str] = {}
    last = None
    start = 1
    try:
        for attempt in range(1, args.retries + 1):
            lines, end, bad = run_dump(ser, FILES[args.file], start, args.idle_timeout)
            got.update(lines)
            if end is not None:
                last = end["last"] if last is None else max(last, end["last"])
            missing = [n for n in range(1, (last or 0) + 1) if n not in got]
            print(f"pass {attempt}: from {start}, {len(lines)} lines ok, {bad} bad CRC, "
                  f"last={last}, missing={len(missing)}")
            if end is not None and not missing:
                break
            start = missing[0] if missing else (max(got) + 1 if got else 1)
        else:
            print("gave up with lines still missing", file=sys.stderr)
            return 1
    finally:
        ser.close()

    body = "".join(got[n] + "\n" for n in range(1, last + 1))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(body, encoding="utf-8")
    print(f"wrote {last} lines to {args.out} (crc32 {zlib.crc32(body.encode()):08x})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
