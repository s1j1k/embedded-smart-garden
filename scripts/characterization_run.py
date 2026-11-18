#!/usr/bin/env python3
"""
Characterization master: Phase 1 closed-loop → Phase 2 open-loop bumps → wrap-up.

Uses Serial APIs: AUTO RESUME/PAUSE, CYCLE, DOSE, MODE, STOCK.
Does not trust water level. Volume from session ground-truth JSON.
"""
from __future__ import annotations

import csv
import json
import math
import os
import re
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

try:
    import serial
except ImportError:
    os.system(f"{sys.executable} -m pip install pyserial --quiet")
    import serial

ROOT = Path(__file__).resolve().parents[1]
SESSION = Path(
    (ROOT / "docs" / ".active_characterization_session").read_text(encoding="utf-8").strip()
)
P1 = SESSION / "phase1"
P2 = SESSION / "phase2"
P3 = SESSION / "phase3"
for d in (P1, P2, P3):
    d.mkdir(parents=True, exist_ok=True)

PORT = os.environ.get("GARDEN_PORT", "COM3")
BAUD = 115200
PH_MIN, PH_MAX = 5.5, 5.8
TDS_MIN, TDS_MAX = 800.0, 1200.0
TDS_DEAD_LOW, TDS_DEAD_HIGH = 750.0, 1250.0
HOLD_S = 30 * 60
TIMEOUT_S = 6 * 60 * 60
# After settle, nudge CYCLE so vegetative 2 h intervals don't stall the capture.
CYCLE_NUDGE_S = 20 * 60


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def local_now() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def open_serial() -> serial.Serial:
    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    time.sleep(0.4)
    ser.reset_input_buffer()
    return ser


def send(ser: serial.Serial, line: str, raw: Path | None = None) -> None:
    ser.write((line + "\n").encode("ascii", errors="ignore"))
    ser.flush()
    msg = f"{utc_now()} >>> {line}\n"
    print(msg.strip())
    if raw:
        with raw.open("a", encoding="utf-8") as f:
            f.write(msg)


def read_for(ser: serial.Serial, sec: float, raw: Path | None = None) -> str:
    end = time.monotonic() + sec
    buf = []
    while time.monotonic() < end:
        chunk = ser.read(4096)
        if chunk:
            text = chunk.decode("utf-8", errors="replace")
            buf.append(text)
            if raw:
                with raw.open("a", encoding="utf-8") as f:
                    f.write(text)
        else:
            time.sleep(0.05)
    return "".join(buf)


def parse_sensors(text: str) -> dict:
    out = {}
    m = re.search(r"pH Approx \(filtered\)\s*-\s*([-0-9.]+)", text)
    if m:
        out["ph"] = float(m.group(1))
    m = re.search(r"TDS Approx\s*-\s*(\d+)\s*ppm", text)
    if m:
        out["tds"] = float(m.group(1))
    m = re.search(r"Water Temperature\s*-\s*([-0-9.]+)\s*C", text)
    if m:
        out["temp"] = float(m.group(1))
    m = re.search(r"System Mode\s*-\s*(\S+)", text)
    if m:
        out["mode"] = m.group(1)
    m = re.search(r"TDSppm:(\d+).*?pH:([-0-9.]+)", text)
    if m:
        out["tds"] = float(m.group(1))
        out["ph"] = float(m.group(2))
    m = re.search(r"gain_ph_up[=:\s]+([-0-9.]+)", text, re.I)
    if m:
        out["gup"] = float(m.group(1))
    m = re.search(r"gain_ph_down[=:\s]+([-0-9.]+)", text, re.I)
    if m:
        out["gdn"] = float(m.group(1))
    m = re.search(r"gain_ec[=:\s]+([-0-9.]+)", text, re.I)
    if m:
        out["gec"] = float(m.group(1))
    # CTRL-LOG rows sometimes embed gains after recheck
    m = re.search(
        r"CTRL-LOG\].*gain[_\s]?up[=:\s]+([-0-9.]+).*gain[_\s]?down[=:\s]+([-0-9.]+)",
        text,
        re.I,
    )
    if m:
        out["gup"] = float(m.group(1))
        out["gdn"] = float(m.group(2))
    return out


