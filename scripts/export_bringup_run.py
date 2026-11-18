#!/usr/bin/env python3
"""Export a bring-up run from the Pi and build its analysis CSVs.

Pulls every MQTT message since ``--since`` from the Pi's SQLite store (over SSH,
read-only), then writes into ``--run-dir``:

  raw/mqtt_messages.jsonl.gz  the messages as stored (received_at, topic, payload)
  telemetry_full.csv.gz       every telemetry reading, all fields flattened
  sensor_readings.csv         the analysis columns of the same readings
  control_events.csv          every control event (checks, doses, rechecks, faults)
  commands.csv                commands to the controller and its acknowledgements
  actuator_events.csv         one row per chemistry dose, from the controller's own
                              events: when it was planned, pump, requested and
                              delivered mL, the reading it was planned from and the
                              settled reading at its recheck (a 12 s pump run is
                              usually missed between telemetry samples)
  milestones.csv              plants in, resume, first dose, pH and TDS each first
                              inside their dead limits, both in band, settled

    python scripts/export_bringup_run.py --run-dir docs/bringup_2026-10-14_plants \\
        --since 2026-10-13T14:00:00Z --plants-in 2026-10-13T14:31:00Z

Milestones need 10 min inside the dead limits ("first in") or 60 min ("held", so
a dose that dips inside and rebounds is not counted). Settled = TDS and pH both
inside their dead limits with no dose for 40 min.
SSH: PI_HOST (default admin@192.168.4.30), PI_SSH_KEY (default ~/.ssh/smart_garden_pi).
"""

from __future__ import annotations

import argparse
import gzip
import io
import json
import os
import subprocess
from pathlib import Path

import pandas as pd

REMOTE = r'''
import gzip, json, sqlite3, sys
c = sqlite3.connect("file:/home/admin/smart-garden-iot-dashboard/data/sensor_readings.sqlite?mode=ro", uri=True)
out = gzip.GzipFile(fileobj=sys.stdout.buffer, mode="wb")
for rec, topic, pj in c.execute(
        "select received_at, topic, payload_json from mqtt_messages where received_at >= ? order by id",
        (sys.argv[1],)):
    out.write((json.dumps({"received_at": rec, "topic": topic, "payload": pj}) + "\n").encode())
out.close()
'''

SENSOR_COLUMNS = [
    "time", "firmware_version", "boot_id", "system_mode", "auto_control_paused",
    "ph", "ph_ema", "tds_ppm", "tds_ppm_ema", "water_temp_c", "ambient_temp_c",
    "ambient_humidity_pct", "water_level_pct", "tank_volume_l",
    "ph_dead_low", "ph_dead_high", "ec_dead_low", "ec_dead_high",
    "nutrient_pump_on", "ph_up_pump_on", "ph_down_pump_on", "water_pump_on",
    "nutrient_rem_ml", "ph_up_rem_ml", "ph_down_rem_ml", "water_rem_ml",
    "ph_session_ml", "ec_session_ml", "ph_down_no_response", "ph_up_no_response",
    "solution.status", "solution.due", "solution.watch", "solution.ph_out_h",
    "solution.buffer_ratio", "solution.reagent_ml_per_l",
]
PUMPS = {"nutrient": "tds_ppm_ema", "ph_up": "ph_ema", "ph_down": "ph_ema", "water": "water_level_pct"}
SETTLED_FOR = pd.Timedelta(minutes=40)
IN_BAND_FOR = pd.Timedelta(minutes=10)   # a milestone needs this long inside, not one sample
HELD_FOR = pd.Timedelta(minutes=60)      # "held": inside and still inside an hour later


def first_sustained(t: pd.DataFrame, inside: pd.Series, hold: pd.Timedelta = IN_BAND_FOR):
    """Time of the first stretch of ``inside`` readings lasting at least ``hold``."""
    start = None
    for i in t.index:
        if inside[i]:
            start = t.at[i, "time"] if start is None else start
            if t.at[i, "time"] - start >= hold:
                return start
        else:
            start = None
    return None


def _flat(d, prefix=""):
    out = {}
    for k, v in d.items():
        key = prefix + k
        if isinstance(v, dict):
            out.update(_flat(v, key + "."))
        elif isinstance(v, list):
            out[key] = "|".join(map(str, v))
        else:
            out[key] = v
    return out


