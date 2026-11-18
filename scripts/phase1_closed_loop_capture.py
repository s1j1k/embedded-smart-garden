#!/usr/bin/env python3
"""Phase 1 closed-loop capture for vegetative adaptive-P bring-up.

Logs Serial sensor/control lines, tracks in-band hold (both pH and TDS for
30 minutes), issues CYCLE only after settle when still out of band (same
control law; skips 2 h schedule wait). Hard stop at 6 h.
"""
from __future__ import annotations

import csv
import json
import os
import re
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

try:
    import serial
except ImportError:
    os.system(f"{sys.executable} -m pip install pyserial --quiet")
    import serial

SESSION = Path(
    Path(__file__).resolve().parents[1]
    / "docs"
    / ".active_characterization_session"
).read_text(encoding="utf-8").strip()
OUT = Path(SESSION) / "phase1"
OUT.mkdir(parents=True, exist_ok=True)

PORT = os.environ.get("GARDEN_PORT", "COM3")
BAUD = 115200
PH_MIN, PH_MAX = 5.5, 5.8
TDS_MIN, TDS_MAX = 800.0, 1200.0
HOLD_S = 30 * 60
TIMEOUT_S = 6 * 60 * 60
SAMPLE_CSV = OUT / "closed_loop_trace.csv"
EVENTS_CSV = OUT / "control_events_serial.csv"
METRICS_JSON = OUT / "metrics_summary.json"
RAW_LOG = OUT / "serial_raw.txt"
STATUS_JSON = OUT / "status.json"

SENSOR_RE = re.compile(
    r"pH Approx \(filtered\)\s*-\s*(?P<ph>[-0-9.]+)"
    r"|TDS Approx\s*-\s*(?P<tds>\d+)\s*ppm"
    r"|Water Temperature\s*-\s*(?P<temp>[-0-9.]+)\s*C"
    r"|Water Level\s*-\s*(?P<level>-?\d+)\s*%"
    r"|System Mode\s*-\s*(?P<mode>\S+)"
    r"|gain_ph_up[=:]\s*(?P<gup>[-0-9.]+)"
    r"|gain_ph_down[=:]\s*(?P<gdn>[-0-9.]+)"
    r"|gain_ec[=:]\s*(?P<gec>[-0-9.]+)",
    re.I,
)
# Compact machine line from Serial sensor dump
COMPACT_RE = re.compile(
    r"TDSppm:(?P<tds>\d+).*?pH:(?P<ph>[-0-9.]+)",
    re.I,
)
EVENT_RE = re.compile(
    r"\[(STAGE-CTRL|CONTROL|STAGE|DOSE|STOCK|WARN|PH-CAL|TDS)\]|"
    r"event=|CTRL |operator_dose|ph_low|ph_high|ec_low|in_band|dead_zone|"
    r"session_cap|baseline_|gain",
    re.I,
)
SETTLE_DONE_RE = re.compile(
    r"dose complete|waiting for mixing|settle",
    re.I,
)
BUSY_RE = re.compile(r"dose |Top-up |DOSING|Burst", re.I)


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def local_now() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def send(ser: serial.Serial, line: str) -> None:
    ser.write((line + "\n").encode("ascii", errors="ignore"))
    ser.flush()
    with RAW_LOG.open("a", encoding="utf-8") as f:
        f.write(f"{utc_now()} >>> {line}\n")


