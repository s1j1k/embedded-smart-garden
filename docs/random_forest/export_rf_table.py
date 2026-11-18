"""Run on the Pi from the dashboard repo: dump the harvest random forest's training table."""
import sys
from datetime import datetime
from pathlib import Path
ROOT = Path.cwd()
sys.path.insert(0, str(ROOT / "src"))
import pandas as pd
import development, garden_settings, harvest, plant_growth, sqlite_store

settings = garden_settings.load()
planted = development._naive_local(garden_settings.planted_at().isoformat())
pots = [str(p).lower() for p in settings.get("pots")]
breaks = development.parse_breaks([
    {"start": "2026-09-03T17:00", "end": "2026-09-25T12:00", "reason": "windowsill"},
    {"start": "2026-10-06T00:00", "end": "2026-10-08T12:00", "reason": "plain-water bring-ups"},
])
print("planted", planted, "pots", pots)
plants = plant_growth.load_plants()
rows = sqlite_store.load_hourly_conditions("data/sensor_readings.sqlite", planted.astimezone().isoformat())
hourly = development.hourly_conditions(rows)
conds = {c: development.daily_development(hourly, c, planted, datetime.now(), breaks,
                                          settings.get("grow_light_ppfd")) for c in set(pots)}
sizes = harvest.daily_sizes(plants)
sizes.to_csv("/tmp/rf_sizes.csv", index=False)
for c, d in conds.items():
    d.to_csv(f"/tmp/rf_daily_{c}.csv")
table = harvest.training_table(sizes, conds, pots)
table.to_csv("/tmp/rf_table.csv", index=False)
print(table.round(3).to_string())
for name, t in (("all", table), ("clean", table[table["day"] <= "2026-10-15"])):
    if len(t) >= harvest.MIN_DAYS:
        from sklearn.ensemble import RandomForestRegressor
        x = t[harvest.FEATURES].fillna(t[harvest.FEATURES].median())
        m = RandomForestRegressor(n_estimators=200, min_samples_leaf=2, random_state=0, n_jobs=1).fit(x, t["rgr"])
        print(name, len(t), {k: round(v, 3) for k, v in sorted(zip(harvest.FEATURES, m.feature_importances_), key=lambda i: -i[1])})
