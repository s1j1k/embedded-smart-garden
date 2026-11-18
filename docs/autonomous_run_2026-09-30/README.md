# Autonomous vegetative run, 30 Sep – 5 Oct 2026

The controller ran unattended in vegetative mode from the afternoon of Wed 30 Sep, and the data stops at 5 Oct 03:07 UTC. Local times are Australia/Melbourne: AEST (UTC+10) until 4 Oct 02:00, then AEDT (UTC+11). The figures use hours since the plain-water refill, so the DST change doesn't shift the axis.

Settings during the run: TDS target 600–800 ppm with a 550–850 ppm deadband, and pH setpoint 6.0 with a 5.9–6.1 deadband. Checks run every 2 h, with a 45 min EC settle and a 15 min pH settle. The controller is `adaptive_P` and acts on the 3 min EMA of each sensor. The device reports firmware **1.6.0**, so the 1.6.2 pump-overrun fix and the 1.7.0 changes were not flashed for this run.

Regenerate with:

```text
python scripts/build_autonomous_run.py
python scripts/plot_autonomous_run.py
```

## Headline numbers

| Milestone | UTC | Local | h after plain water |
| --- | --- | --- | --- |
| Reservoir refilled with plain water (TDS < 30 ppm) | 30 Sep 12:37 | 30 Sep 22:37 | 0 |
| First autonomous nutrient dose | 30 Sep 13:05 | 30 Sep 23:05 | 0.5 |
| TDS EMA ≥ 100 ppm | 30 Sep 17:27 | 1 Oct 03:27 | 4.8 |
| TDS EMA ≥ 300 ppm | 1 Oct 04:09 | 1 Oct 14:09 | 15.5 |
| **TDS EMA ≥ 550 ppm (enters deadband)** | **2 Oct 22:07** | **3 Oct 08:07** | **57.5** |
| TDS EMA ≥ 600 ppm (target) | 2 Oct 22:07 | 3 Oct 08:07 | 57.5 |

pH was already inside 5.9–6.1 when TDS crossed 550 ppm. After that point, the TDS EMA stayed in 550–850 ppm for 100 % of the time. The pH EMA was in 5.9–6.1 for 52 % of the time and in 5.5–6.5 for 85 %. `milestones.csv` holds these numbers.

## What the run shows

- **Three nutrient overruns supplied most of the TDS rise.** A normal 0.5 mL command books about 0.7–0.9 mL against stock, because doses run in whole 0.405 mL bursts. Each dose raised TDS by 5–25 ppm, typically 8–10 ppm. Three doses instead booked about 7.6 mL each:

  | Overrun (UTC) | Commanded | TDS before → after |
  | --- | --- | --- |
  | 30 Sep 17:22 | 0.35 mL | 17 → 117 ppm |
  | 1 Oct 04:05 | 0.3 mL | 222 → 353 ppm |
  | 2 Oct 22:06 | 0.5 mL | 545 → 740 ppm |

  Together they added about 430 of the roughly 765 ppm. The third one carried TDS across the deadband edge. At the normal rate of about 9 ppm per 2 h cycle, the remaining 5 ppm to 550 would have taken one more cycle. Without any of the overruns, starting from 14 ppm would have taken roughly 115–120 h instead of 57.5 h. That's an estimate from the per-cycle rate; it isn't a measured result. The overruns match the bug fixed in 1.6.2.
- **The proportional gain is capped by `ec_max_dose_ml` = 0.5 mL.** With the gap above 300 ppm, every cycle asks for the 0.5 mL cap. The slope in the plain-water phase is set by that cap and the 2 h cycle, not by the gain.
- **A power-on reset on 1 Oct 02:05 UTC reset the adapted gains** to their defaults (EC 0.0005, pH 0.12). The next dose was 0.3 mL, and it was the second overrun.
- **pH drifts up between doses while nutrients are being added.** The controller corrects with 0.05–0.15 mL pH-down doses every 2 h, without an overshoot below 5.5. There was one dip to about 5.0 on 3 Oct 08:00 UTC, about 66 h in; a pH-up dose followed, and pH recovered within 3 h.
- **pH dosing has been locked out since 4 Oct 02:52 UTC (86 h).** Every `ph_check` since then carries `ph_session_cap` (2 mL cap). pH has drifted to 6.4–6.8 without correction. Resetting the session or raising the cap is needed before the run continues.
- **Influx uploads were failing.** The device log shows `connection refused ssl` and a full 8-line queue, so Influx holds only about 950 of the roughly 40 000 readings. The Pi's MQTT store is the complete record.

## Mixing delay after a dose

Measured from 10 s telemetry around each pump start (`dose_response.csv`, `autonomous_run_dose_response.png`).

| | Raw reading settled | 3 min EMA (controller input) 95 % | Firmware wait |
| --- | --- | --- | --- |
| TDS, 26 doses + 3 overruns | about 1.5-2 min. The first sample (10-20 s) overshoots to about 1.5x, dips at 1 min, then holds | about 20 min (63 % at 4 min, 90 % at 11.5 min) | `ec_settle_s` 2700 s (45 min) |
| pH, one clean step (1 Oct 00:07 UTC pH-up, +1.2 at TDS 170 ppm) | about 9 min (63 % at 1.8 min, 90 % at 6.3 min) | about 14 min (90 % at 11.8 min) | `ph_settle_s` 900 s (15 min) |

