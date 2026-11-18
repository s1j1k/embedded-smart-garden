"""Export one run's readings and control events from the Pi's SQLite (read-only)."""
import csv
import json
import sqlite3
import sys

sys.path.insert(0, "/home/admin/smart-garden-iot-dashboard/src")
import sqlite_store  # noqa: E402

DB = "/home/admin/smart-garden-iot-dashboard/data/sensor_readings.sqlite"
START, END = sys.argv[1], sys.argv[2]

conn = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
first = sqlite_store.first_id_since(conn, START)
last = sqlite_store.first_id_since(conn, END)
wanted = ["received_at", "timestamp", "tds_ppm", "ph", "ph_filtered", "water_level_pct",
          "tank_volume_l", "current_drawdown_mm", "target_drawdown_mm", "water_pump_on",
          "nutrient_pump_on", "ph_up_pump_on", "ph_down_pump_on", "ec_dead_low", "ec_dead_high",
          "ph_dead_low", "ph_dead_high", "water_temp_c"]
have = {r[1] for r in conn.execute("PRAGMA table_info(sensor_readings)")}
cols = [c for c in wanted if c in have]
rows = conn.execute(f"SELECT {', '.join(cols)} FROM sensor_readings WHERE id >= ? AND id < ? ORDER BY id",
                    (first, last)).fetchall()
with open("/tmp/run_readings.csv", "w", newline="") as f:
    writer = csv.writer(f)
    writer.writerow(cols)
    writer.writerows(rows)

events = [e for e in sqlite_store.load_control_events(DB, START) if e.get("timestamp", "") < END]
others = [e for e in events if e.get("event") != "level_check"]
levels = [e for e in events if e.get("event") == "level_check"]
with open("/tmp/run_events.json", "w") as f:
    json.dump({"events": others, "level_checks": levels}, f)
print(len(rows), "readings;", len(others), "events;", len(levels), "level checks")
if levels:
    print("level check keys:", sorted(levels[0].keys()))
