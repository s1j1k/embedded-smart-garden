# Final bring-up from plain water, with plants (16 Oct 2026)

Firmware 1.9.23 (commit 717f73e): a cycle starts when the filtered TDS or pH
leaves its dead limits; the nutrient loop runs until TDS is inside its dead
limits, then the pH loop runs. Settle waits end when the reading stops moving,
at the latest 20 min after a dose.

## Results

| | |
|---|---|
| Start | 7 L plain water, TDS 17 ppm, pH 7.6, plants in |
| Nutrient | 2 doses, 14.80 + 4.56 mL |
| TDS in the 600-800 ppm target band | **12 min** after the first dose (settled at ~780 ppm, no overshoot past the band) |
| pH | nutrient acid took it 7.6 -> 3.1 (17 ppm water has almost no alkalinity); once TDS was in range, **one pH-up dose (1.25 mL)** brought it inside 5.5-6.5 |
| pH in the dead limits | **23 min** after the first nutrient dose |
| pH loop correcting a high pH | one pH-down dose (0.57 mL) took pH 6.92 -> 6.53 in 5 min |

## Figures (made by `plot_final.py` from the CSVs below)

- `bringup_final.png`: TDS (raw and control value) and pH from plain water until
  both are in range (to 26 min), with each dose marked. For the poster / report.
- `ph_recovery.png`: the pH loop correcting a high pH: one pH-down dose taking pH
  from 6.9 to the 6.5 limit. (pH had crept up to ~6.9 later in the run.)

Re-make them with `python plot_final.py` in this folder.

## Data files

From `scripts/export_bringup_run.py` (Pi MQTT store, from 16:49 UTC):

- `sensor_readings.csv`: every 10 s reading (pH, TDS raw and filtered, level, pumps, sessions).
- `control_events.csv`: every controller event (checks, doses, rechecks, resets).
- `actuator_events.csv`: one row per dose: pump, delivered mL, reading before and after the
  recheck, change per mL.
- `milestones.csv`, `commands.csv`, `telemetry_full.csv.gz`, `raw/mqtt_messages.jsonl.gz`.
- `serial.log`: every serial line from the ESP32 with the UTC time it arrived
  (gap 18:12-22:40 UTC, when the logger was not running; the Pi has that stretch).
  `inbox.txt`: commands sent to the board (also in the log as `>>> CMD`).

Re-export: `python scripts/export_bringup_run.py --run-dir docs/bringup_2026-10-16_final
--since 2026-10-15T16:49:00Z --plants-in 2026-10-15T16:06:00Z`

## Timeline (UTC; local time is UTC+11)

| UTC | Local | Event |
|---|---|---|
| 2026-10-15 16:06 | 03:06 | Plants in |
| 2026-10-15 16:49:30 | 03:49 | `LIGHT ON 8.7`: grow light on for the 8.7 h missed in the last light period |
| 2026-10-15 16:49:49 | 03:49 | `AUTO RESUME`: **bring-up start** (first check 10 min later) |
| 2026-10-15 16:59:49 | 03:59 | TDS 17 ppm: nutrient 14.80 mL. pH 7.6 -> 3.8 within a minute |
| 2026-10-15 17:10:08 | 04:10 | TDS settled at 496 ppm (10 min): nutrient 4.56 mL |
| 2026-10-15 17:18:14 | 04:18 | TDS settled at 732 ppm (8 min): nutrient loop done |
| 2026-10-15 17:21:14 | 04:21 | pH settled at 3.12: pH-up 1.25 mL. pH 6.2 within 2 min |
| 2026-10-15 17:41-17:57 | 04:41-04:57 | pH crept to 6.6; three short pH-down doses (375-490 ms runs) did not move it |
| 2026-10-15 22:39:53 | 09:39 | pH 6.92: pH-down 0.57 mL -> 6.53 in 5 min |
| 2026-10-15 22:48-23:00 | 09:48-10:00 | three more short pH-down doses (375-386 ms); pH 6.47-6.52 |
| 2026-10-16 02:18 | 13:18 | Firmware with the no-response check as a warning only flashed; panel ON |

Before 16:49 (not part of the run): the board was flashed twice (manual grow-light
on, then this), the Pi was restarted 16:15-16:21, and the panel was off 16:21-16:49.

## Notes for follow-up

- **Short pump runs may deliver little.** Every pH-down dose that did not move pH ran
  the pump for 375-490 ms; the ones that worked ran longer. "Delivered" mL in the
  logs is calculated from run time, not measured. A minimum pump run time is worth
  testing.
- LittleFS on the ESP32 is full (`/control_events.csv` has no size cap), so the
  on-board journal is not written; the Pi and `serial.log` hold the record.
- Ambient (DHT11) and CO2 sensors not responding (ambient -127 C, CO2 I2C error).
