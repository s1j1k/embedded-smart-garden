"""Figures for the final bring-up from plain water, 16 Oct 2026 (firmware 1.9.23).

Inputs (from scripts/export_bringup_run.py): sensor_readings.csv, actuator_events.csv,
control_events.csv. Run from this folder:

    python plot_final.py

bringup_final.png   TDS and pH from plain water into range: the nutrient loop, then
                    the pH loop, up to a few minutes after both are in range.
ph_recovery.png     The pH loop correcting a high pH (drifted to ~6.9 later in the run):
                    one pH-down dose takes it from 6.9 to the 6.5 limit.
"""
from __future__ import annotations

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import report_style as rs  # noqa: E402

rs.apply()

r = pd.read_csv("sensor_readings.csv", parse_dates=["time"])
doses = pd.read_csv("actuator_events.csv", parse_dates=["time"])
ev = pd.read_csv("control_events.csv")
ev["time"] = pd.to_datetime(ev["received_at"], utc=True, format="ISO8601")

T0 = doses.loc[doses.pump == "nutrient", "time"].min()           # first nutrient dose
RESET = ev.loc[ev.event == "ph_session_reset", "time"].min()       # pH loop re-armed
r["t"] = (r.time - T0).dt.total_seconds() / 60
doses["t"] = (doses.time - T0).dt.total_seconds() / 60

PUMP = {"nutrient": (rs.GREEN, "nutrient"), "ph_up": (rs.PURPLE, "pH-up"),
        "ph_down": (rs.ORANGE, "pH-down")}


def mark_doses(ax, rows, t_col, y, unit_fmt="{:.2f} mL"):
    for _, d in rows.iterrows():
        colour, _ = PUMP[d.pump]
        ax.axvline(d[t_col], color=colour, lw=0.9, ls="--", zorder=1)
        ax.annotate(unit_fmt.format(d.delivered_ml), (d[t_col], y), xytext=(3, 0),
                    textcoords="offset points", rotation=90, va="bottom", fontsize=7,
                    color=colour)


def first_inside(t, values, low, high, hold_min=10.0):
    """First time the series enters [low, high] and stays there hold_min minutes."""
    inside = (values >= low) & (values <= high)
    for i in range(len(t)):
        if inside.iloc[i]:
            window = (t >= t.iloc[i]) & (t <= t.iloc[i] + hold_min)
            if inside[window].all() and t.iloc[-1] >= t.iloc[i] + hold_min:
                return t.iloc[i]
    return None


# ---- Figure 1: bring-up, until both TDS and pH are in range ------------------------
early = r[(r.t >= 0) & (r.t <= 60)]
t_tds = first_inside(early.t, early.tds_ppm_ema, 600, 800)
t_ph = first_inside(early.t, early.ph_ema, 5.5, 6.5, hold_min=5.0)
END = max(t_tds, t_ph) + 3
win = r[(r.t >= -5) & (r.t <= END)]
d1 = doses[doses.t <= END]
fig, (ax_tds, ax_ph) = plt.subplots(2, 1, figsize=(6.5, 5.2), sharex=True)

ax_tds.axhspan(600, 800, color=rs.GREEN, alpha=0.12, lw=0, label="target 600-800 ppm")
ax_tds.axhline(550, color=rs.RED, ls=":", lw=1, label="dose limit 550 ppm")
ax_tds.plot(win.t, win.tds_ppm, ".", ms=1.5, color=rs.CYAN, alpha=0.6, label="raw")
ax_tds.plot(win.t, win.tds_ppm_ema, color=rs.BLUE, lw=1.4, label="control value")
mark_doses(ax_tds, d1[d1.pump == "nutrient"], "t", 30)
ax_tds.plot([t_tds], [600], "o", color=rs.BLUE, ms=4)
ax_tds.annotate(f"in the target band at {t_tds:.0f} min", (t_tds, 600), xytext=(6, -14),
                textcoords="offset points", fontsize=8)
ax_tds.set_ylabel("TDS (ppm)")
ax_tds.set_ylim(0, 900)
ax_tds.legend(loc="lower right", fontsize=7, ncol=2)

ax_ph.axhspan(5.5, 6.5, color=rs.GREEN, alpha=0.12, lw=0, label="dead limits 5.5-6.5")
ax_ph.plot(win.t, win.ph_ema, color=rs.BLUE, lw=1.4, label="control value")
mark_doses(ax_ph, d1[d1.pump != "nutrient"], "t", 2.7)
y = win.ph_ema[win.t >= t_ph].iloc[0]
ax_ph.plot([t_ph], [y], "o", color=rs.BLUE, ms=4)
ax_ph.annotate(f"in the dead limits at {t_ph:.0f} min", (t_ph, y), xytext=(-10, -16),
               textcoords="offset points", fontsize=8, ha="right")
