# Plain-water bring-up, run 3, 7 Oct 2026 (firmware 1.9.4) — stopped below the band

Vegetative band (600-800 ppm, dead limits 550-850; pH 5.5-6.5). Fresh tap water
(16 ppm, pH 6.07), 25 °C, no plants. Boot 10:54 local (boot_id 13); the first
nutrient check ran at the end of the 10 min boot hold. Times are from the first
nutrient dose (11:03:56 local).

| File | Contents |
|---|---|
| `export_run3.csv.gz` | Pi MQTT export from 23:40 UTC 6 Oct (all fields) |
| `readings.csv`, `control_events.csv` | This run, `t_min` from the first dose |
| `bringup_run3.png` | TDS and pH with the nutrient checks marked |
| `plot_run3.py` | Regenerates the CSVs and plot |

## What happened

| t (min) | Event | TDS (ppm) | pH | Level |
|---|---|---|---|---|
| 0 | Nutrient dose 13.6 mL (cap to 600 at the worst-case gain) | 16 | 6.07 | 40 % |
| 0-20 | Tank still being filled by hand, 40 % → 50 % | | | |
| 20.3 | Recheck (gain 18.4), pH check no dose; nutrient 3.3 mL = rest of the session budget | 331 | 5.71 | 50 % |
| 40.4 | Recheck; nutrient check: budget used up, no dose | 436 | 5.73 | 50 % |
| 51 | Flat | 443 | 5.70 | 50 % |
| 90 | Checks at 40.4 and 80.4 min: no dose; still flat (quiet baseline for identification) | 447 | 5.56 | 50 % |

- **No overshoot, but no settling (C1 not met):** TDS stopped at ~440 ppm, below the
  550 dead limit, with nothing more allowed.
- **Cause: the mass-balance session budget** (1.9.3/1.9.4) was sized at the first
  check from the level then (40 %, ~6.7 L, so a worst-case gain of 43 ppm/mL and a
  17 mL total) while the tank was still being filled to 50 % (7.8 L).
- **The nutrient was not weaker.** Dose 1 looks like 23 ppm/mL only because ~1 L
  of water was added during its mixing; dose 2 (3.3 mL, full tank) gave ~32
  ppm/mL, i.e. ~245 ppm·L/mL, matching runs 1 (259) and 2 (237).
- **Mixing:** dose 1 settled to within ~2 ppm/min by ~6 min and crept ~35 ppm over
  the next 14 min (partly confounded by the filling); dose 2 mixed in ~3 min.
- pH drifted 6.07 → 5.65 from the nutrient and back to 5.7; no pH dose needed.

## Change for the next run (firmware 1.9.5)

The session budget is removed; safety is per dose: the cap uses the worst-case
gain and aims at the band middle (700) when more than a band-width below it, the
top edge (800) when closer, and after the 20 min wait a dose waits until the
filtered TDS rises < 2 ppm/min over 5 min (5 min steps, up to +20 min, else no
dose that cycle). See `../bringup_2026-10-06_run2/sim_settle_detect.py`:
no overshoot over 150-289 ppm·L/mL and mixing time constants 0.2-30 min
(peak ≤ 777 ppm). Fill the tank fully before the boot hold ends.