The 18 pH-down doses (0.05-0.15 mL) change pH by less than the drift of 0.05-0.25 pH/h, so their own response cannot be measured; the pH figure rests on the single pH-up step. A 20 min EC wait would be enough. The 15 min pH wait leaves little margin; 18-20 min is safer until more clean pH steps are measured.

## Files

| File | Rows | What it is |
| --- | --- | --- |
| `sensor_readings.csv` | 40 341 | Every reading from 30 Sep 03:30 to 5 Oct 03:07 UTC, every 10 s. It holds the columns needed for analysis and simulation: pH (voltage, raw, burst filter, EMA), TDS (reading, EMA, nutrient and pH-salt split), pump flags, remaining stock, gains, schedule state, temperatures and level. |
| `telemetry_full.csv.gz` | 40 341 | The same rows with all 211 telemetry fields, including config, calibration, health and schedule state. This is lossless. |
| `control_events.csv` | 226 | The controller's own decision log (LittleFS `/control_events.csv`) since 30 Sep: `level_check`, `ec_check`, `ec_recheck`, `ph_check`, `ph_recheck`, `operator_dose`, with values before and after, dose, gains and alert flags. |
| `actuator_events.csv` | 53 | One row per pump start, from the telemetry pump flags. Each row has the commanded volume from the control log, the volume the controller booked against stock (`*_rem_ml` before minus after), the controller input before and at the recheck, and an `overrun` flag (booked > 3 mL). |
| `milestones.csv` | 11 | The table above. |
| `autonomous_run_overview.png` | | TDS, pH and control-event lanes from plain water to now. This is the report figure for time to the vegetative band. |
| `autonomous_run_tds_filter.png` | | TDS reading against the 3 min EMA: the whole run, a zoom on the 2 Oct overrun step, a quiet hour, and the residual. |
| `autonomous_run_ph_filter.png` | | pH raw, burst filter and 3 min EMA: the same layout. |
| `autonomous_run_dose_response.png` | | Sensor response after each dose, with the firmware settle waits marked: TDS for normal doses and for the overruns, the one clean pH step, and the pH-down average. |
| `dose_response.csv` | 50 | Per dose: settled change, peak, time to 90 %, settle time, pre-dose drift, and EMA settle time. |
| `autonomous_run_figures.pdf` | | All four figures. |
| `raw/` | | Unprocessed inputs, listed below. |

`raw/` contents:

- `pi_mqtt_telemetry_dedup.csv.gz`: every telemetry payload in the Pi dashboard's SQLite (`mqtt_messages`) since 30 Sep, flattened and de-duplicated on (timestamp, uptime_ms, boot_count). The Pi stored 437 158 messages, mostly repeats from the device's CSV replay. The export script is `scripts/pi_export_mqtt_telemetry.py`.
- `control_events_device.csv`, `serial_dump_ctrl.txt`: `DUMP CTRL` over serial on 5 Oct, the full log back to 22 Sep.
- `serial_dump_sensors.txt`: `DUMP SENSORS`. It holds only a few rows. In 1.6.0 `/sensor_readings.csv` was also the MQTT replay buffer: every row was replayed to the Pi, which is why each reading arrived twice, and then removed, so the device kept no history. 1.6.2 split the backup log from the replay buffer.
- `influx_sensor_reading.csv`, `influx_control_event.csv`: everything in Influx for the window, as a cross-check. It's sparse. It has `ph_session_ml` and `ec_session_ml`, which MQTT does not.

## Notes for using the data

- `row_source` is `live` for normal telemetry. It's `replay` for the 224 readings that reached the Pi only through the device's CSV replay, which filled short gaps; those rows have fewer columns. The figures use live rows only.
- Coverage is continuous at 10 s from 30 Sep 13:00 UTC onward. From 4 Oct 22:05 to 5 Oct 00:45 UTC, live rows arrive every 2–5 min, because MQTT dropouts left some readings unpublished.
- Before 12:37 UTC on 30 Sep the data is bench work: about 20 power-on reboots while flashing, operator serial doses (TDS reached about 1100 ppm and pH about 9.6), and the pH probe reading about −22 while it was unplugged. `hours_since_plain_water` is negative there.
- `*_pump_on` is sampled every 10 s. A dose is a few seconds long, so it shows up as one flagged row. Use `actuator_events.csv` for volumes.
- `controller_accounted_ml` is the controller's own run-time estimate, not a measurement. pH doses book about 0.5 mL against 0.05–0.15 mL commanded, because they run in whole bursts.
- `tds_ppm_nutrient` subtracts an estimated pH-salt contribution (`tds_ph_salt_ppm`, about 135 ppm by the end) from the TDS reading. The EC controller doses on `tds_ppm_ema`, not on the nutrient-only value.
