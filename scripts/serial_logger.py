#!/usr/bin/env python3
"""Log everything the ESP32 prints over USB serial, with timestamps, for a run.

Each line is written as  <UTC ISO time>\t<line>  to --out (appended, flushed
every line). The port is opened with DTR/RTS low so the board is not reset, and
reopened if the cable drops. Commands can be sent while logging: append a line
to the --inbox file (e.g. `echo CYCLE >> inbox.txt`); each new line is written
to the board and logged as ">>> CMD".

    python scripts/serial_logger.py --out docs/run/serial.log --inbox docs/run/inbox.txt
"""

from __future__ import annotations

import argparse
import time
from datetime import datetime, timezone
from pathlib import Path

import serial


def stamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def open_port(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = 0.2
    ser.dtr = False  # no auto-reset
    ser.rts = False
    ser.open()
    return ser


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True)
    ap.add_argument("--inbox")
    args = ap.parse_args()

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    inbox = Path(args.inbox) if args.inbox else None
    if inbox:
        inbox.touch()
    sent = inbox.stat().st_size if inbox else 0

    with out_path.open("a", encoding="utf-8") as out:
        def log(text: str) -> None:
            out.write(f"{stamp()}\t{text}\n")
            out.flush()

        log(f"### serial_logger start port={args.port} baud={args.baud}")
        ser = None
        buf = b""
        while True:
            if ser is None:
                try:
                    ser = open_port(args.port, args.baud)
                    log("### port open")
                except serial.SerialException as exc:
                    log(f"### port unavailable: {exc}")
                    time.sleep(5)
                    continue
            try:
                buf += ser.read(4096)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    log(line.decode("utf-8", "replace").rstrip("\r"))
                if inbox and inbox.stat().st_size > sent:
                    with inbox.open("rb") as f:
                        f.seek(sent)
                        new = f.read()
                    sent += len(new)
                    for cmd in new.decode("utf-8", "replace").splitlines():
                        if cmd.strip():
                            log(f">>> {cmd.strip()}")
                            ser.write((cmd.strip() + "\n").encode())
            except (serial.SerialException, OSError) as exc:
                log(f"### port lost: {exc}")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                time.sleep(2)


if __name__ == "__main__":
    main()
