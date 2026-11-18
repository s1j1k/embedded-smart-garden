#!/usr/bin/env python3
"""Process gains of the three dosing loops from every logged dose, and the figure.

Inputs
  all_doses_2026-10-06_to_14.csv  every settled dose in the Pi event log, paired
                                  check -> recheck (scripts/export_bringup_run.py
                                  logic, built for 6-14 Oct)
  plus the step tests written up in docs/ (nutrient runs 1-3, 8 Oct pH-up, 7 Oct
  pH-down buffer test), entered below with their source.

Outputs (this folder, and the figure into the report folder)
  gains_curated.csv     one row per measurement: loop, time, per-litre gain,
                        used / excluded and why, source
  gain_identification.png

The gain used for dose sizing is the largest valid per-litre response of each loop:
with a proportional dose u = lambda e / K and an integrating tank, the next error is
e (1 - lambda K_true / K), so no dose overshoots while K >= lambda K_true. Taking the
largest response seen makes every dose land short in a more buffered tank, and the
next check doses again.

    python docs/dose_response/build_gains.py
"""

from __future__ import annotations

import sys
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import report_style  # noqa: E402

REPORT = HERE.parents[2] / "smart-garden-report"
TZ = "Australia/Melbourne"

# Periods where a measurement does not describe the tank (pump or probe fault).
FAULTS = [
    # 14 Oct 01:51-04:33 local: partial first dose, then nothing delivered.
    ("nutrient", "2026-10-13T14:45Z", "2026-10-13T18:00Z", "nutrient pump not delivering"),
    # 11 Oct 23:53 - 13 Oct 02:48 local: logged doses, pump not turning (re-primed 13 Oct).
    ("ph_down", "2026-10-11T12:00Z", "2026-10-12T16:00Z", "pH-down pump not delivering"),
    # 9-11 Oct: failing pH signal board, readings swinging 1.4-12; recalibrated 11 Oct.
    ("ph_up", "2026-10-09T00:00Z", "2026-10-11T00:00Z", "pH probe front-end fault"),
    ("ph_down", "2026-10-09T00:00Z", "2026-10-11T00:00Z", "pH probe front-end fault"),
]
# Step tests written up in docs/ (per-litre gain, local date).
DOC_POINTS = [
    # 30 Sep - 3 Oct autonomous run (docs/autonomous_run_2026-09-30/dose_response.csv,
    # 6.5 L): the clean pH-up step near plain water, and two in the buffered tank.
    ("ph_up", "2026-10-01T00:07:54Z", 1.181 / 0.538 * 6.5, "30 Sep run: clean step at 170 ppm"),
    ("ph_up", "2026-10-02T00:51:13Z", 0.037 / 0.541 * 6.5, "30 Sep run: step in buffered tank"),
    ("ph_up", "2026-10-03T08:52:21Z", -0.161 / 0.49 * 6.5, "30 Sep run: step in buffered tank"),
    # Nutrient runs 1-3 (6-7 Oct) are in the event log already.
    ("ph_up", "2026-10-08 04:50", 0.04 * 6.5, "8 Oct correction, dose 1 (docs/ph_correction_2026-10-08)"),
    ("ph_up", "2026-10-08 06:15", 0.22 * 6.5, "8 Oct correction, dose 2 (docs/ph_correction_2026-10-08)"),
    ("ph_down", "2026-10-07 17:00", 0.03 * 8.0, "7 Oct buffer test, 18 mL by hand"),
]
# The dose that sets each gain, measured the same way from the raw readings:
# dy = mean over the settle window - mean over the 5 min before the dose, and the
# per-litre gain is dy V / u. (loop, readings, time column, value column, dose time
# UTC, delivered mL, volume L, settle window min, source)
EVENTS = [
    ("ph_up", "autonomous_run_2026-09-30/sensor_readings.csv", "time_utc", "ph_ema",
     "2026-10-01T00:07:54Z", 0.538, 6.5, (20, 26), "1 Oct 10:07, 170 ppm (30 Sep run)"),
    ("ph_down", "bringup_2026-10-14_plants/sensor_readings.csv", "time", "ph_ema",
     "2026-10-13T19:08:55Z", 0.544, 6.5, (10, 20), "14 Oct 06:08, 127 ppm (bring-up)"),
    ("nutrient", "bringup_2026-10-06/readings.csv", "timestamp", "tds_ppm_ema",
     "2026-10-06T09:26:26Z", 32.705, 8.4, (20, 26), "6 Oct 20:26, run 1 (8.4 L)"),
]
SYMBOL = {"ph_up": r"\hat{G}_{p,\mathrm{up}}", "ph_down": r"\hat{G}_{p,\mathrm{down}}",
          "nutrient": r"\hat{G}_{p,\mathrm{EC}}"}
YUNIT = {"ph_up": "pH", "ph_down": "pH", "nutrient": "ppm"}


