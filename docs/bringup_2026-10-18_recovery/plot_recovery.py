"""Level, nutrient and pH loops on 18 Oct 2026: refill test, then recovery of a neglected tank.

    python plot_recovery.py  ->  recovery.png / .pdf, and the settling times on stdout

Data from scripts/export_bringup_run.py (sensor_readings.csv, actuator_events.csv).
Before this run the tank had little effective control for about two days: from
16 Oct 19:36 to 18 Oct 08:51 a refused top-up stalled the nutrient and pH loops,
and on 18 Oct auto control was paused for most of the day for tests and the pH
probe calibration. It started at about pH 7.0 (buffer calibration) and 345 ppm.

Level: the Grove stick reads in 5 % (5 mm) steps; the raw 10 s readings and their
30 s median (what firmware 1.9.24 acts on) are both shown. Band: 20 mm +/- 5 mm
below the net pots = 40-50 % on the stick (net-pot touch 65 %).
"Settled" = inside the band and staying there for 10 min.
Re-run as the run continues; the maintenance period extends to the latest export.
"""
import sys
from datetime import timedelta
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.dates as mdates  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import report_style as rs  # noqa: E402

rs.apply()
AEDT = pd.Timedelta(hours=11)
HOLD = pd.Timedelta(minutes=10)

r = pd.read_csv(HERE / "sensor_readings.csv")
r["t"] = pd.to_datetime(r["time"], utc=True).dt.tz_localize(None) + AEDT
r = r.sort_values("t")
r["level"] = pd.to_numeric(r["water_level_pct"], errors="coerce").where(lambda v: v >= 0)
r["level_med"] = r.set_index("t")["level"].rolling("30s").median().values
doses = pd.read_csv(HERE / "actuator_events.csv") if (HERE / "actuator_events.csv").exists() else pd.DataFrame()
if not doses.empty:
    doses["t"] = pd.to_datetime(doses["time"], utc=True).dt.tz_localize(None) + AEDT


def settled_at(t, values, low, high, start):
    """First time from start the value enters [low, high] and stays HOLD."""
    inside = (values >= low) & (values <= high)
    entered = None
    for ti, ok in zip(t, inside):
        if ti < start:
            continue
        if ok and entered is None:
            entered = ti
        elif not ok:
            entered = None
        if entered is not None and ti - entered >= HOLD:
            return entered
    return None


start = r["t"].iloc[0]
lvl_set = settled_at(r["t"], r["level_med"], 40, 50, start)
tds_col = "tds_ppm_ema" if r["tds_ppm_ema"].notna().any() else "tds_ppm"
first_ec = doses[doses.pump == "nutrient"]["t"].min() if not doses.empty else start
tds_set = settled_at(r["t"], r[tds_col], 550, 850, first_ec if pd.notna(first_ec) else start)
ph_col = "ph_ema" if r["ph_ema"].notna().any() else "ph"
ph_set = settled_at(r["t"], r[ph_col], 5.5, 6.5, first_ec if pd.notna(first_ec) else start)
for name, when, ref in (("level", lvl_set, start), ("TDS", tds_set, first_ec), ("pH", ph_set, first_ec)):
    print(f"{name:5s} settled: {when:%H:%M}  ({(when - ref).total_seconds() / 60:.0f} min after its start)"
          if when is not None and ref is not None and pd.notna(ref) else f"{name:5s} not settled yet")

fig, (ax_l, ax_t, ax_p) = plt.subplots(3, 1, figsize=(6.5, 6.4), sharex=True,
                                       gridspec_kw={"height_ratios": [1, 1.4, 1.4]})
pump = r[r.get("water_pump_on", False) == True]  # noqa: E712
for ax in (ax_l, ax_t, ax_p):
    for t0 in pump["t"]:
        ax.axvspan(t0, t0 + timedelta(seconds=10), color=rs.CYAN, alpha=0.15, lw=0)

ax_l.axhspan(40, 50, color=rs.GREEN, alpha=0.12, lw=0, label="band 15-25 mm below pots")
ax_l.axhline(45, color=rs.GREEN, lw=0.8, ls="--", label="target 20 mm (45 %)")
ax_l.plot(r["t"], r["level"], ".", ms=2, color=rs.CYAN, alpha=0.7, label="stick (5 % steps)")
ax_l.plot(r["t"], r["level_med"], color=rs.BLUE, lw=1.3, label="30 s median")
ax_l.set_ylabel("Level (%)")
ax_l.set_ylim(15, 55)
ax_l.legend(loc="lower right", fontsize=6.5, ncol=2)

ax_t.axhspan(600, 800, color=rs.GREEN, alpha=0.12, lw=0, label="target 600-800 ppm")
ax_t.axhline(550, color=rs.RED, ls=":", lw=1, label="dose limit 550 ppm")
ax_t.plot(r["t"], r["tds_ppm"], ".", ms=1.5, color=rs.CYAN, alpha=0.6, label="raw")
ax_t.plot(r["t"], r[tds_col], color=rs.BLUE, lw=1.3, label="control value")
ax_p.axhspan(5.5, 6.5, color=rs.GREEN, alpha=0.12, lw=0, label="band 5.5-6.5")
ax_p.plot(r["t"], r[ph_col], color=rs.BLUE, lw=1.3, label="pH")
if not doses.empty:
    for _, d in doses.iterrows():
        ax = ax_t if d.pump == "nutrient" else ax_p
        color = rs.GREEN if d.pump == "nutrient" else rs.PURPLE
        ax.axvline(d.t, color=color, ls="--", lw=0.9)
        ax.annotate(f"{d.delivered_ml:.1f} mL", (d.t, 0.04), xycoords=("data", "axes fraction"),
                    rotation=90, fontsize=6.5, color=color, xytext=(2, 0), textcoords="offset points")
for ax, when, label in ((ax_l, lvl_set, "level"), (ax_t, tds_set, "TDS"), (ax_p, ph_set, "pH")):
    if when is not None:
        ax.axvline(when, color="black", lw=0.8)
        ax.annotate(f"{label} in band {when:%H:%M}", (when, 0.92), xycoords=("data", "axes fraction"),
                    xytext=(3, 0), textcoords="offset points", fontsize=7)
ax_t.set_ylabel("TDS (ppm)")
ax_t.legend(loc="lower right", fontsize=6.5, ncol=2)
ax_p.set_ylabel("pH")
ax_p.legend(loc="upper right", fontsize=6.5)
ax_p.set_xlabel("Time, 18 Oct 2026 (AEDT)")
ax_p.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
fig.suptitle("Level, nutrient and pH loops: recovery after two days without effective control",
             fontsize=9.5)
fig.tight_layout()
for ext in ("png", "pdf"):
    fig.savefig(HERE / f"recovery.{ext}", dpi=200)
