import sqlite3, json, csv, gzip, sys
SINCE = "2026-09-30T00:00:00Z"
c = sqlite3.connect("file:/home/admin/smart-garden-iot-dashboard/data/sensor_readings.sqlite?mode=ro", uri=True)
def flat(d, p=""):
    out = {}
    for k, v in d.items():
        key = p + k
        if isinstance(v, dict): out.update(flat(v, key + "."))
        elif isinstance(v, list): out[key] = "|".join(map(str, v))
        else: out[key] = v
    return out
seen = {}; order = []; keys = {}
for mid, rec, topic, pj in c.execute("select id, received_at, topic, payload_json from mqtt_messages where received_at >= ? order by id", (SINCE,)):
    try: d = json.loads(pj)
    except Exception: continue
    k = (d.get("timestamp"), d.get("uptime_ms"), d.get("boot_count"))
    if k in seen:
        seen[k]["mqtt_copies"] += 1; continue
    row = {"mqtt_received_at": rec, "mqtt_id": mid, "mqtt_copies": 1}
    row.update(flat(d))
    seen[k] = row; order.append(k)
    for kk in row: keys.setdefault(kk, None)
rows = sorted((seen[k] for k in order), key=lambda r: (str(r.get("timestamp")), r.get("uptime_ms") or 0))
cols = list(keys)
with gzip.open("/tmp/telemetry_dedup.csv.gz", "wt", newline="") as f:
    w = csv.DictWriter(f, fieldnames=cols); w.writeheader()
    for r in rows: w.writerow({k: ("1" if v is True else "0" if v is False else v) for k, v in r.items()})
print(len(rows), "rows", len(cols), "cols", file=sys.stderr)