def event(loop):
    """(minutes, values, before, settled, gain per litre, row) for a loop's defining dose."""
    row = next(e for e in EVENTS if e[0] == loop)
    _, path, tcol, ycol, at, u, V, (s0, s1), _src = row
    r = pd.read_csv(HERE.parent / path, low_memory=False)
    r["t"] = pd.to_datetime(r[tcol], utc=True, errors="coerce")
    r[ycol] = pd.to_numeric(r[ycol], errors="coerce")
    m = (r["t"] - pd.Timestamp(at)).dt.total_seconds() / 60
    w = r[(m >= -8) & (m <= s1 + 6)].assign(m=m)
    before = w.loc[(w.m >= -5) & (w.m < 0), ycol].mean()
    settled = w.loc[(w.m >= s0) & (w.m <= s1), ycol].mean()
    gain = (settled - before) / u * V * (-1 if loop == "ph_down" else 1)
    return w["m"], w[ycol], before, settled, gain, row


UNITS = {"nutrient": "ppm L / mL", "ph_up": "pH L / mL", "ph_down": "pH L / mL"}
TITLES = {"ph_up": "pH-up", "ph_down": "pH-down", "nutrient": "Nutrient"}
# Gains in firmware before and after this analysis (per litre).
OLD = {"ph_up": (2.52, "up to 1.9.21: 2.52 at 700 ppm, x 700/TDS"),
       "ph_down": (10.0, "up to 1.9.21: bottle label 10"),
       "nutrient": (260.0, "up to 1.9.21: 260")}


def load() -> pd.DataFrame:
    d = pd.read_csv(HERE / "all_doses_2026-10-06_to_14.csv", parse_dates=["time"])
    d = d[d["loop"].isin(["nutrient", "ph_up", "ph_down"])].copy()
    d["gain"] = d["gain_L_per_ml"]
    d.loc[d["loop"] == "ph_down", "gain"] = -d.loc[d["loop"] == "ph_down", "gain_L_per_ml"]
    d["status"], d["note"] = "used", "controller event log"

    for loop, start, end, why in FAULTS:
        hit = (d["loop"] == loop) & (d["time"] >= start) & (d["time"] <= end)
        d.loc[hit, ["status", "note"]] = ["excluded", why]
    # 6 Oct 22:59: the level read 4.0 L, so its per-litre gain is understated.
    hit = (d["loop"] == "nutrient") & (d["V"] < 5)
    d.loc[hit, ["status", "note"]] = ["excluded", "level misread (4.0 L)"]
    # 10 Oct 22:08: the recheck caught a spike (786 ppm) from the failing probe front
    # end; TDS settled at 634 ppm 20 min later: 199 ppm for 5.49 mL at 6.5 L.
    hit = (d["loop"] == "nutrient") & (d["time"].dt.strftime("%m-%d %H") == "10-10 11")
    d.loc[hit, "gain"] = (634.2 - 435.4) / 5.487 * 6.5
    d.loc[hit, "note"] = "recheck spike; settled value used"
    # 6 Oct 20:39-21:03: three pH-up doses 12 min apart, none settled: one 2.60 mL step.
    six = (d["loop"] == "ph_up") & (d["time"] < "2026-10-06T11:00Z")
    if six.sum() == 3:
        first = d[six].iloc[0]
        d.loc[six, ["status", "note"]] = ["excluded", "unsettled (12 min); counted as one step"]
        step = first.copy()
        step["ml"] = d.loc[six, "ml"].sum()
        step["after"] = d[six].iloc[-1]["after"]
        step["gain"] = (step["after"] - first["before"]) / step["ml"] * 6.5
        step["status"], step["note"] = "used", "6 Oct: three doses as one step"
        d = pd.concat([d, step.to_frame().T], ignore_index=True)
    d["source"] = "event log"

    def when(t):
        return pd.Timestamp(t) if t.endswith("Z") else pd.Timestamp(t, tz=TZ).tz_convert("UTC")

    docs = pd.DataFrame([{"loop": l, "time": when(t), "gain": g,
                          "status": "used", "note": src, "source": "docs"}
                         for l, t, g, src in DOC_POINTS])
    out = pd.concat([d, docs], ignore_index=True)
    out["time"] = pd.to_datetime(out["time"], utc=True)
    out["gain"] = out["gain"].astype(float)
    for loop in UNITS:
        *_, gain, row = event(loop)
        gap = (out["time"] - pd.Timestamp(row[4])).abs().where(out["loop"] == loop)
        if gap.min() < pd.Timedelta("15min"):   # the same dose in the event log or docs
            out.loc[gap.idxmin(), ["gain", "status", "note"]] = [
                gain, "used", f"defining dose, settled {row[7][0]}-{row[7][1]} min"]
        else:
            out.loc[len(out)] = {"loop": loop, "time": pd.Timestamp(row[4]), "gain": gain,
                                 "status": "used", "note": "defining dose", "source": row[1]}
    return out.sort_values(["loop", "time"]).reset_index(drop=True)


def chosen(g: pd.DataFrame) -> dict:
    out = {}
    for loop in UNITS:
        used = g[(g["loop"] == loop) & (g["status"] == "used")]
        best = used.loc[used["gain"].idxmax()]
        out[loop] = best
    return out