def fetch(since: str) -> list[dict]:
    host = os.getenv("PI_HOST", "admin@192.168.4.30")
    key = os.path.expanduser(os.getenv("PI_SSH_KEY", "~/.ssh/smart_garden_pi"))
    raw = subprocess.run(
        ["ssh", "-i", key, "-o", "IdentitiesOnly=yes", "-o", "BatchMode=yes", host,
         f"python3 - {since}"], input=REMOTE.encode(), capture_output=True, check=True).stdout
    return [json.loads(line) for line in gzip.decompress(raw).decode().splitlines() if line]


def by_topic(messages, suffix):
    rows = []
    for m in messages:
        if not m["topic"].endswith(suffix):
            continue
        try:
            payload = json.loads(m["payload"])
        except (TypeError, ValueError):
            continue
        rows.append({"received_at": m["received_at"], **_flat(payload)})
    return pd.DataFrame(rows)


def telemetry(messages) -> pd.DataFrame:
    t = by_topic(messages, "/telemetry")
    if t.empty:
        return t
    # One row per reading: live and replayed copies share (boot_id, seq).
    t = t.drop_duplicates(subset=[c for c in ("boot_id", "seq", "timestamp") if c in t])
    t["time"] = pd.to_datetime(t["timestamp"].where(t["timestamp"].astype(str).str[:2] == "20",
                                                    t["received_at"]), utc=True, errors="coerce")
    return t.dropna(subset=["time"]).sort_values("time").reset_index(drop=True)


def dose_events(events: pd.DataFrame) -> pd.DataFrame:
    """One row per nutrient / pH dose: the check that planned it and its recheck."""
    if events.empty or "event" not in events:
        return pd.DataFrame()
    e = events.copy()
    e["time"] = pd.to_datetime(e["timestamp"], utc=True, errors="coerce")
    rows, planned = [], {}
    # The controller's own order: a recheck and the next check share a timestamp.
    order = [c for c in ("boot_id", "event_seq") if c in e] or ["time"]
    for _, r in e.sort_values(order).iterrows():
        ev = r["event"]
        if ev == "ec_check" and pd.notna(r.get("ec_dose_ml")):
            planned["ec"] = r
        elif ev == "ph_check":
            planned["ph"] = r
        elif ev == "ec_recheck" and "ec" in planned:
            p = planned.pop("ec")
            rows.append({"time": p["time"], "pump": "nutrient", "requested_ml": p.get("ec_dose_ml"),
                         "delivered_ml": r.get("ec_dose_ml"), "reading_before": p.get("ec_before"),
                         "reading_after": r.get("ec_after"), "unit": "ppm", "recheck_time": r["time"]})
        elif ev == "ph_recheck" and pd.notna(r.get("ph_dose_ml")) and "ph" in planned:
            p = planned.pop("ph")
            rows.append({"time": p["time"], "pump": f"ph_{r.get('ph_dose_dir')}",
                         "requested_ml": None, "delivered_ml": r.get("ph_dose_ml"),
                         "reading_before": r.get("ph_before"), "reading_after": r.get("ph_after"),
                         "unit": "pH", "recheck_time": r["time"]})
    out = pd.DataFrame(rows)
    if not out.empty:
        out["change"] = out["reading_after"] - out["reading_before"]
        out["change_per_ml"] = out["change"] / out["delivered_ml"]
    return out


def actuator_events(t: pd.DataFrame) -> pd.DataFrame:
    """Pump runs seen in telemetry (pump_on samples); misses most short doses."""
    rows = []
    for pump, reading in PUMPS.items():
        on, rem = f"{pump}_pump_on", f"{pump}_rem_ml"
        if on not in t or rem not in t:
            continue
        state = t[on].fillna(0).astype(int)
        starts = t.index[(state == 1) & (state.shift(fill_value=0) == 0)]
        for i in starts:
            start = t.at[i, "time"]
            before = t[t["time"] < start].tail(1)
            after = t[t["time"] > start + pd.Timedelta(minutes=1)].head(1)
            later = t[t["time"] >= start + pd.Timedelta(minutes=20)].head(1)
            rows.append({
                "time": start, "pump": pump,
                "accounted_ml": (float(before[rem].iloc[0]) - float(after[rem].iloc[0]))
                if len(before) and len(after) else None,
                f"{reading}_before": before[reading].iloc[0] if len(before) and reading in t else None,
                f"{reading}_20min": later[reading].iloc[0] if len(later) and reading in t else None,
            })
    return pd.DataFrame(rows).sort_values("time") if rows else pd.DataFrame()