def parse_stock(text: str) -> dict:
    stock = {}
    for name in ("water", "nutrient", "ph_up", "ph_down"):
        m = re.search(rf"{name}:\s*([-0-9.]+)\s*/\s*([-0-9.]+)\s*mL", text)
        if m:
            stock[name] = {"remaining_ml": float(m.group(1)), "capacity_ml": float(m.group(2))}
    return stock


def load_volume() -> float:
    p = SESSION / "phase0" / "ground_truth_volume_start.json"
    if p.exists():
        return float(json.loads(p.read_text(encoding="utf-8"))["volume_ml"])
    return 8709.1


# ---------------------------------------------------------------------------
# Phase 1
# ---------------------------------------------------------------------------
def run_phase1() -> dict:
    raw = P1 / "serial_raw_resume.txt"
    sample_csv = P1 / "closed_loop_trace.csv"
    events_csv = P1 / "control_events_serial.csv"
    status = P1 / "status.json"
    metrics_path = P1 / "metrics_summary.json"

    fields = [
        "ts_utc",
        "elapsed_s",
        "ph",
        "tds_ppm",
        "water_temp_c",
        "mode",
        "gain_ph_up",
        "gain_ph_down",
        "gain_ec",
        "in_band_ph",
        "in_band_tds_target",
        "in_band_tds_dead",
        "hold_s",
    ]
    with sample_csv.open("w", newline="", encoding="utf-8") as f:
        csv.DictWriter(f, fieldnames=fields).writeheader()
    with events_csv.open("w", newline="", encoding="utf-8") as f:
        csv.writer(f).writerow(["ts_utc", "line"])

    ser = open_serial()
    start = time.monotonic()
    state = {
        "ph": None,
        "tds": None,
        "temp": None,
        "mode": None,
        "gup": None,
        "gdn": None,
        "gec": None,
        "in_band_since": None,
        "first_ph": None,
        "first_tds": None,
        "max_ph_hi": 0.0,
        "max_tds_hi": 0.0,
        "doses": [],
        "gains": [],
        "cycles": 0,
        "last_cycle": 0.0,
        "last_sample": 0.0,
        "end_reason": None,
        "tds_relaxed": False,
    }
    try:
        send(ser, "AUTO RESUME", raw)
        read_for(ser, 2, raw)
        send(ser, "MODE VEGETATIVE", raw)
        read_for(ser, 2, raw)
        send(ser, "CYCLE", raw)
        state["cycles"] = 1
        state["last_cycle"] = time.monotonic()
        read_for(ser, 3, raw)

        line_buf = ""
        while True:
            elapsed = time.monotonic() - start
            if elapsed >= TIMEOUT_S:
                state["end_reason"] = "timeout_6h_no_convergence"
                break

            chunk = ser.read(4096).decode("utf-8", errors="replace")
            if chunk:
                line_buf += chunk
                with raw.open("a", encoding="utf-8") as f:
                    f.write(chunk)

            while "\n" in line_buf:
                line, line_buf = line_buf.split("\n", 1)
                line = line.strip("\r")
                if not line:
                    continue
                if re.search(
                    r"STAGE-CTRL|CTRL-LOG|DOSE|AUTO|gain|dose complete|ec_check|ph_check|ph_high|ph_low|ec_low|dead_zone|in_band|session_cap",
                    line,
                    re.I,
                ):
                    with events_csv.open("a", newline="", encoding="utf-8") as f:
                        csv.writer(f).writerow([utc_now(), line])
                    if re.search(r"STAGE-CTRL\].*dose", line, re.I):
                        state["doses"].append({"ts": utc_now(), "line": line})

                parsed = parse_sensors(line + "\n")
                state.update({k: v for k, v in parsed.items() if v is not None})
                # map short keys
                if "ph" in parsed:
                    state["ph"] = parsed["ph"]
                if "tds" in parsed:
                    state["tds"] = parsed["tds"]
                if "temp" in parsed:
                    state["temp"] = parsed["temp"]
                if "mode" in parsed:
                    state["mode"] = parsed["mode"]
                if "gup" in parsed:
                    state["gup"] = parsed["gup"]
                if "gdn" in parsed:
                    state["gdn"] = parsed["gdn"]
                if "gec" in parsed:
                    state["gec"] = parsed["gec"]

                if "Sensor read cycle complete" in line or "========== SENSOR DATA" in line:
                    if state["ph"] is None or state["tds"] is None:
                        continue
                    if time.monotonic() - state["last_sample"] < 8:
                        continue
                    state["last_sample"] = time.monotonic()
                    ph, tds = state["ph"], state["tds"]
                    in_ph = PH_MIN <= ph <= PH_MAX
                    in_tds_t = TDS_MIN <= tds <= TDS_MAX
                    in_tds_d = TDS_DEAD_LOW <= tds <= TDS_DEAD_HIGH
                    # Hold: pH in target; TDS in target if possible, else dead zone
                    # (session started with 5V-off overdose → often stuck ~1240).
                    both = in_ph and in_tds_t
                    both_relaxed = in_ph and in_tds_d
                    if in_ph and state["first_ph"] is None:
                        state["first_ph"] = utc_now()
                    if in_tds_t and state["first_tds"] is None:
                        state["first_tds"] = utc_now()
                    if ph > PH_MAX:
                        state["max_ph_hi"] = max(state["max_ph_hi"], ph - PH_MAX)
                    if tds > TDS_MAX:
                        state["max_tds_hi"] = max(state["max_tds_hi"], tds - TDS_MAX)

                    use_hold = both or (
                        both_relaxed and elapsed > 45 * 60
                    )  # allow dead-zone hold after 45 min if target TDS unreachable
                    if use_hold:
                        if state["in_band_since"] is None:
                            state["in_band_since"] = time.monotonic()
                            state["tds_relaxed"] = not both and both_relaxed
                    else:
                        state["in_band_since"] = None
                        state["tds_relaxed"] = False

                    hold_s = (
                        time.monotonic() - state["in_band_since"]
                        if state["in_band_since"]
                        else 0.0
                    )
                    row = {
                        "ts_utc": utc_now(),
                        "elapsed_s": f"{elapsed:.0f}",
                        "ph": f"{ph:.3f}",
                        "tds_ppm": f"{tds:.0f}",
                        "water_temp_c": ""
                        if state["temp"] is None
                        else f"{state['temp']:.2f}",
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
                        "in_band_tds_target": str(in_tds_t).lower(),
                        "in_band_tds_dead": str(in_tds_d).lower(),
                        "hold_s": f"{hold_s:.0f}",
                    }
                    with sample_csv.open("a", newline="", encoding="utf-8") as f:
                        csv.DictWriter(f, fieldnames=fields).writerow(row)
                    if state["gup"] is not None:
                        state["gains"].append(
                            {
                                "ts": utc_now(),
                                "gain_ph_up": state["gup"],
                                "gain_ph_down": state["gdn"],
                                "gain_ec": state["gec"],
                            }
                        )
                    status.write_text(
                        json.dumps(
                            {
                                "phase": 1,
                                "local_time": local_now(),
                                "elapsed_s": round(elapsed),
                                "ph": ph,
                                "tds_ppm": tds,
                                "hold_s": round(hold_s),
                                "cycles": state["cycles"],
                                "tds_relaxed_hold": state["tds_relaxed"],
                                "end_reason": None,
                            },
                            indent=2,
                        ),
                        encoding="utf-8",
                    )
                    if hold_s >= HOLD_S:
                        state["end_reason"] = (
                            "both_in_target_30min"
                            if both
                            else "ph_target_and_tds_deadzone_30min"
                        )
                        break

            # CYCLE nudge when out of hold window
            if (
                state["ph"] is not None
                and state["in_band_since"] is None
                and (time.monotonic() - state["last_cycle"]) > CYCLE_NUDGE_S
            ):
                send(ser, "CYCLE", raw)
                state["cycles"] += 1
                state["last_cycle"] = time.monotonic()

            if state["end_reason"]:
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
        "elapsed_s": round(time.monotonic() - start),
        "ph_band": [PH_MIN, PH_MAX],
        "tds_band": [TDS_MIN, TDS_MAX],
        "tds_dead": [TDS_DEAD_LOW, TDS_DEAD_HIGH],
        "time_to_first_in_band_ph": state["first_ph"],
        "time_to_first_in_band_tds_target": state["first_tds"],
        "max_ph_overshoot_above_max": state["max_ph_hi"],
        "max_tds_overshoot_above_max": state["max_tds_hi"],
        "cycle_requests": state["cycles"],
        "dose_lines": state["doses"],
        "gain_trajectory_tail": state["gains"][-80:],
        "tds_hold_relaxed_to_dead_zone": state["tds_relaxed"],
        "volume_ml": load_volume(),
        "final_ph": state["ph"],
        "final_tds": state["tds"],
    }
    metrics_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")
    status.write_text(
        json.dumps(
            {"phase": 1, "done": True, "end_reason": state["end_reason"], **metrics},
            indent=2,
        ),
        encoding="utf-8",
    )
    print("PHASE1_DONE", state["end_reason"])
    if state["end_reason"] == "timeout_6h_no_convergence":
        print("TIMEOUT_HIT_WITHOUT_CONVERGENCE")
    return metrics


