"""Pull the 18 Oct TDS on/off test from the Pi's SQLite into tds_onoff.csv.

Run on the Pi (reads the dashboard database read-only):
    ssh admin@<pi> 'python3 - 2026-10-18T04:45 2026-10-18T05:50' < extract_window.py > tds_onoff.csv

Arguments are the UTC start and end of the window. Telemetry rows only.
The id range is found by binary search: received_at has no index.
"""
import csv
import json
import sqlite3
import sys

DB = "file:/home/admin/smart-garden-iot-dashboard/data/sensor_readings.sqlite?mode=ro"
start, end = sys.argv[1], sys.argv[2]
c = sqlite3.connect(DB, uri=True)


def first_id(ts):
    lo, hi = c.execute("select min(id), max(id) from mqtt_messages").fetchone()
    while lo < hi:
        m = (lo + hi) // 2
        r = c.execute("select received_at from mqtt_messages where id >= ? order by id limit 1",
                      (m,)).fetchone()[0]
        if r < ts:
            lo = m + 1
        else:
            hi = m
    return lo


w = csv.writer(sys.stdout, lineterminator="\n")
w.writerow(["received_at_utc", "ph", "ph_voltage", "ph_ema", "tds_ppm", "water_temp_c"])
for ra, p in c.execute(
        "select received_at, payload_json from mqtt_messages "
        "where id >= ? and topic like '%/telemetry' order by id", (first_id(start),)):
    if ra >= end:
        break
    d = json.loads(p)
    if d.get("ph") is None:
        continue
    w.writerow([ra, d.get("ph"), d.get("ph_voltage"), d.get("ph_ema"), d.get("tds_ppm"),
                d.get("water_temp_c")])