def milestones(t, events, doses, plants_in) -> pd.DataFrame:
    rows = []
    if plants_in:
        rows.append(("plants_in", pd.Timestamp(plants_in)))
    if not events.empty and "event" in events:
        resumed = events[events["event"] == "auto_resume"]
        if len(resumed):
            rows.append(("auto_resume", pd.to_datetime(resumed["received_at"].iloc[-1], utc=True)))
    if not doses.empty:
        rows.append(("first_dose", doses["time"].iloc[0]))
    ph_in = (t["ph_ema"] >= t["ph_dead_low"]) & (t["ph_ema"] <= t["ph_dead_high"])
    tds_in = (t["tds_ppm_ema"] >= t["ec_dead_low"]) & (t["tds_ppm_ema"] <= t["ec_dead_high"])
    for loop, inside in (("ph", ph_in), ("tds", tds_in)):
        when = first_sustained(t, inside)
        if when is not None:
            rows.append((f"{loop}_first_in_dead_limits", when))
        # Entered and then stayed: a dose that dips inside and rebounds does not count.
        held = first_sustained(t, inside, HELD_FOR)
        if held is not None:
            rows.append((f"{loop}_held_in_dead_limits", held))
    band = ph_in & tds_in
    if band.any() and first_sustained(t, band) is not None:
        rows.append(("first_in_band", first_sustained(t, band)))
        dose_times = list(doses["time"]) if not doses.empty else []
        for i in t.index[band]:
            start = t.at[i, "time"]
            window = t[(t["time"] >= start) & (t["time"] <= start + SETTLED_FOR)]
            if window["time"].iloc[-1] - start >= SETTLED_FOR * 0.95 and band[window.index].all() \
                    and not any(start < d <= start + SETTLED_FOR for d in dose_times):
                rows.append(("settled", start))
                break
    out = pd.DataFrame(rows, columns=["milestone", "time_utc"])
    out["time_utc"] = pd.to_datetime(out["time_utc"], utc=True)
    out["time_local"] = out["time_utc"].dt.tz_convert("Australia/Melbourne")
    start = out.loc[out["milestone"] == "auto_resume", "time_utc"]
    if len(start):
        out["minutes_from_resume"] = ((out["time_utc"] - start.iloc[0]).dt.total_seconds() / 60).round(1)
    return out


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--run-dir", required=True, type=Path)
    p.add_argument("--since", required=True, help="UTC ISO time, e.g. 2026-10-13T14:00:00Z")
    p.add_argument("--plants-in", help="UTC ISO time the plants went in")
    args = p.parse_args()
    run = args.run_dir
    (run / "raw").mkdir(parents=True, exist_ok=True)

    messages = fetch(args.since)
    with gzip.open(run / "raw" / "mqtt_messages.jsonl.gz", "wt", encoding="utf-8") as f:
        for m in messages:
            f.write(json.dumps(m) + "\n")

    t = telemetry(messages)
    t.to_csv(run / "telemetry_full.csv.gz", index=False)
    t[[c for c in SENSOR_COLUMNS if c in t]].to_csv(run / "sensor_readings.csv", index=False)
    events = by_topic(messages, "/events")
    events.to_csv(run / "control_events.csv", index=False)
    cmds = pd.concat([by_topic(messages, "/cmd").assign(kind="cmd"),
                      by_topic(messages, "/ack").assign(kind="ack")], ignore_index=True)
    if not cmds.empty:
        cmds = cmds[~cmds.get("cmd", pd.Series(dtype=str)).astype(str).eq("light_flash")]
        cmds.sort_values("received_at").to_csv(run / "commands.csv", index=False)
    doses = dose_events(events)
    doses.to_csv(run / "actuator_events.csv", index=False)
    ms = milestones(t, events, doses, args.plants_in)
    ms.to_csv(run / "milestones.csv", index=False)
    print(f"{len(messages)} messages, {len(t)} readings, {len(events)} events, "
          f"{len(doses)} pump runs -> {run}")
    print(ms.to_string(index=False))


if __name__ == "__main__":
    main()