# ---------------------------------------------------------------------------
# Phase 2 bump tests
# ---------------------------------------------------------------------------
@dataclass
class Bump:
    name: str
    pump: str
    ml: float
    settle_max_s: float
    log_ph: bool = True
    log_tds: bool = True


def wait_settled(
    ser: serial.Serial,
    raw: Path,
    csv_path: Path,
    max_s: float,
    roc_ph: float = 0.02,
    roc_tds: float = 8.0,
    stable_n: int = 6,
    interval_s: float = 20.0,
) -> tuple[float, list[dict]]:
    """Sample every interval_s until RoC small for stable_n samples or max_s."""
    fields = ["ts_utc", "t_s", "ph", "tds_ppm", "water_temp_c"]
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        csv.DictWriter(f, fieldnames=fields).writeheader()
    rows: list[dict] = []
    t0 = time.monotonic()
    stable = 0
    last_ph = last_tds = None
    while time.monotonic() - t0 < max_s:
        text = read_for(ser, interval_s, raw)
        p = parse_sensors(text)
        if "ph" not in p and "tds" not in p:
            continue
        t_s = time.monotonic() - t0
        row = {
            "ts_utc": utc_now(),
            "t_s": f"{t_s:.1f}",
            "ph": "" if "ph" not in p else f"{p['ph']:.3f}",
            "tds_ppm": "" if "tds" not in p else f"{p['tds']:.0f}",
            "water_temp_c": "" if "temp" not in p else f"{p['temp']:.2f}",
        }
        with csv_path.open("a", newline="", encoding="utf-8") as f:
            csv.DictWriter(f, fieldnames=fields).writerow(row)
        rows.append({"t_s": t_s, **p})
        ok = True
        if last_ph is not None and "ph" in p:
            if abs(p["ph"] - last_ph) > roc_ph:
                ok = False
        if last_tds is not None and "tds" in p:
            if abs(p["tds"] - last_tds) > roc_tds:
                ok = False
        if last_ph is None:
            ok = False
        stable = stable + 1 if ok else 0
        last_ph = p.get("ph", last_ph)
        last_tds = p.get("tds", last_tds)
        if stable >= stable_n:
            break
    return time.monotonic() - t0, rows


