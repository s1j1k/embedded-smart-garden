"""Plots and summary for the 6 Oct 2026 plain-water bring-up (firmware 1.9.0).

Input: bringup_1006.csv.gz, MQTT telemetry and control events exported from the
Pi (scripts/pi_export_mqtt_telemetry.py with SINCE = 2026-10-06T09:00:00Z).
"""
import gzip
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import pandas as pd

DOSE_UPTIME_MS = 602010  # ec_check event: 32.7 mL nutrient delivered
DOSE_ML = 32.70537

d = pd.read_csv("bringup_1006.csv.gz", low_memory=False)
r = d[d.kind == "reading"].copy()
ev = d[d.kind == "control_event"].copy()
r["t_min"] = (r.uptime_ms - DOSE_UPTIME_MS) / 60000
ev["t_min"] = (ev.uptime_ms - DOSE_UPTIME_MS) / 60000

cols = ["timestamp", "uptime_ms", "t_min", "tds_ppm", "tds_ppm_ema", "tds_ppm_nutrient",
        "tds_ph_salt_ppm", "ph_raw", "ph", "ph_filtered", "ph_voltage", "water_temp_c",
        "water_level_pct", "ph_session_ml", "ec_session_ml", "gain_ec", "gain_ph_up",
        "sched.activity"]
r[[c for c in cols if c in r]].to_csv("readings.csv", index=False)
ecols = ["timestamp", "uptime_ms", "t_min", "event", "ec_before", "ec_dose_ml", "ec_after",
         "ph_before", "ph_dose_dir", "ph_dose_ml", "ph_after", "gain_ec", "gain_up",
         "water_temp_c", "cumulative_session_ml"]
ev[[c for c in ecols if c in ev]].to_csv("control_events.csv", index=False)

# Step response: baseline before the dose, final value 20-24 min after.
y0 = r[r.t_min < 0].tds_ppm.mean()
yf = r[(r.t_min > 20) & (r.t_min < 24)].tds_ppm.mean()
band = 0.05 * (yf - y0)
settle = {}
for c in ["tds_ppm", "tds_ppm_ema"]:
    s = r[["t_min", c]].dropna()
    settle[c] = s[(s[c] - yf).abs() > band].t_min.max()

fig, ax = plt.subplots(3, 1, figsize=(9, 9), sharex=True)
ax[0].plot(r.t_min, r.tds_ppm, ".", ms=3, label="raw TDS")
ax[0].plot(r.t_min, r.tds_ppm_ema, label="control filter")
ax[0].plot(r.t_min, r.tds_ppm_nutrient, "--", label="nutrient-only (salt-corrected)")
ax[0].axhspan(yf - band, yf + band, color="g", alpha=0.15, label="final ±5 %")
ax[0].axhline(850, color="r", lw=0.8, ls=":", label="dead limit 850")
ax[0].axvline(6, color="k", lw=0.8, ls="--", label="old wait 6 min")
ax[0].axvline(9, color="k", lw=0.8, label="new wait 9 min")
ax[0].set_ylabel("TDS (ppm)")
ax[0].legend(fontsize=7, loc="lower right")
ax[0].set_title(f"Nutrient step {DOSE_ML:.1f} mL: {y0:.0f} -> {yf:.0f} ppm, "
                f"gain {(yf - y0) / DOSE_ML:.1f} ppm/mL; 5 % settle raw "
                f"{settle['tds_ppm']:.1f} min, filtered {settle['tds_ppm_ema']:.1f} min",
                fontsize=9)
ax[1].plot(r.t_min, r.ph_raw if "ph_raw" in r else r.ph, ".", ms=3, label="raw pH")
ax[1].plot(r.t_min, r.ph_filtered, label="control filter")
ax[1].axhspan(5.5, 6.5, color="g", alpha=0.15, label="dead limits")
for _, e in ev[ev.ph_dose_ml.notna()].iterrows():
    ax[1].axvline(e.t_min, color="m", lw=0.8)
ax[1].set_ylabel("pH")
ax[1].legend(fontsize=7)
ax[2].plot(r.t_min, r.ph_session_ml, label="pH-up session (mL)")
ax[2].plot(r.t_min, r.water_level_pct, label="water level (%)")
ax[2].plot(r.t_min, r.water_temp_c, label="water temp (°C)")
ax[2].set_xlabel("minutes after nutrient dose")
ax[2].legend(fontsize=7)
fig.tight_layout()
fig.savefig("bringup_2026-10-06.png", dpi=130)

zoom = r[(r.t_min > -2) & (r.t_min < 30)]
plt.figure(figsize=(7, 4))
plt.plot(zoom.t_min, zoom.tds_ppm, ".", ms=4, label="raw")
plt.plot(zoom.t_min, zoom.tds_ppm_ema, label="control filter")
plt.axhspan(yf - band, yf + band, color="g", alpha=0.15, label="final ±5 %")
plt.axvline(9, color="k", lw=0.8, label="settle wait 9 min")
plt.xlabel("minutes after dose"); plt.ylabel("TDS (ppm)"); plt.legend(fontsize=8)
plt.title("TDS step response, 6 Oct 2026", fontsize=10)
plt.tight_layout(); plt.savefig("tds_step_settle.png", dpi=130)
print(f"y0 {y0:.1f} yf {yf:.1f} gain {(yf - y0) / DOSE_ML:.2f} settle {settle}")
