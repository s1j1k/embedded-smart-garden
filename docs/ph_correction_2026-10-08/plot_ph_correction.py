"""Unattended pH correction on 8 Oct 2026 (firmware 1.9.12), and the TDS it added.

    python plot_ph_correction.py  ->  ph_correction_2026-10-08.png / .pdf

Data: readings.csv (Pi SQLite export, 02:00-12:30 UTC). pH drifted below the 5.5 dead
limit; the controller dosed pH-up at 04:50 UTC (1.6 mL) and, after a run of reboots,
at 06:15 UTC (2.9 mL). Delivered volumes are the drops in the pH-up stock estimate.
The level sensor read nothing during this window, so the caps used the default 6.5 L.
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import sys as _sys
from pathlib import Path as _Path
_sys.path.insert(0, str(_Path(__file__).resolve().parents[1]))
import report_style  # noqa: E402  LaTeX fonts, MATLAB axes
import pandas as pd

TEAL, CORAL, BAND, BLUE, GREY = "#0b5d7a", "#e4572e", "#cfe8d6", "#2a78d6", "#8c959f"
report_style.apply()

r = pd.read_csv("readings.csv")
r["t"] = pd.to_datetime(r["timestamp"], utc=True)
t0 = pd.Timestamp("2026-10-08T04:50:26Z")
r["min"] = (r["t"] - t0).dt.total_seconds() / 60
r = r[(r["min"] >= -90) & (r["min"] <= 240)]

stock = r[["min", "ph_up_rem_ml"]].dropna()
drops = stock[stock.ph_up_rem_ml.diff() < -0.05]
doses = [(m, -d) for m, d in zip(drops["min"], stock.ph_up_rem_ml.diff()[drops.index])]
boots = r[r.boot_id.diff() > 0]["min"]

fig, (a1, a2) = plt.subplots(2, 1, figsize=(9, 6.2), sharex=True,
                             gridspec_kw={"height_ratios": [1.3, 1]})
a1.axhspan(5.5, 6.5, color=BAND, label="pass band 5.5-6.5")
a1.plot(r["min"], r.ph, color=BLUE, lw=0, marker=".", ms=2.5, alpha=0.35, label="raw")
a1.plot(r["min"], r.ph_filtered, color=TEAL, lw=2.4, label="control value")
a1.set_ylim(4.8, 6.6)
a1.set_ylabel("pH")
a2.axhline(850, color=CORAL, ls=":", lw=1.4, label="upper dead limit 850")
a2.plot(r["min"], r.tds_ppm, color=TEAL, lw=1.6, label="TDS")
a2.set_ylabel("TDS (ppm)")
a2.set_xlabel("minutes after the first pH-up dose (04:50 UTC, 15:50 local)")
for ax in (a1, a2):
    for m, ml in doses:
        ax.axvline(m, color=CORAL, ls="--", lw=1.3)
    for m in boots:
        ax.axvline(m, color=GREY, lw=0.6, alpha=0.6)
for m, ml in doses:
    a1.annotate(f"pH-up {ml:.1f} mL", (m, 6.55), xytext=(3, 0), textcoords="offset points",
                rotation=90, fontsize=10, color=CORAL, va="top")
a1.set_title("Unattended pH correction, 8 Oct 2026 (firmware 1.9.12); grey lines: reboots",
             loc="left", color=TEAL, fontweight="bold", fontsize=12)
a1.legend(loc="lower right", frameon=False, fontsize=9)
a2.legend(loc="lower right", frameon=False, fontsize=9)
fig.tight_layout()
for ext in ("png", "pdf"):
    fig.savefig(f"ph_correction_2026-10-08.{ext}", dpi=200)

before = lambda m: r[(r["min"] > m - 10) & (r["min"] < m - 0.2)].tds_ppm.median()
after = lambda m, w: r[(r["min"] > m + w - 5) & (r["min"] < m + w)].tds_ppm.median()
for m, ml in doses:
    print(f"dose {ml:.2f} mL at {m:.1f} min: pH {r.iloc[(r['min'] - m).abs().argmin()].ph_filtered:.2f},"
          f" TDS {before(m):.0f} -> {after(m, 35):.0f}")
print("boots at", [round(m, 1) for m in boots])
print("pH at +60, +120, +200 min:",
      [round(r.iloc[(r['min'] - m).abs().argmin()].ph_filtered, 2) for m in (60, 120, 200)])