def fit_fopdt(rows: list[dict], key: str, dose_ml: float) -> dict | None:
    """Rough FOPDT from step: y(t) after dose at t≈0. Uses 10–90% rise."""
    ys = [(r["t_s"], r[key]) for r in rows if key in r]
    if len(ys) < 8 or dose_ml <= 0:
        return None
    y0 = ys[0][1]
    yinf = sum(v for _, v in ys[-4:]) / 4.0
    dy = yinf - y0
    if abs(dy) < 1e-6:
        return {"K": 0.0, "theta_s": None, "tau_s": None, "y0": y0, "yinf": yinf}
    K = dy / dose_ml
    target10 = y0 + 0.1 * dy
    target90 = y0 + 0.9 * dy
    t10 = t90 = None
    for t, y in ys:
        if t10 is None and ((dy > 0 and y >= target10) or (dy < 0 and y <= target10)):
            t10 = t
        if t90 is None and ((dy > 0 and y >= target90) or (dy < 0 and y <= target90)):
            t90 = t
            break
    if t10 is None or t90 is None or t90 <= t10:
        return {"K": K, "theta_s": None, "tau_s": None, "y0": y0, "yinf": yinf}
    # Approx: t10≈θ+0.1τ, t90≈θ+2.3τ → τ=(t90-t10)/2.2, θ=t10-0.1τ
    tau = (t90 - t10) / 2.2
    theta = max(0.0, t10 - 0.1 * tau)
    return {
        "K": K,
        "theta_s": theta,
        "tau_s": tau,
        "y0": y0,
        "yinf": yinf,
        "t10_s": t10,
        "t90_s": t90,
    }