def figure(g: pd.DataFrame, pick: dict) -> plt.Figure:
    report_style.apply(9)
    fig, axes = plt.subplots(2, 3, figsize=(10.5, 6.8), gridspec_kw={"height_ratios": [1.2, 1]})
    order = ["ph_up", "ph_down", "nutrient"]
    for ax, loop in zip(axes[0], order):
        m, y, before, settled, gain, row = event(loop)
        u, V, (s0, s1) = row[5], row[6], row[7]
        unit = YUNIT[loop]
        digits = 0 if unit == "ppm" else 2
        gdig = 0 if unit == "ppm" else 1
        sym = SYMBOL[loop]
        ax.plot(m, y, color=report_style.BLUE, lw=1.3)
        ax.axvline(0, color=report_style.ORANGE, lw=1, ls=":")
        ax.axhline(before, color="0.45", lw=0.9, ls="--")
        ax.axhline(settled, color="0.45", lw=0.9, ls="--")
        ax.axvspan(s0, s1, color=report_style.GREEN, alpha=0.12)
        xa = s1 + 2.5
        ax.annotate("", xy=(xa, settled), xytext=(xa, before),
                    arrowprops=dict(arrowstyle="<->", color="k", lw=1))
        ax.text(xa + 1.2, (before + settled) / 2,
                rf"$\Delta y$ = {settled - before:+.{digits}f} {unit}", va="center", ha="left",
                fontsize=8)
        ax.text(0.6, before, f" dose $u$ = {u:.3g} mL",
                va="top" if loop == "ph_down" else "bottom", fontsize=8,
                color=report_style.ORANGE)
        ax.set_title(f"{TITLES[loop]}: {row[8]}", fontsize=9)
        box = (rf"${sym}\,V = |\Delta y|\,V/u$ = {gain:.{gdig}f} {UNITS[loop]}" "\n"
               rf"controller: ${sym} = {gain:.{gdig}f}/V_c$"
               f" = {gain / V:.{1 if unit == 'ppm' else 2}f} {unit}/mL at {V:g} L")
        # Room for the box above a rising trace, below a falling one.
        lo, hi = min(y.min(), before, settled), max(y.max(), before, settled)
        span = hi - lo
        down = loop == "ph_down"
        ax.set_ylim(lo - (0.62 if down else 0.06) * span, hi + (0.06 if down else 0.55) * span)
        ax.text(0.03, 0.03 if down else 0.97, box, transform=ax.transAxes,
                va="bottom" if down else "top", fontsize=7.4,
                bbox=dict(boxstyle="round,pad=0.3", fc="white", ec="0.7"))
        ax.set_xlabel("minutes from the dose")
        ax.set_ylabel("pH (filtered)" if unit == "pH" else "TDS (ppm, filtered)")
        ax.set_xlim(-8, s1 + 19)
        ax.grid(True, alpha=0.3)
    for ax, loop in zip(axes[1], order):
        rows = g[g["loop"] == loop]
        t = rows["time"].dt.tz_convert(TZ).dt.tz_localize(None)
        used = rows["status"] == "used"
        ax.scatter(t[used], rows.loc[used, "gain"], s=16, color=report_style.BLUE,
                   label="measured", zorder=3)
        ax.scatter(t[~used], rows.loc[~used, "gain"], s=16, marker="x",
                   color=report_style.ORANGE, label="excluded (fault)", zorder=3)
        old, old_label = OLD[loop]
        ax.axhline(old, color="0.4", ls="--", lw=1, label=old_label)
        best = pick[loop]
        gdig = 0 if loop == "nutrient" else 1
        ax.axhline(best["gain"], color=report_style.GREEN, lw=1.4,
                   label=rf"${SYMBOL[loop]}\,V$ = {best['gain']:.{gdig}f}: largest measured")
        bt = pd.Timestamp(best["time"]).tz_convert(TZ).tz_localize(None)
        ax.scatter([bt], [best["gain"]], s=70, facecolors="none",
                   edgecolors=report_style.GREEN, lw=1.4, zorder=4)
        ax.set_ylabel(rf"gain $\times V$ ({UNITS[loop]})")
        ax.tick_params(axis="x", rotation=30)
        ax.xaxis.set_major_formatter(plt.matplotlib.dates.DateFormatter("%d %b"))
        ax.legend(fontsize=6.3, loc="lower center" if loop == "nutrient" else "upper left")
        ax.grid(True, alpha=0.3)
    fig.tight_layout()
    return fig


def main() -> None:
    g = load()
    pick = chosen(g)
    cols = ["loop", "time", "ml", "before", "after", "tds", "V", "gain", "status", "note", "source"]
    g[[c for c in cols if c in g]].to_csv(HERE / "gains_curated.csv", index=False)
    fig = figure(g, pick)
    for folder in (HERE, REPORT):
        if folder.exists():
            fig.savefig(folder / "gain_identification.png", dpi=200)
    for loop, best in pick.items():
        n = int(((g["loop"] == loop) & (g["status"] == "used")).sum())
        print(f"{loop:9s} used {best['gain']:7.2f} {UNITS[loop]:10s} from "
              f"{pd.Timestamp(best['time']).tz_convert(TZ):%d %b %H:%M} ({best['note']}); "
              f"{n} valid measurements")


if __name__ == "__main__":
    main()
