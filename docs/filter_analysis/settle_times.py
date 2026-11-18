"""Settling time of every dose in the 30 Sep - 5 Oct run, per input filter.

For each pump start, the burst readings around it are replayed through three
control inputs - the raw burst reading, the old 3 min EMA, and the 2 mHz
Butterworth now in firmware 1.9 - with pump-on samples held as on the ESP32.
The settling time is how long after the pump starts each input stays within a
tolerance of the final value (median raw reading 25-40 min after the start).
Linear drift measured before the dose is removed first.

Tolerance: 10 % of the step or the noise floor (2 ppm, 0.03 pH), whichever is
larger. Steps smaller than 3 ppm or 0.1 pH are reported but not timed: they
are within noise and drift.

Run:  python docs/filter_analysis/settle_times.py
Writes settle_times.csv and settle_times.png next to this file.
"""
from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from scipy import signal

HERE = Path(__file__).resolve().parent
RUN = HERE.parent / "autonomous_run_2026-09-30"
DT = 10.0
FS = 1.0 / DT
PUMPS = ["nutrient_pump_on", "ph_up_pump_on", "ph_down_pump_on", "water_pump_on"]

FILTERS = {
    "burst reading": None,
    "EMA 3 min (<=1.8)": (np.array([1 - np.exp(-DT / 180.0)]),
                          np.array([1.0, -np.exp(-DT / 180.0)])),
    "Butterworth-2 2 mHz (1.9)": signal.butter(2, 0.002, btype="low", fs=FS),
}
TOL_FRAC = 0.10
NOISE = {"TDS": 2.0, "pH": 0.03}
MIN_STEP = {"TDS": 3.0, "pH": 0.1}


def load():
    d = pd.read_csv(RUN / "sensor_readings.csv", low_memory=False)
    d = d[(d.row_source == "live") & (d.hours_since_plain_water >= 0)].copy()
    d["t"] = pd.to_datetime(d.time_utc, utc=True)
    for p in PUMPS:
        d[p] = d[p].astype(str).str.lower().isin(["1", "true", "1.0"])
    d["pump"] = d[PUMPS].any(axis=1)
    ev = pd.read_csv(RUN / "actuator_events.csv")
    ev = ev[ev.hours_since_plain_water > 0].copy()
    ev["t"] = pd.to_datetime(ev.time_utc, utc=True)
    return d.sort_values("t").reset_index(drop=True), ev.sort_values("t").reset_index(drop=True)


def settle_one(d, t0, col, label, direction):
    w = d[(d.t > t0 - pd.Timedelta(minutes=30)) & (d.t < t0 + pd.Timedelta(minutes=41))]
    tm = (w.t - t0).dt.total_seconds().to_numpy() / 60
    x = w[col].astype(float).to_numpy().copy()
    pump = w.pump.to_numpy()
    for i in range(1, len(x)):                      # firmware holds during a pump
        if pump[i] or np.isnan(x[i]):
            x[i] = x[i - 1]
    pre = (tm > -20) & (tm < -0.5)
    if pre.sum() < 30 or (tm > 40).sum() == 0:
        return None
    slope = np.polyfit(tm[pre], x[pre], 1)[0]       # drift per minute
    x = x - slope * tm
    base = np.median(x[(tm > -10) & (tm < -0.5)])
    final = np.median(x[(tm > 25) & (tm < 40)])
    step = final - base
    out = {"step": step}
    # Too small to time, or moving against the dose (e.g. a pH-up dose given
    # while pH was still falling): not a clean step response.
    if abs(step) < MIN_STEP[label] or np.sign(step) != direction:
        return out
    tol = max(TOL_FRAC * abs(step), NOISE[label])
    after = (tm >= 0) & (tm <= 35)
    for name, ba in FILTERS.items():
        if ba is None:
            y = x
        else:
            b, a = ba
            y, _ = signal.lfilter(b, a, x, zi=signal.lfilter_zi(b, a) * x[0])
        ta = tm[after]
        outside = np.nonzero(np.abs(y[after] - final) > tol)[0]
        if not len(outside):
            out[name] = 0.0
        elif outside[-1] + 1 >= len(ta):
            out[name] = np.inf          # not settled within 35 min
        else:
            out[name] = float(ta[outside[-1] + 1])
    return out


def main():
    d, ev = load()
    rows = []
    for _, e in ev.iterrows():
        nxt = ev.t[ev.t > e.t]
        clash = len(nxt) and (nxt.iloc[0] - e.t) < pd.Timedelta(minutes=40)
        col, label = (("tds_ppm", "TDS") if e.pump == "nutrient" else ("ph_filtered", "pH"))
        direction = -1 if e.pump == "ph_down" else 1
        res = settle_one(d, e.t, col, label, direction)
        kind = ("nutrient (overrun)" if e.pump == "nutrient" and e.overrun == 1
                else e.pump.replace("_", "-"))
        row = {"time_utc": e.time_utc, "dose": kind, "booked_ml": e.controller_accounted_ml,
               "next_pump_within_40min": bool(clash)}
        if res:
            row.update(res)
        rows.append(row)
    out = pd.DataFrame(rows)
    out.to_csv(HERE / "settle_times.csv", index=False)

    names = list(FILTERS)
    timed = out.dropna(subset=[names[0]])
    timed = timed[~timed.next_pump_within_40min]
    print(f"doses: {len(out)}, timed (step above noise, no pump within 40 min): {len(timed)}")
    summary = (timed.groupby("dose")[names]
               .agg(["count", "median", lambda s: s.quantile(0.9), "max",
                     lambda s: int(np.isinf(s).sum())]))
    stat_names = ["count", "median", "p90", "max", "not settled <35 min"]
    summary.columns = [f"{f} | {stat_names[i % 5]}" for i, (f, _) in enumerate(summary.columns)]
    with pd.option_context("display.width", 200, "display.max_columns", 20):
        print(summary.round(1).T.to_string())
    untimed = out[out[names[0]].isna()]
    print("\nnot timed (step within noise/drift):")
    print(untimed.groupby("dose").size().to_string())

    fig, axes = plt.subplots(1, 2, figsize=(10, 3.6), sharey=False)
    for ax, dose in zip(axes, ["nutrient", "ph-up"]):
        sub = timed[timed.dose.str.startswith(dose)]
        data = [np.minimum(sub[n].to_numpy(), 36.0) for n in names]  # >35 min drawn at 36
        if len(sub) >= 5:
            ax.boxplot(data, tick_labels=["burst", "EMA 3 min", "Butterworth 2 mHz"],
                       showfliers=False)
        else:
            ax.set_xticks([1, 2, 3], ["burst", "EMA 3 min", "Butterworth 2 mHz"])
        for i, v in enumerate(data, start=1):
            ax.scatter(np.full(len(v), i) + np.random.uniform(-0.08, 0.08, len(v)), v,
                       s=14 if len(sub) >= 5 else 40, alpha=0.7, color="tab:blue")
        ax.set_ylabel("Minutes to settle (within 10 % or noise)")
        name = "Nutrient doses" if dose == "nutrient" else "pH-up doses (clean steps)"
        ax.set_title(f"{name}, n = {len(sub)}")
        ax.grid(True, alpha=0.3)
    fig.suptitle("Settling time of each real dose, 30 Sep - 5 Oct, by control-input filter")
    fig.savefig(HERE / "settle_times.png", dpi=150, bbox_inches="tight")


if __name__ == "__main__":
    main()