def run_phase2(phase1_metrics: dict) -> dict:
    raw = P2 / "serial_raw.txt"
    manifest_path = P2 / "manifest.json"
    ser = open_serial()
    manifest = {
        "started_at": utc_now(),
        "phase1_end": phase1_metrics.get("end_reason"),
        "tests": [],
        "stock_start": {},
        "stock_end": {},
    }
    try:
        send(ser, "AUTO PAUSE", raw)
        read_for(ser, 2, raw)
        send(ser, "STOCK", raw)
        manifest["stock_start"] = parse_stock(read_for(ser, 3, raw))

        # Baseline settle snapshot
        base_csv = P2 / "00_baseline.csv"
        wait_settled(ser, raw, base_csv, max_s=180, stable_n=4, interval_s=15)

        bumps = [
            Bump("ph_up_1", "ph_up", 0.15, 90 * 60),
            Bump("ph_up_2", "ph_up", 0.30, 90 * 60),
            Bump("ph_down_1", "ph_down", 0.15, 90 * 60),
            Bump("ph_down_2", "ph_down", 0.30, 90 * 60),
            Bump("nutrient_1", "nutrient", 0.40, 120 * 60),
            Bump("nutrient_2", "nutrient", 0.80, 120 * 60),
            Bump("nutrient_cross_ph", "nutrient", 0.40, 120 * 60),  # log both
        ]

        for i, b in enumerate(bumps, start=1):
            print(f"BUMP {b.name} DOSE {b.pump} {b.ml} mL")
            dose_ts = utc_now()
            send(ser, f"DOSE {b.pump.upper()} {b.ml}", raw)
            ack = read_for(ser, 8, raw)
            csv_path = P2 / f"{i:02d}_{b.name}.csv"
            settle_s, rows = wait_settled(
                ser,
                raw,
                csv_path,
                max_s=b.settle_max_s,
                interval_s=20.0,
            )
            key = "tds" if b.pump == "nutrient" and b.name != "nutrient_cross_ph" else "ph"
            if b.name == "nutrient_cross_ph":
                fit_ph = fit_fopdt(rows, "ph", b.ml)
                fit_tds = fit_fopdt(rows, "tds", b.ml)
                fit = {"ph": fit_ph, "tds": fit_tds}
            elif b.pump == "nutrient":
                fit = fit_fopdt(rows, "tds", b.ml)
            else:
                fit = fit_fopdt(rows, "ph", b.ml)
            entry = {
                "name": b.name,
                "pump": b.pump,
                "dose_ml_requested": b.ml,
                "dose_cmd_ts": dose_ts,
                "ack_excerpt": "\n".join(
                    x for x in ack.splitlines() if "DOSE" in x or "STAGE-CTRL" in x
                )[:500],
                "settle_s": settle_s,
                "settle_ts": utc_now(),
                "csv": str(csv_path.name),
                "fopdt": fit,
            }
            manifest["tests"].append(entry)
            manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

        send(ser, "STOCK", raw)
        manifest["stock_end"] = parse_stock(read_for(ser, 3, raw))
        # Leave auto running if near band (Phase 3 decision)
        send(ser, "AUTO RESUME", raw)
        read_for(ser, 2, raw)
    finally:
        try:
            ser.close()
        except Exception:
            pass
    manifest["ended_at"] = utc_now()
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print("PHASE2_DONE")
    return manifest