def main() -> int:
    fields = [
        "ts_utc",
        "uptime_hint_s",
        "ph",
        "tds_ppm",
        "water_temp_c",
        "water_level_pct",
        "mode",
        "gain_ph_up",
        "gain_ph_down",
        "gain_ec",
        "in_band_ph",
        "in_band_tds",
        "in_band_both",
        "hold_s",
    ]
    if not SAMPLE_CSV.exists():
        with SAMPLE_CSV.open("w", newline="", encoding="utf-8") as f:
            csv.DictWriter(f, fieldnames=fields).writeheader()
    if not EVENTS_CSV.exists():
        with EVENTS_CSV.open("w", newline="", encoding="utf-8") as f:
            csv.writer(f).writerow(["ts_utc", "line"])

    state = {
        "ph": None,
        "tds": None,
        "temp": None,
        "level": None,
        "mode": None,
        "gup": None,
        "gdn": None,
        "gec": None,
        "last_sample_mono": 0.0,
        "in_band_since": None,
        "first_in_band_ph": None,
        "first_in_band_tds": None,
        "max_ph_overshoot": 0.0,
        "max_tds_overshoot": 0.0,
        "approached_ph": False,
        "approached_tds": False,
        "doses": [],
        "gain_traj": [],
        "cycle_count": 0,
        "last_cycle_mono": 0.0,
        "settling": False,
        "start_mono": time.monotonic(),
        "ended": False,
        "end_reason": None,
    }

    ser = serial.Serial(PORT, BAUD, timeout=0.2)
    time.sleep(2.0)
    ser.reset_input_buffer()
    send(ser, "MODE VEGETATIVE")
    time.sleep(0.5)
    send(ser, "CYCLE")
    state["cycle_count"] += 1
    state["last_cycle_mono"] = time.monotonic()

    buf = ""
    try:
        while True:
            elapsed = time.monotonic() - state["start_mono"]
            if elapsed >= TIMEOUT_S:
                state["ended"] = True
                state["end_reason"] = "timeout_6h_no_convergence"
                break

            chunk = ser.read(4096).decode("utf-8", errors="replace")
            if chunk:
                buf += chunk
                with RAW_LOG.open("a", encoding="utf-8") as f:
                    f.write(chunk)

            while "\n" in buf:
                line, buf = buf.split("\n", 1)
                line = line.strip("\r")
                if not line:
                    continue

                if EVENT_RE.search(line):
                    with EVENTS_CSV.open("a", newline="", encoding="utf-8") as f:
                        csv.writer(f).writerow([utc_now(), line])
                    if SETTLE_DONE_RE.search(line):
                        state["settling"] = True
                    if re.search(r"STAGE-CTRL\].*dose", line, re.I):
                        state["settling"] = True
                        state["doses"].append({"ts": utc_now(), "line": line})

                for m in SENSOR_RE.finditer(line):
                    gd = {k: v for k, v in m.groupdict().items() if v is not None}
                    if "ph" in gd:
                        state["ph"] = float(gd["ph"])
                    if "tds" in gd:
                        state["tds"] = float(gd["tds"])
                    if "temp" in gd:
                        state["temp"] = float(gd["temp"])
                    if "level" in gd:
                        state["level"] = int(gd["level"])
                    if "mode" in gd:
                        state["mode"] = gd["mode"]
                    if "gup" in gd:
                        state["gup"] = float(gd["gup"])
                    if "gdn" in gd:
                        state["gdn"] = float(gd["gdn"])
                    if "gec" in gd:
                        state["gec"] = float(gd["gec"])

                cm = COMPACT_RE.search(line)
                if cm:
                    state["tds"] = float(cm.group("tds"))
                    state["ph"] = float(cm.group("ph"))

                # Emit a sample when we have a fresh SENSOR DATA block end
                if "Sensor read cycle complete" in line or line.startswith("====="):
                    if state["ph"] is None or state["tds"] is None:
                        continue
                    # debounce ~8 s
                    if time.monotonic() - state["last_sample_mono"] < 8.0:
                        continue
                    state["last_sample_mono"] = time.monotonic()
                    ph, tds = state["ph"], state["tds"]
                    in_ph = PH_MIN <= ph <= PH_MAX
                    in_tds = TDS_MIN <= tds <= TDS_MAX
                    both = in_ph and in_tds

                    if in_ph and state["first_in_band_ph"] is None:
                        state["first_in_band_ph"] = utc_now()
                        state["approached_ph"] = True
                    if in_tds and state["first_in_band_tds"] is None:
                        state["first_in_band_tds"] = utc_now()
                        state["approached_tds"] = True

                    # overshoot after first entry attempt (beyond band)
                    if state["approached_ph"] or ph > PH_MAX:
                        if ph > PH_MAX:
                            state["max_ph_overshoot"] = max(
                                state["max_ph_overshoot"], ph - PH_MAX
                            )
                            state["approached_ph"] = True
                    if state["approached_tds"] or tds > TDS_MAX:
                        if tds > TDS_MAX:
                            state["max_tds_overshoot"] = max(
                                state["max_tds_overshoot"], tds - TDS_MAX
                            )
                            state["approached_tds"] = True

                    if both:
                        if state["in_band_since"] is None:
                            state["in_band_since"] = time.monotonic()
                    else:
                        state["in_band_since"] = None

                    hold_s = (
                        time.monotonic() - state["in_band_since"]
                        if state["in_band_since"]
                        else 0.0
                    )

                    row = {
                        "ts_utc": utc_now(),
                        "uptime_hint_s": f"{elapsed:.0f}",
                        "ph": f"{ph:.3f}",
                        "tds_ppm": f"{tds:.0f}",
                        "water_temp_c": ""
                        if state["temp"] is None
                        else f"{state['temp']:.2f}",
                        "water_level_pct": ""
                        if state["level"] is None
                        else str(state["level"]),
                        "mode": state["mode"] or "",
                        "gain_ph_up": ""
                        if state["gup"] is None
                        else f"{state['gup']:.4f}",
                        "gain_ph_down": ""
                        if state["gdn"] is None
                        else f"{state['gdn']:.4f}",
                        "gain_ec": ""
                        if state["gec"] is None
                        else f"{state['gec']:.6f}",
                        "in_band_ph": str(in_ph).lower(),
                        "in_band_tds": str(in_tds).lower(),
                        "in_band_both": str(both).lower(),
                        "hold_s": f"{hold_s:.0f}",
                    }
                    with SAMPLE_CSV.open("a", newline="", encoding="utf-8") as f:
                        csv.DictWriter(f, fieldnames=fields).writerow(row)

                    if state["gup"] is not None:
                        state["gain_traj"].append(
                            {
                                "ts": utc_now(),
                                "gain_ph_up": state["gup"],
                                "gain_ph_down": state["gdn"],
                                "gain_ec": state["gec"],
                            }
                        )

                    STATUS_JSON.write_text(
                        json.dumps(
                            {
                                "phase": 1,
                                "local_time": local_now(),
                                "elapsed_s": round(elapsed),
                                "ph": ph,
                                "tds_ppm": tds,
                                "in_band_both": both,
                                "hold_s": round(hold_s),
                                "cycles": state["cycle_count"],
                                "end_reason": state["end_reason"],
                            },
                            indent=2,
                        ),
                        encoding="utf-8",
                    )

                    if both and hold_s >= HOLD_S:
                        state["ended"] = True
                        state["end_reason"] = "both_channels_in_band_30min"
                        break

                    # After settle, if out of band and idle-ish, request another CYCLE
                    # so the same adaptive-P law can act without waiting 2 h.
                    since_cycle = time.monotonic() - state["last_cycle_mono"]
                    if (
                        not both
                        and since_cycle > 20 * 60
                        and not BUSY_RE.search(line)
                    ):
                        send(ser, "CYCLE")
                        state["cycle_count"] += 1
                        state["last_cycle_mono"] = time.monotonic()
                        state["settling"] = False

            # also time-based CYCLE nudge every 25 min if still out of band
            if (
                state["ph"] is not None
                and state["tds"] is not None
                and not (
                    PH_MIN <= state["ph"] <= PH_MAX
                    and TDS_MIN <= state["tds"] <= TDS_MAX
                )
                and (time.monotonic() - state["last_cycle_mono"]) > 25 * 60
            ):
                send(ser, "CYCLE")
                state["cycle_count"] += 1
                state["last_cycle_mono"] = time.monotonic()

            if state["ended"]:
                break
            time.sleep(0.05)
    finally:
        try:
            ser.close()
        except Exception:
            pass

    metrics = {
        "end_reason": state["end_reason"],
        "ended_at": utc_now(),
        "elapsed_s": round(time.monotonic() - state["start_mono"]),
        "ph_band": [PH_MIN, PH_MAX],
        "tds_band": [TDS_MIN, TDS_MAX],
        "time_to_first_in_band_ph": state["first_in_band_ph"],
        "time_to_first_in_band_tds": state["first_in_band_tds"],
        "max_ph_overshoot_above_max": state["max_ph_overshoot"],
        "max_tds_overshoot_above_max": state["max_tds_overshoot"],
        "cycle_requests": state["cycle_count"],
        "dose_lines_seen": state["doses"],
        "gain_trajectory_tail": state["gain_traj"][-50:],
        "volume_ml_start": 8709.1,
        "water_cal_waived": True,
        "note": (
            "CYCLE used after ~20–25 min when still out of band to bypass "
            "2 h vegetative check interval; no manual DOSE in Phase 1."
        ),
    }
    METRICS_JSON.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    STATUS_JSON.write_text(
        json.dumps(
            {
                "phase": 1,
                "done": True,
                "end_reason": state["end_reason"],
                "local_time": local_now(),
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    print(json.dumps({"phase1_done": True, **metrics}, indent=2))
    if state["end_reason"] == "timeout_6h_no_convergence":
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
