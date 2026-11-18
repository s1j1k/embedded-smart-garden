"""Mixing time of each dose in a bring-up run, for comparing aeration setups.

    python mixing_time.py <run dir> [<run dir> ...]  ->  <run dir>/mixing_time.csv and a summary

A run dir is what scripts/export_bringup_run.py writes (sensor_readings.csv,
actuator_events.csv). For each dose:

  step      final - before, where before is the median reading in the 2 min before
            the dose, and final the median of the last 3 min of the window. The
            window runs from the dose to the next dose, at most 20 min.
  t95_raw   minutes from the dose until the raw 10 s reading stays within 5 % of
            the step of its final value (or within twice the pre-dose noise, if
            that is larger): the tank itself.
  t95_ctrl  the same for the filtered control value (ph_ema / tds_ppm_ema): what
            the controller sees.
  recheck   minutes from the dose to the controller's recheck (its settle detection).

The 5 % band is a ratio of the step, so a linear calibration error (the 16 Oct pH
probe line) does not change t95. Doses whose window is shorter than 6 min, or
whose step is smaller than 3x the noise, are listed but not timed.
"""
import csv
import statistics as st
import sys
from datetime import datetime, timedelta
from pathlib import Path

WINDOW = timedelta(minutes=20)
PRE = timedelta(minutes=2)
TAIL = timedelta(minutes=3)
COLUMNS = {"nutrient": ("tds_ppm", "tds_ppm_ema"), "ph_up": ("ph", "ph_ema"),
           "ph_down": ("ph", "ph_ema"), "water": ("water_level_pct", "water_level_pct")}


def ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00"))


def num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def t95(rows, col, t0, before, final, noise):
    step = final - before
    band = max(0.05 * abs(step), 2 * noise)
    last_out = None
    for t, r in rows:
        v = num(r.get(col))
        if v is None:
            continue
        if abs(v - final) > band:
            last_out = t
    if last_out is None:
        return 0.0
    return (last_out - t0).total_seconds() / 60 + 10 / 60  # settled from the next sample


def analyse(run):
    run = Path(run)
    readings = [(ts(r["time"]), r) for r in csv.DictReader(open(run / "sensor_readings.csv"))]
    doses = list(csv.DictReader(open(run / "actuator_events.csv")))
    starts = [ts(d["time"]) for d in doses]
    out = []
    for i, d in enumerate(doses):
        t0 = starts[i]
        end = min([s for s in starts if s > t0] + [t0 + WINDOW])
        raw_col, ctrl_col = COLUMNS.get(d["pump"], (None, None))
        row = {"time": d["time"], "pump": d["pump"], "ml": d["delivered_ml"],
               "window_min": round((end - t0).total_seconds() / 60, 1)}
        if d.get("recheck_time"):
            row["recheck_min"] = round((ts(d["recheck_time"]) - t0).total_seconds() / 60, 1)
        if raw_col is None:
            out.append(row)
            continue
        pre = [num(r[raw_col]) for t, r in readings if t0 - PRE <= t < t0 and num(r.get(raw_col)) is not None]
        win = [(t, r) for t, r in readings if t0 <= t < end]
        tail = [num(r[raw_col]) for t, r in win if t >= end - TAIL and num(r.get(raw_col)) is not None]
        if len(pre) < 3 or len(tail) < 3 or end - t0 < timedelta(minutes=6):
            row["note"] = "window too short"
            out.append(row)
            continue
        before, final = st.median(pre), st.median(tail)
        noise = st.pstdev(pre)
        row.update(before=round(before, 3), final=round(final, 3), step=round(final - before, 3))
        if abs(final - before) < 3 * max(noise, 1e-6):
            row["note"] = "step within noise"
            out.append(row)
            continue
        row["t95_raw_min"] = round(t95(win, raw_col, t0, before, final, noise), 1)
        ctrl_pre = [num(r[ctrl_col]) for t, r in readings if t0 - PRE <= t < t0 and num(r.get(ctrl_col)) is not None]
        ctrl_tail = [num(r[ctrl_col]) for t, r in win if t >= end - TAIL and num(r.get(ctrl_col)) is not None]
        if len(ctrl_pre) >= 3 and len(ctrl_tail) >= 3:
            row["t95_ctrl_min"] = round(t95(win, ctrl_col, t0, st.median(ctrl_pre), st.median(ctrl_tail),
                                            st.pstdev(ctrl_pre)), 1)
        out.append(row)
    keys = ["time", "pump", "ml", "before", "final", "step", "t95_raw_min", "t95_ctrl_min",
            "recheck_min", "window_min", "note"]
    with open(run / "mixing_time.csv", "w", newline="") as f:
        w = csv.DictWriter(f, keys)
        w.writeheader()
        for r in out:
            w.writerow({k: r.get(k, "") for k in keys})
    print(f"== {run.name}")
    for r in out:
        print(f"  {r['time'][11:16]} {r['pump']:9s} {float(r['ml']):6.2f} mL  step {r.get('step', ''):>8}  "
              f"t95 raw {r.get('t95_raw_min', '-'):>5}  ctrl {r.get('t95_ctrl_min', '-'):>5}  "
              f"recheck {r.get('recheck_min', '-'):>5} min  {r.get('note', '')}")


for arg in sys.argv[1:]:
    analyse(arg)