# ---------------------------------------------------------------------------
# Plots + Influx cross-check + wrap-up
# ---------------------------------------------------------------------------
def make_plots(phase1_metrics: dict, phase2_manifest: dict) -> None:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        os.system(f"{sys.executable} -m pip install matplotlib --quiet")
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

    # Phase 1 traces
    rows = list(csv.DictReader((P1 / "closed_loop_trace.csv").open(encoding="utf-8")))
    if rows:
        t = [i for i in range(len(rows))]
        ph = [float(r["ph"]) for r in rows]
        tds = [float(r["tds_ppm"]) for r in rows]
        fig, ax = plt.subplots(figsize=(10, 4))
        ax.plot(t, ph, label="pH")
        ax.axhspan(PH_MIN, PH_MAX, alpha=0.15, color="green", label="pH target")
        ax.axhspan(5.4, 5.9, alpha=0.08, color="gray", label="pH dead")
        ax.set_xlabel("sample #")
        ax.set_ylabel("pH")
        ax.legend()
        ax.set_title("Phase 1 pH vs time")
        fig.tight_layout()
        fig.savefig(P1 / "plot_ph.png", dpi=120)
        plt.close(fig)

        fig, ax = plt.subplots(figsize=(10, 4))
        ax.plot(t, tds, label="TDS")
        ax.axhspan(TDS_MIN, TDS_MAX, alpha=0.15, color="green", label="TDS target")
        ax.axhspan(TDS_DEAD_LOW, TDS_DEAD_HIGH, alpha=0.08, color="gray", label="TDS dead")
        ax.set_xlabel("sample #")
        ax.set_ylabel("TDS ppm")
        ax.legend()
        ax.set_title("Phase 1 TDS vs time")
        fig.tight_layout()
        fig.savefig(P1 / "plot_tds.png", dpi=120)
        plt.close(fig)

        gup = [r["gain_ph_up"] for r in rows if r.get("gain_ph_up")]
        if gup:
            fig, ax = plt.subplots(figsize=(10, 4))
            ax.plot([float(x) for x in gup], label="gain_ph_up")
            gdn = [float(r["gain_ph_down"]) for r in rows if r.get("gain_ph_down")]
            if gdn:
                ax.plot(gdn, label="gain_ph_down")
            ax.legend()
            ax.set_title("Adaptive gains (from serial when present)")
            fig.tight_layout()
            fig.savefig(P1 / "plot_gains.png", dpi=120)
            plt.close(fig)

    for test in phase2_manifest.get("tests", []):
        csv_name = test.get("csv")
        if not csv_name:
            continue
        path = P2 / csv_name
        if not path.exists():
            continue
        brows = list(csv.DictReader(path.open(encoding="utf-8")))
        if not brows:
            continue
        ts = [float(r["t_s"]) for r in brows if r.get("t_s")]
        fig, ax = plt.subplots(figsize=(8, 4))
        if any(r.get("ph") for r in brows):
            ax.plot(
                ts,
                [float(r["ph"]) for r in brows if r.get("ph")],
                label="pH",
            )
        ax2 = ax.twinx()
        if any(r.get("tds_ppm") for r in brows):
            ax2.plot(
                [float(r["t_s"]) for r in brows if r.get("tds_ppm")],
                [float(r["tds_ppm"]) for r in brows if r.get("tds_ppm")],
                color="orange",
                label="TDS",
            )
        ax.set_title(f"Bump {test['name']} ({test['pump']} {test['dose_ml_requested']} mL)")
        ax.set_xlabel("t (s)")
        fig.tight_layout()
        fig.savefig(P2 / f"plot_{test['name']}.png", dpi=120)
        plt.close(fig)


