# Plain-water bring-up, 6 Oct 2026 (firmware 1.9.0)

Vegetative band (600-800 ppm, dead limits 550-850; pH 5.5-6.5). Tank of plain
water, no plants, air stone running. Data: MQTT telemetry (10 s) and control
events exported from the Pi SQLite (`mqtt_messages`).

| File | Contents |
|---|---|
| `bringup_1006.csv.gz` | Full export, all 252 telemetry/event fields |
| `readings.csv` | Key columns from the 10 s readings, `t_min` = minutes after the nutrient dose |
| `control_events.csv` | Control events (checks, doses, rechecks, gains) |
| `bringup_2026-10-06.png` | TDS, pH, level/temperature and pH-up volume |
| `tds_step_settle.png` | TDS step response with the 5 % settling band |
| `plot_bringup.py` | Regenerates the CSVs and plots from the export |

## Findings

- One 32.7 mL nutrient dose (the old 32.5 mL hard max) took TDS from 22 to
  1042 ppm: **31 ppm/mL**, twice the 14 ppm/mL the cap assumed, so the dose
  overshot the 850 ppm dead limit. The cap now uses 31 ppm/mL and the hard
  max is 25 mL.
- TDS settles within 5 % of its final value at **8.0 min (raw)** and **8.7 min
  (control filter)**. The 6 min wait booked the gain early, so the nutrient
  settle wait (and the top-up TDS wait) is now 9 min, giving a 21 min cycle.
- The nutrient pulled pH from 6.6 to below 5.5 at about 7.5 min and kept
  falling slowly to 3.9 over 30 min. 1.6 mL of pH-up barely moved it at
  ~1000 ppm (buffered), so the pH-up gain here is far below the label value.
- The salt-corrected "nutrient-only" TDS sat about 250 ppm below the reading
  because the pH-reagent accumulators (7.45 mL down, 2.3 mL up) were carried
  over from the previous solution: the refill did not go through `MODE FILL`
  / `CONFIRM`, which resets them. That corrupted the gain update (booked from
  730 instead of 982 ppm). Clear with `RESETPHSALT` after a manual refill.
- Above the upper dead limit the nutrient loop does nothing
  (`ec_high_no_dilute`): there is no automatic dilution, by design.
