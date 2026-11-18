#!/usr/bin/env python3
"""Build the analysis CSVs for the autonomous vegetative run from 30 Sep 2026.

Inputs (docs/autonomous_run_2026-09-30/raw/):
  pi_mqtt_telemetry_dedup.csv.gz  every MQTT telemetry payload the Pi stored
                                  (scripts/pi_export_mqtt_telemetry.py), one row
                                  per distinct (timestamp, uptime_ms, boot_count)
  control_events_device.csv       /control_events.csv from LittleFS (DUMP CTRL)
  influx_*.csv                    Influx cross-check (sparse; uploads were failing)

Outputs (docs/autonomous_run_2026-09-30/):
  telemetry_full.csv.gz   every reading, every field (live rows + replay rows
                          that fill gaps in the live stream)
  sensor_readings.csv     same rows, the columns needed for analysis/simulation
  control_events.csv      controller decisions since 30 Sep (device log)
  actuator_events.csv     one row per pump start, with commanded and
                          controller-accounted volume
  milestones.csv          plain-water start and time to the vegetative band

    python scripts/build_autonomous_run.py
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
RUN = ROOT / "docs" / "autonomous_run_2026-09-30"
RAW = RUN / "raw"
TZ = "Australia/Melbourne"  # AEST until 4 Oct 02:00, AEDT after
SINCE = pd.Timestamp("2026-09-30T00:00:00Z")
OVERRUN_ML = 3.0

PUMPS = {
    "nutrient": ("nutrient_pump_on", "nutrient_rem_ml"),
    "ph_up": ("ph_up_pump_on", "ph_up_rem_ml"),
    "ph_down": ("ph_down_pump_on", "ph_down_rem_ml"),
    "water": ("water_pump_on", "water_rem_ml"),
    "drain": ("drain_pump_on", None),
}

CORE_COLUMNS = [
    "time_utc", "time_local", "hours_since_plain_water", "row_source",
    "firmware_version", "uptime_ms", "boot_count", "reset_reason",
    "grow_stage", "auto_control_paused", "sched.activity",
    # pH
    "ph_voltage", "ph_raw", "ph_filtered", "ph_ema", "ph", "ph_temp_c",
    "ph_comp_factor", "ph_sensor_fault", "ph_dose",
    "ph_dead_low", "ph_dead_high", "ph_center", "ph_target_min", "ph_target_max",
    # TDS
    "tds_ppm", "tds_ppm_ema", "tds_ppm_nutrient", "tds_ph_salt_ppm",
    "tds_ph_up_ml", "tds_ph_down_ml", "tds_comp_coeff", "nutrient_dose",
    "ec_dead_low", "ec_dead_high", "nutrient_tds_min", "nutrient_tds_max",
    # actuators and controller state
    "nutrient_pump_on", "ph_up_pump_on", "ph_down_pump_on", "water_pump_on",
    "drain_pump_on", "grow_light_on", "grow_light_brightness_pct",
    "nutrient_rem_ml", "ph_up_rem_ml", "ph_down_rem_ml", "water_rem_ml",
    "gain_ec", "gain_ph_up", "gain_ph_down",
    "sched.ec.in_s", "sched.ph.in_s", "sched.ph.hold", "sched.ec.hold",
    # environment
    "water_temp_c", "water_level_pct", "water_level_raw_pct", "current_drawdown_mm",
    "ambient_temp_c", "ambient_humidity_pct", "co2_ppm", "light_adc",
    "wifi_rssi_dbm", "free_heap_b", "alerts",
]


def load_telemetry() -> pd.DataFrame:
    d = pd.read_csv(RAW / "pi_mqtt_telemetry_dedup.csv.gz", low_memory=False).copy()
    d["t"] = pd.to_datetime(d["timestamp"], utc=True)
    d = d[d["t"] >= SINCE]
    live = d[d["firmware_version"].notna()].copy()
    live["row_source"] = "live"
    # Replayed rows from the device's CSV buffer carry fewer fields and no
    # uptime; keep one only where no live row exists within 6 s.
    rep = d[d["firmware_version"].isna()].copy().sort_values("t")
    live = live.sort_values("t")
    near = pd.merge_asof(
        rep[["t"]], live[["t"]].rename(columns={"t": "t_live"}),
        left_on="t", right_on="t_live", direction="nearest",
    )
    gap = (near["t"] - near["t_live"]).abs() > pd.Timedelta(seconds=6)
    rep = rep[gap.to_numpy() | near["t_live"].isna().to_numpy()]
    rep["row_source"] = "replay"
    out = pd.concat([live, rep]).sort_values(["t", "row_source"]).reset_index(drop=True)
    return out


def plain_water_start(d: pd.DataFrame) -> pd.Timestamp:
    """Refill with plain water: first reading under 30 ppm after the 1100 ppm bench test."""
    x = d[d["t"] >= pd.Timestamp("2026-09-30T12:00:00Z")]
    return x.loc[x["tds_ppm"] < 30, "t"].iloc[0]


def actuator_events(d: pd.DataFrame, ctrl: pd.DataFrame) -> pd.DataFrame:
    live = d[d["row_source"] == "live"].set_index("t").sort_index()
    rows = []
    for name, (flag, rem) in PUMPS.items():
        on = d.set_index("t").sort_index()[flag].fillna(0).astype(float) > 0
        starts = on.index[on & ~on.shift(fill_value=False)]
        for t0 in starts:
            run = on.loc[t0:]
            stop = run.index[~run.to_numpy()][0] if (~run).any() else pd.NaT
            row = {
                "time_utc": t0, "pump": name,
                "rows_on": int(on.loc[t0:stop].sum()) if stop is not pd.NaT else int(run.sum()),
                "flag_off_utc": stop,
            }
            if rem:
                before = live.loc[: t0 - pd.Timedelta(seconds=1), rem].dropna()
                after = live.loc[t0 + pd.Timedelta(seconds=60): t0 + pd.Timedelta(minutes=10), rem].dropna()
                if len(before) and len(after):
                    row["controller_accounted_ml"] = round(before.iloc[-1] - after.iloc[0], 3)
            # Commanded dose from the control-event log.
            if name == "nutrient":
                m = ctrl[(ctrl["event"] == "ec_check") & ((ctrl["t"] - t0).abs() < pd.Timedelta(seconds=60))]
                if len(m):
                    row["commanded_ml"] = m["ec_dose_ml"].iloc[0]
                    row["controller_input_before"] = m["ec_before"].iloc[0]
                    rc = ctrl[(ctrl["event"] == "ec_recheck") & (ctrl["t"] > t0) & (ctrl["t"] < t0 + pd.Timedelta(hours=1))]
                    if len(rc):
                        row["recheck_utc"] = rc["t"].iloc[0]
                        row["controller_input_after"] = rc["ec_before"].iloc[0]
            elif name.startswith("ph"):
                rc = ctrl[(ctrl["event"] == "ph_recheck") & (ctrl["t"] > t0) & (ctrl["t"] < t0 + pd.Timedelta(minutes=30))]
                if len(rc):
                    row["commanded_ml"] = rc["ph_dose_ml"].iloc[0]
                    row["controller_input_after"] = rc["ph_before"].iloc[0]
                    row["recheck_utc"] = rc["t"].iloc[0]
                pre = live.loc[: t0 - pd.Timedelta(seconds=1), "ph_ema"].dropna()
                if len(pre):
                    row["controller_input_before"] = round(pre.iloc[-1], 3)
            rows.append(row)
    ev = pd.DataFrame(rows).sort_values("time_utc").reset_index(drop=True)
    if "commanded_ml" in ev and "controller_accounted_ml" in ev:
        # Doses run in whole pump bursts (0.29-0.41 mL), so a normal dose books
        # 0.5-0.9 mL. A runaway books ~7.6 mL.
        ev["overrun"] = (ev["controller_accounted_ml"] > OVERRUN_ML).astype(int)
    return ev


def milestones(d: pd.DataFrame, t0: pd.Timestamp, ev: pd.DataFrame) -> pd.DataFrame:
    x = d[(d["t"] >= t0) & (d["row_source"] == "live")]
    # The EMA still holds the pre-refill value for a few minutes; start the
    # threshold search once it has settled on the plain water.
    settled = x.loc[x["tds_ppm_ema"] < 30, "t"].iloc[0]
    x = x[x["t"] >= settled]
    first = lambda mask: x.loc[mask, "t"].iloc[0] if mask.any() else pd.NaT  # noqa: E731
    tds = x["tds_ppm_ema"]
    ph = x["ph_ema"]
    m = {
        "plain_water_refill": t0,
        "tds_ema_settled_on_plain_water": settled,
        "first_autonomous_nutrient_dose": ev.loc[(ev.pump == "nutrient") & (ev.time_utc >= t0), "time_utc"].iloc[0],
        "tds_ema_ge_100ppm": first(tds >= 100),
        "tds_ema_ge_300ppm": first(tds >= 300),
        "tds_ema_ge_550ppm_deadband": first(tds >= 550),
        "tds_ema_ge_600ppm_target": first(tds >= 600),
    }
    t_tds = m["tds_ema_ge_550ppm_deadband"]
    after = x[x["t"] >= t_tds]
    inband = (after["ph_ema"] >= 5.9) & (after["ph_ema"] <= 6.1)
    m["ph_ema_in_5.9_6.1_after_tds_in_band"] = after.loc[inband, "t"].iloc[0] if inband.any() else pd.NaT
    rows = []
    for k, t in m.items():
        rows.append({
            "milestone": k,
            "time_utc": t,
            "time_local": t.tz_convert(TZ) if pd.notna(t) else pd.NaT,
            "hours_since_plain_water": round((t - t0).total_seconds() / 3600, 2) if pd.notna(t) else np.nan,
        })
    # Fraction of time in band once both loops first reached it.
    t_both = m["ph_ema_in_5.9_6.1_after_tds_in_band"]
    w = x[x["t"] >= t_both]
    rows.append({"milestone": "pct_time_tds_ema_550_850_after_entry",
                 "hours_since_plain_water": round(100 * ((w.tds_ppm_ema >= 550) & (w.tds_ppm_ema <= 850)).mean(), 1)})
    rows.append({"milestone": "pct_time_ph_ema_5.9_6.1_after_entry",
                 "hours_since_plain_water": round(100 * ((w.ph_ema >= 5.9) & (w.ph_ema <= 6.1)).mean(), 1)})
    rows.append({"milestone": "pct_time_ph_ema_5.5_6.5_after_entry",
                 "hours_since_plain_water": round(100 * ((w.ph_ema >= 5.5) & (w.ph_ema <= 6.5)).mean(), 1)})
    return pd.DataFrame(rows)


def main() -> None:
    d = load_telemetry()
    t0 = plain_water_start(d)
    d.insert(0, "time_utc", d["t"].dt.strftime("%Y-%m-%dT%H:%M:%SZ"))
    d.insert(1, "time_local", d["t"].dt.tz_convert(TZ).dt.strftime("%Y-%m-%d %H:%M:%S%z"))
    d.insert(2, "hours_since_plain_water", ((d["t"] - t0).dt.total_seconds() / 3600).round(5))

    ctrl = pd.read_csv(RAW / "control_events_device.csv")
    ctrl["t"] = pd.to_datetime(ctrl["recorded_at_utc"], utc=True)
    ctrl = ctrl[ctrl["t"] >= SINCE].copy()
    ctrl.insert(1, "time_local", ctrl["t"].dt.tz_convert(TZ).dt.strftime("%Y-%m-%d %H:%M:%S%z"))
    ctrl.insert(2, "hours_since_plain_water", ((ctrl["t"] - t0).dt.total_seconds() / 3600).round(4))

    ev = actuator_events(d, ctrl)
    ms = milestones(d, t0, ev)

    full = d.drop(columns=["t"])
    full.to_csv(RUN / "telemetry_full.csv.gz", index=False, compression="gzip")
    full[[c for c in CORE_COLUMNS if c in full]].to_csv(RUN / "sensor_readings.csv", index=False)
    ctrl.drop(columns=["t"]).to_csv(RUN / "control_events.csv", index=False)
    for c in ("time_utc", "flag_off_utc", "recheck_utc"):
        if c in ev:
            ev[c] = pd.to_datetime(ev[c], utc=True)
    ev.insert(1, "time_local", ev["time_utc"].dt.tz_convert(TZ).dt.strftime("%Y-%m-%d %H:%M:%S%z"))
    ev.insert(2, "hours_since_plain_water", ((ev["time_utc"] - t0).dt.total_seconds() / 3600).round(4))
    ev.to_csv(RUN / "actuator_events.csv", index=False, date_format="%Y-%m-%dT%H:%M:%SZ")
    ms.to_csv(RUN / "milestones.csv", index=False, date_format="%Y-%m-%dT%H:%M:%SZ")

    print(f"rows {len(full)} (live {int((d.row_source == 'live').sum())}, replay {int((d.row_source == 'replay').sum())}), cols {full.shape[1]}")
    print(f"control events {len(ctrl)}, actuator events {len(ev)}")
    print(ms.to_string(index=False))


if __name__ == "__main__":
    main()