def influx_crosscheck(phase1_metrics: dict) -> dict:
    """Pull Influx sensor_reading for the Phase 1 window; flag vs local CSV."""
    secrets = (ROOT / "secrets.h").read_text(encoding="utf-8", errors="ignore")

    def grab(name: str) -> str | None:
        m = re.search(rf'{name}\s*=\s*"([^"]+)"', secrets)
        return m.group(1) if m else None

    url = grab("INFLUX_URL")
    org = grab("INFLUX_ORG")
    bucket = grab("INFLUX_BUCKET")
    token = grab("INFLUX_TOKEN")
    if not all([url, org, bucket, token]):
        return {"ok": False, "error": "missing_influx_secrets"}
    try:
        import urllib.request
    except ImportError:
        return {"ok": False, "error": "urllib_missing"}

    # Last N hours covering the run
    elapsed = int(phase1_metrics.get("elapsed_s") or 3600) + 600
    flux = f'''
from(bucket: "{bucket}")
  |> range(start: -{elapsed}s)
  |> filter(fn: (r) => r._measurement == "sensor_reading")
  |> filter(fn: (r) => r.device_id == "garden-node-01")
  |> filter(fn: (r) => r._field == "ph" or r._field == "tds_ppm" or r._field == "gain_ph_up" or r._field == "gain_ph_down" or r._field == "gain_ec")
  |> pivot(rowKey:["_time"], columnKey: ["_field"], valueColumn: "_value")
'''
    req = urllib.request.Request(
        f"{url}/api/v2/query?org={org}",
        data=flux.encode("utf-8"),
        headers={
            "Authorization": f"Token {token}",
            "Content-Type": "application/vnd.flux",
            "Accept": "application/csv",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            body = resp.read().decode("utf-8", errors="replace")
        (P1 / "influx_sensor_reading.csv").write_text(body, encoding="utf-8")
        local_n = sum(1 for _ in (P1 / "closed_loop_trace.csv").open(encoding="utf-8")) - 1
        influx_n = body.count("\n") - body.count("#")
        discrepancy = None
        if local_n > 5 and influx_n < local_n * 0.3:
            discrepancy = f"influx_rows_low local={local_n} influx_lines≈{influx_n}"
        return {
            "ok": True,
            "local_samples": local_n,
            "influx_csv_lines": influx_n,
            "discrepancy": discrepancy,
            "path": str(P1 / "influx_sensor_reading.csv"),
        }
    except Exception as e:
        return {"ok": False, "error": str(e)}


def wrapup(phase1_metrics: dict, phase2_manifest: dict, influx: dict) -> None:
    ser = open_serial()
    raw = P3 / "serial_raw.txt"
    try:
        send(ser, "STOCK", raw)
        stock = parse_stock(read_for(ser, 3, raw))
        text = read_for(ser, 15, raw)
        sens = parse_sensors(text)
        send(ser, "AUTO", raw)
        auto = read_for(ser, 2, raw)
    finally:
        ser.close()

    # Reagent consumed from phase2 stock delta if present
    consumed = {}
    ss, se = phase2_manifest.get("stock_start") or {}, phase2_manifest.get("stock_end") or {}
    for k in ("nutrient", "ph_up", "ph_down", "water"):
        if k in ss and k in se:
            consumed[k] = round(
                ss[k]["remaining_ml"] - se[k]["remaining_ml"], 3
            )

    summary = {
        "ended_at": utc_now(),
        "final_sensors": sens,
        "stock_now": stock,
        "reagent_consumed_ml_phase2_stock_delta": consumed,
        "phase1": phase1_metrics,
        "phase2_tests": len(phase2_manifest.get("tests", [])),
        "influx_crosscheck": influx,
        "volume_ml_start": load_volume(),
        "volume_ml_end": None,  # operator prompt
        "auto_status_excerpt": "\n".join(
            x for x in auto.splitlines() if "AUTO" in x
        ),
        "recommendation": None,
    }
    ph = sens.get("ph")
    tds = sens.get("tds")
    if ph is not None and tds is not None:
        in_ph = PH_MIN <= ph <= PH_MAX
        in_tds = TDS_MIN <= tds <= TDS_MAX
        if in_ph and in_tds:
            summary["recommendation"] = "leave_auto_running_plant_ready"
            # ensure resume
            ser = open_serial()
            try:
                send(ser, "AUTO RESUME", raw)
                read_for(ser, 2, raw)
            finally:
                ser.close()
        else:
            summary["recommendation"] = (
                f"out_of_band ph={ph} tds={tds} — report to operator; "
                "auto left resumed for gentle correction unless paused"
            )
            ser = open_serial()
            try:
                send(ser, "AUTO RESUME", raw)
                read_for(ser, 2, raw)
            finally:
                ser.close()
    (P3 / "wrapup_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print("WRAPUP", json.dumps(summary, indent=2))


def main() -> int:
    print("SESSION", SESSION)
    print("Resuming Phase 1…")
    p1 = run_phase1()
    if p1.get("end_reason") == "timeout_6h_no_convergence":
        print("ALERT: Phase 1 hit 6 h timeout without convergence")
    print("Starting Phase 2 bump tests…")
    p2 = run_phase2(p1)
    print("Plots + Influx cross-check…")
    make_plots(p1, p2)
    influx = influx_crosscheck(p1)
    (P1 / "influx_crosscheck.json").write_text(json.dumps(influx, indent=2), encoding="utf-8")
    wrapup(p1, p2, influx)
    print("ALL_DONE", SESSION)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
