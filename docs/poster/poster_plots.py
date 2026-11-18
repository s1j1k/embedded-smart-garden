"""Poster figures: closed-loop response (TDS and pH) and the dosing record.

    python poster_plots.py step_1.9.5.csv.gz 14 "2026-10-07T01:55:20Z" 1.9.5

Arguments: Pi export (scripts/pi_export_mqtt_telemetry.py), boot_id, the UTC time the
step started (AUTO RESUME), and a label. Writes closed_loop_<label>.png/.pdf and
dosing_<label>.png/.pdf. Doses come from the control events: a nutrient dose is
logged on its ec_check, a pH dose on its ph_recheck (it is drawn at the check that
issued it, the last ph_check before that recheck).
"""
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parents[1]))
import report_style  # noqa: E402  LaTeX fonts, MATLAB axes
import pandas as pd

# Poster palette (matches the teal/green/coral of the poster).
TEAL = "#0b5d7a"
GREEN = "#3a9d5d"
CORAL = "#e4572e"
BLUE = "#2a78d6"
GREY = "#8c959f"
BAND = "#cfe8d6"

report_style.apply()

path, boot, start_utc, label = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
heading = sys.argv[5] if len(sys.argv) > 5 else "Closed-loop response"
xlab = sys.argv[6] if len(sys.argv) > 6 else "minutes after the step"
d = pd.read_csv(path, low_memory=False)
d = d[d.boot_id == boot].copy()
d["t"] = pd.to_datetime(d["timestamp"], utc=True)
t0 = pd.Timestamp(start_utc)
d["min"] = (d["t"] - t0).dt.total_seconds() / 60
r = d[(d.kind == "reading") & (d["min"] >= -5)].sort_values("min")
ev = d[d.kind == "control_event"].sort_values("min")
for col in ("ec_dose_ml", "ph_dose_ml", "ph_dose_dir", "ec_before", "ph_before"):
    if col not in ev:
        ev[col] = float("nan")

# ---- Dose list ---------------------------------------------------------------------------
doses = []                                          # (min, pump, mL)
for _, e in ev[(ev.event == "ec_check") & ev.ec_dose_ml.notna()].iterrows():
    doses.append((e["min"], "Nutrient", e.ec_dose_ml))
checks = ev[ev.event == "ph_check"]["min"].tolist()
for _, e in ev[(ev.event == "ph_recheck") & ev.ph_dose_ml.notna()].iterrows():
    issued = max([m for m in checks if m < e["min"]], default=e["min"])
    doses.append((issued, "pH-up" if e.ph_dose_dir == "up" else "pH-down", e.ph_dose_ml))
doses.sort()

def band(ax, lo, hi, dlo, dhi):
    ax.axhspan(lo, hi, color=BAND, zorder=0, label="target band")
    for v in (dlo, dhi):
        ax.axhline(v, color=CORAL, ls=":", lw=1.4, zorder=1)

def marks(ax, which, color):
    for m, pump, ml in doses:
        if pump in which:
            ax.axvline(m, color=color, ls="--", lw=1.2, zorder=1)

# ---- Closed-loop response ---------------------------------------------------------------
fig, (a1, a2) = plt.subplots(2, 1, figsize=(9, 6.4), sharex=True)
band(a1, 600, 800, 550, 850)
a1.plot(r["min"], r.tds_ppm, color=BLUE, lw=0, marker=".", ms=3, alpha=0.35, label="raw")
a1.plot(r["min"], r.tds_ppm_ema, color=TEAL, lw=2.6, label="control value")
marks(a1, ("Nutrient",), GREEN)
a1.set_ylabel("TDS (ppm)")
a1.set_ylim(min(400, r.tds_ppm_ema.min() - 30), 900)
a1.text(0.99, 0.04, "dead limits 550 / 850 ppm", transform=a1.transAxes, ha="right",
        color=CORAL, fontsize=10)

band(a2, 5.8, 6.2, 5.5, 6.5)
a2.plot(r["min"], r.ph, color=BLUE, lw=0, marker=".", ms=3, alpha=0.35)
a2.plot(r["min"], r.ph_ema if "ph_ema" in r else r.ph, color=TEAL, lw=2.6)
marks(a2, ("pH-up", "pH-down"), CORAL)
a2.set_ylabel("pH")
a2.set_xlabel(xlab)
a2.text(0.99, 0.04, "dead limits 5.5 / 6.5", transform=a2.transAxes, ha="right",
        color=CORAL, fontsize=10)

# Settling: end of the last dose's settling wait, if a later check found both inside.
title = f"{heading} (firmware {label})"
# Settled: the first check after the last dose's wait at which both control values
# are inside their dead limits (no dose follows). Not marked if that never happens.
settled = None
if doses:
    last = doses[-1][0]
    later = ev[(ev["min"] > last + 19) & ev.event.isin(["ec_check", "ph_check"])]
    for _, e in later.iterrows():
        if 550 <= e.ec_before <= 850 and 5.5 <= e.ph_before <= 6.5:
            settled = e["min"]
            break
if settled is not None:
    for ax in (a1, a2):
        ax.axvline(settled, color="k", lw=1.6)
    a1.annotate(f"settled: {settled - doses[0][0]:.0f} min",
                xy=(settled, 880), xytext=(-6, 0), textcoords="offset points",
                fontsize=12, va="top", ha="right", fontweight="bold")
else:
    title += " - pH not yet settled"
a1.set_title(title, loc="left", color=TEAL, fontweight="bold")
h, l = a1.get_legend_handles_labels()
a1.legend(h, l, loc="center right", fontsize=10, frameon=False)
fig.tight_layout()
for ext in ("png", "pdf"):
    fig.savefig(f"closed_loop_{label}.{ext}", dpi=220)

# ---- Dosing record ------------------------------------------------------------------------
colors = {"Nutrient": GREEN, "pH-up": CORAL, "pH-down": BLUE}
fig, ax = plt.subplots(figsize=(9, 3.4))
for pump, color in colors.items():
    xs = [m for m, p, _ in doses if p == pump]
    ys = [ml for _, p, ml in doses if p == pump]
    if xs:
        ax.vlines(xs, 0, ys, color=color, lw=4)
        ax.scatter(xs, ys, color=color, s=60, zorder=3, label=pump)
        for x, y in zip(xs, ys):
            ax.annotate(f"{y:.2g} mL", (x, y), xytext=(4, 4), textcoords="offset points",
                        fontsize=10, color=color)
ax.set_xlim(a1.get_xlim())
ax.set_ylabel("dose (mL)")
ax.set_xlabel(xlab)
ax.set_title("Doses decided by the controller", loc="left", color=TEAL, fontweight="bold")
ax.legend(frameon=False, fontsize=10)
fig.tight_layout()
for ext in ("png", "pdf"):
    fig.savefig(f"dosing_{label}.{ext}", dpi=220)
print("doses:", [(round(m, 1), p, round(v, 2)) for m, p, v in doses])
