"""Recovery test on 18 Oct 2026: water top-up, then nutrient, then the pH check.

    python plot_full_cycle.py   ->  full_cycle.png

Reads run_readings.csv (10 s readings from the Pi's SQLite) and run_events.json
(controller events), both exported by export_run.py.
"""
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.dates as mdates
import matplotlib.pyplot as plt
import pandas as pd

HERE = Path(__file__).resolve().parent
TZ = "Australia/Sydney"
START, END = pd.Timestamp("2026-10-18 19:55", tz=TZ), pd.Timestamp("2026-10-18 22:20", tz=TZ)
WATER, NUTRIENT, PH = "#2f86c9", "#3a9d5d", "#7a5cc4"

d = pd.read_csv(HERE / "run_readings.csv")
d["t"] = pd.to_datetime(d["received_at"], utc=True).dt.tz_convert(TZ)
d = d[(d["t"] >= START) & (d["t"] <= END)]
events = json.loads((HERE / "run_events.json").read_text())["events"]
ev = pd.DataFrame(events)
ev["t"] = pd.to_datetime(ev["timestamp"], utc=True).dt.tz_convert(TZ)
ev = ev[(ev["t"] >= START) & (ev["t"] <= END)]
doses = ev[(ev["event"] == "ec_check") & pd.to_numeric(ev.get("ec_dose_ml"), errors="coerce").gt(0)]
ph_checks = ev[ev["event"] == "ph_check"]
water_on = d[d["water_pump_on"] == 1]["t"]

fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(8, 7.2), sharex=True, dpi=200,
                                    gridspec_kw={"height_ratios": [1, 1.3, 1.1]})
fig.suptitle("Recovery test, 18 Oct 2026: water top-up, then nutrient, then pH", fontsize=11)

level = d.set_index("t")["water_level_pct"].rolling("3min").median()
ax1.plot(level.index, level.values, color=WATER, lw=1.8, label="water level (3 min median)")
ax1.axhline(45, color=WATER, ls=":", lw=1, label="target 45 %")
for t in water_on:
    ax1.axvspan(t - pd.Timedelta(seconds=10), t + pd.Timedelta(seconds=10), color=WATER, alpha=0.25, lw=0)
ax1.set_ylabel("level (%)")
ax1.set_ylim(20, 55)
ax1.legend(loc="lower right", fontsize=7, frameon=False)
ax1.text(water_on.iloc[0], 52, " top-up", color=WATER, fontsize=8, va="top")

ax2.axhspan(600, 800, color=NUTRIENT, alpha=0.10, lw=0, label="target 600–800 ppm")
ax2.axhline(550, color="#c0392b", ls=":", lw=1, label="dose limit 550 ppm")
ax2.plot(d["t"], d["tds_ppm"], ".", color=NUTRIENT, ms=2, alpha=0.4, label="raw")
tds = d.set_index("t")["tds_ppm"].rolling("3min").median()
ax2.plot(tds.index, tds.values, color=NUTRIENT, lw=1.8, label="3 min median")
for _, row in doses.iterrows():
    ax2.axvline(row["t"], color=NUTRIENT, ls="--", lw=1)
    ax2.text(row["t"], 330, f" {float(row['ec_dose_ml']):.1f} mL", color=NUTRIENT, fontsize=7,
             rotation=90, va="bottom")
ax2.set_ylabel("TDS (ppm)")
ax2.set_ylim(320, 700)
ax2.legend(loc="lower right", fontsize=7, frameon=False, ncol=2)

ax3.axhspan(5.5, 6.5, color=PH, alpha=0.10, lw=0, label="dead limits 5.5–6.5")
ax3.plot(d["t"], d["ph"], color=PH, lw=1.5, label="pH")
for _, row in ph_checks.iterrows():
    ax3.axvline(row["t"], color=PH, ls=":", lw=1)
    ax3.text(row["t"], 5.6, " pH check: in range,\n no dose", color=PH, fontsize=7, va="bottom")
ax3.set_ylabel("pH")
ax3.set_ylim(5.4, 7.0)
ax3.legend(loc="upper right", fontsize=7, frameon=False)
ax3.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M", tz=TZ))
ax3.set_xlabel("local time, 18 Oct")

for ax in (ax1, ax2, ax3):
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", alpha=0.3)
fig.tight_layout()
fig.savefig(HERE / "full_cycle.png")

first_dose = doses["t"].min()
in_band = d[(d["t"] > first_dose) & (d.set_index("t")["tds_ppm"].rolling("3min").median().values >= 550)]["t"].min()
print(f"water bursts {water_on.min():%H:%M}-{water_on.max():%H:%M}; first nutrient dose {first_dose:%H:%M}; "
      f"TDS median >= 550 at {in_band:%H:%M} ({(in_band - first_dose).total_seconds() / 60:.0f} min); "
      f"{len(doses)} doses: {', '.join(f'{float(x):.2f}' for x in doses['ec_dose_ml'])} mL; "
      f"pH checks at {', '.join(ph_checks['t'].dt.strftime('%H:%M'))}")
