"""Plots and CSVs for the third bring-up, 7 Oct 2026 (firmware 1.9.4, boot_id 13).

Input: export_run3.csv.gz (Pi MQTT export since 09:00 UTC, both runs).
"""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

BOOT = 13
T0_MS = 601900  # first nutrient dose (ec_check), uptime ms

d = pd.read_csv("export_run3.csv.gz", low_memory=False)
d = d[d.boot_id == BOOT]
r = d[d.kind == "reading"].copy()
ev = d[d.kind == "control_event"].copy()
r["t_min"] = (r.uptime_ms - T0_MS) / 60000
ev["t_min"] = (ev.uptime_ms - T0_MS) / 60000
r[[c for c in ["timestamp", "uptime_ms", "t_min", "tds_ppm", "tds_ppm_ema", "ph", "ph_filtered",
   "water_temp_c", "water_level_pct", "tank_volume_l", "ec_session_ml", "ph_session_ml",
   "gain_ec", "sched.activity"] if c in r]].to_csv("readings.csv", index=False)
ev_cols = ["timestamp", "uptime_ms", "t_min", "event", "ec_before", "ec_dose_ml", "ec_after",
           "ph_before", "ph_dose_dir", "ph_dose_ml", "gain_ec", "cumulative_session_ml"]
ev[[c for c in ev_cols if c in ev]].to_csv("control_events.csv", index=False)

doses = ev[(ev.event == "ec_check") & ev.ec_dose_ml.notna()]
checks = ev[ev.event == "ec_check"]
fig, ax = plt.subplots(2, 1, figsize=(9, 7), sharex=True)
ax[0].plot(r.t_min, r.tds_ppm, ".", ms=3, label="raw TDS")
ax[0].plot(r.t_min, r.tds_ppm_ema, label="control filter")
ax[0].axhspan(600, 800, color="g", alpha=0.12, label="target 600-800")
ax[0].axhline(850, color="r", ls=":", label="dead limit 850")
ax[0].axhline(550, color="r", ls=":")
for _, e in checks.iterrows():
    ax[0].axvline(e.t_min, color="k", lw=0.6, ls="--")
for _, e in doses.iterrows():
    ax[0].annotate(f"{e.ec_dose_ml:.1f} mL", (e.t_min, 50), rotation=90, fontsize=8)
ax[0].set_ylabel("TDS (ppm)")
ax[0].set_title("Run 3: 13.6 + 3.3 mL (session budget used up); TDS stopped at ~440 ppm", fontsize=9)
ax[0].legend(fontsize=7, loc="upper left")
ax[1].plot(r.t_min, r.ph, ".", ms=3, label="pH")
ax[1].axhspan(5.5, 6.5, color="g", alpha=0.12, label="dead limits 5.5-6.5")
ax[1].set_ylabel("pH"); ax[1].set_xlabel("minutes after first nutrient dose")
ax[1].legend(fontsize=7)
fig.tight_layout(); fig.savefig("bringup_run3.png", dpi=130)
print(r.tds_ppm.iloc[-1], r.t_min.iloc[-1])