ax_ph.set_ylabel("pH")
ax_ph.set_ylim(2.5, 8.2)
ax_ph.set_xlim(-5, END)
ax_ph.set_xlabel("minutes after the first nutrient dose")
ax_ph.legend(loc="upper right", fontsize=7)
ax_ph.annotate("nutrient acid in\n17 ppm water", (2.5, 4.4), fontsize=7)
fig.suptitle("Bring-up from plain water (7 L, plants in), 16 Oct 2026", fontsize=10)
fig.tight_layout()
fig.savefig("bringup_final.png", dpi=200)

# ---- Figure 1b: TDS only (poster) -------------------------------------------------
# The pH in this run was read with a provisional calibration (no buffers), which
# the 18 Oct buffer calibration showed read 0.8 pH low at pH 4, so the poster
# shows the nutrient loop only.
fig, ax = plt.subplots(figsize=(6.5, 3.0))
ax.axhspan(600, 800, color=rs.GREEN, alpha=0.12, lw=0, label="target 600-800 ppm")
ax.axhline(550, color=rs.RED, ls=":", lw=1, label="dose limit 550 ppm")
ax.plot(win.t, win.tds_ppm, ".", ms=1.5, color=rs.CYAN, alpha=0.6, label="raw")
ax.plot(win.t, win.tds_ppm_ema, color=rs.BLUE, lw=1.4, label="control value")
mark_doses(ax, d1[d1.pump == "nutrient"], "t", 30)
ax.plot([t_tds], [600], "o", color=rs.BLUE, ms=4)
ax.annotate(f"in the target band at {t_tds:.0f} min", (t_tds, 600), xytext=(6, -14),
            textcoords="offset points", fontsize=8)
ax.set_ylabel("TDS (ppm)")
ax.set_ylim(0, 900)
ax.set_xlim(-5, END)
ax.set_xlabel("minutes after the first nutrient dose")
ax.legend(loc="lower right", fontsize=7, ncol=2)
ax.set_title("Nutrient from plain water (7 L, plants in), 16 Oct 2026", fontsize=10)
fig.tight_layout()
fig.savefig("bringup_tds.png", dpi=200)

# ---- Figure 2: the pH loop correcting a high pH -----------------------------------
# The first pH-down dose after pH had drifted to ~6.9, until pH is back inside its limits.
down = doses[(doses.pump == "ph_down") & (doses.time >= RESET)] if pd.notna(RESET) else doses[:0]
t_ph_in = None
if len(down):
    D = down.time.min()
    r["td"] = (r.time - D).dt.total_seconds() / 60
    doses["td"] = (doses.time - D).dt.total_seconds() / 60
    after = r[r.td >= 0]
    t_ph_in = first_inside(after.td, after.ph_ema, 5.5, 6.5, hold_min=3.0)
    # The dose's effect: from just before it until pH has stopped falling.
    stop = 8.0
    demo = r[(r.td >= -2) & (r.td <= stop)]
    d2 = doses[(doses.td >= 0) & (doses.td <= stop) & (doses.pump != "nutrient")]
    fig, ax = plt.subplots(figsize=(6.5, 3.2))
    ax.axhspan(5.5, 6.5, color=rs.GREEN, alpha=0.12, lw=0, label="dead limits 5.5-6.5")
    ax.axhspan(5.8, 6.2, color=rs.GREEN, alpha=0.18, lw=0, label="target 5.8-6.2")
    ax.plot(demo.td, demo.ph_ema, color=rs.BLUE, lw=1.4, label="pH (control value)")
    mark_doses(ax, d2, "td", 5.55)
    before = demo.ph_ema[demo.td < 0].iloc[-1]
    landed = demo.ph_ema[demo.td >= 5].iloc[0]
    ax.annotate(f"pH {before:.2f} to {landed:.2f} in 5 min", (5, landed), xytext=(6, 10),
                textcoords="offset points", fontsize=8)
    ax.set_xlim(-2, stop)
    ax.set_xlabel("minutes after the pH-down dose")
    ax.set_ylabel("pH")
    ax.set_ylim(5.4, 7.05)
    ax.set_title("pH loop: one pH-down dose brings a high pH back to its limit", fontsize=10)
    ax.legend(loc="lower left", fontsize=7, ncol=3)
    fig.tight_layout()
    fig.savefig("ph_recovery.png", dpi=200)

print("first nutrient dose", T0, "| reset", RESET)
print(f"TDS in target band at {t_tds:.1f} min, pH in dead limits at {t_ph:.1f} min; "
      f"pH-down demo: inside the limits {t_ph_in} min after the dose")
for _, d in doses.iterrows():
    print(f"{d.time:%H:%M:%S} {d.pump:9s} {d.delivered_ml:6.3f} mL  "
          f"{d.reading_before} -> {d.reading_after}")
