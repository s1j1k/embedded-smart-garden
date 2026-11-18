"""Poster figure: the pH loop bringing pH back into range (7 Oct 2026, firmware 1.9.12).

    python ph_loop_plot.py   ->  ph_loop_1.9.12.png / .pdf

Data: ph_loop_1.9.12.csv.gz (Pi MQTT export from 09:15 UTC). Boots 28 and 29 (the
ESP32 was reset at 21:40 local to clear the pH session cap). pH-up doses are logged
on their rechecks and drawn at the check that issued them; the third dose's recheck
was cut by the reset, so its size is the session total minus the first two.
Water additions (1.5 L, 1 L) are taken from the level jumps.
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

d = pd.read_csv("ph_loop_1.9.12.csv.gz", low_memory=False).copy()
d["t"] = pd.to_datetime(d["timestamp"], utc=True).dt.tz_convert("Australia/Melbourne")
r = d[(d.kind == "reading") & d.boot_id.isin([28, 29])].sort_values("t")
# Step test starts at the first pH-up dose after the tap water was added (21:12:41);
# earlier history (how the tank got there) is not shown.
t0 = pd.Timestamp("2026-10-07T10:12:41Z").tz_convert("Australia/Melbourne")
r["min"] = (r["t"] - t0).dt.total_seconds() / 60
r = r[r["min"] >= -2]

doses = [(0.0, 3.16), (20.1, 8.24 - 3.06 - 3.16)]   # min after t0, mL
lvl = r[["min", "water_level_pct"]].dropna()
jumps = lvl[lvl.water_level_pct.diff() >= 8]
# 1.5 L then 1 L were added within ~4 min of each other; shown as one marker.
water = []
settled = (pd.Timestamp("2026-10-07T10:50:24Z").tz_convert("Australia/Melbourne") - t0).total_seconds() / 60

fig, ax = plt.subplots(figsize=(9, 4.6))
ax.axhspan(5.5, 6.5, color=BAND, label="pass band 5.5-6.5")
ax.plot(r["min"], r.ph, color=BLUE, lw=0, marker=".", ms=2.5, alpha=0.35, label="raw")
ax.plot(r["min"], r.ph_ema, color=TEAL, lw=2.6, label="control value")
for m, ml in doses:
    ax.axvline(m, color=CORAL, ls="--", lw=1.3)
    ax.annotate(f"pH-up {ml:.1f} mL", (m, 5.98), xytext=(3, 0), textcoords="offset points",
                rotation=90, fontsize=10, color=CORAL, va="top")
for m, txt in water:
    ax.axvline(m, color=GREY, ls="-.", lw=1.0)
    ax.annotate(txt, (m, 5.98), xytext=(-14, 0), textcoords="offset points", rotation=90,
                fontsize=10, color=GREY, va="top")
ax.axvline(settled, color="k", lw=1.6)
ax.annotate(f"in range, no dose: {settled:.0f} min", (settled, 6.38), xytext=(-6, 0),
            textcoords="offset points", ha="right", fontsize=11, fontweight="bold")
ax.set_ylim(5.0, 6.6)
ax.set_ylabel("pH")
ax.set_xlabel("minutes after the step (first pH-up dose)")
ax.set_title("pH step response: pH-up loop back into the pass band (firmware 1.9.12)",
             loc="left", color=TEAL, fontweight="bold", fontsize=13)
ax.legend(loc="upper left", frameon=False, fontsize=10)
fig.tight_layout()
for ext in ("png", "pdf"):
    fig.savefig(f"ph_loop_1.9.12.{ext}", dpi=220)
print("water additions at", [round(m, 1) for m, _ in water], "min; settled at", round(settled, 1))
