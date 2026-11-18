# Plain-water bring-up with plants, 14 Oct 2026 (firmware 1.9.21)

Final bring-up test: from fresh tap water to the vegetative band, with the basil
(pot 1, three seedlings) and lettuce (pot 2) in the tank.

## Record

| When (AEDT, UTC+11) | UTC | Event |
|---|---|---|
| 13 Oct, ~22:50–01:30 | 13 Oct 11:50–14:30 | Old solution drained: carbonate-buffered (≈5 mL K₂CO₃ pH-up, TDS ~1000 ppm); pH rebounded to 8.2–8.3 within hours of every pH-down dose |
| 14 Oct 01:31 | 13 Oct 14:31 | **Plants back in the tank** after a brief rinse in lower-pH water. Fresh tap water: TDS 41 ppm, water 19.7 °C, pH probe still settling after the move (first reading 8.84) |
| 14 Oct 01:41 | 13 Oct 14:41 | Firmware 1.9.21 flashed (boot 86); controller still paused (saved pause) |
| 14 Oct 01:41:40 | 13 Oct 14:41:40 | pH session reset: pH-down no-response fault cleared after the operator primed the pump |
| 14 Oct 01:41:42 | 13 Oct 14:41:42 | Water change recorded for the solution monitor |
| **14 Oct 01:41:44** | **13 Oct 14:41:44** | **Auto control resumed: t = 0** (clean start, first check due after the 10 min hold) |
| 14 Oct 01:50 | 13 Oct 14:50 | Baseline at the end of the hold: **pH 9.9 (strip-confirmed)**, TDS 66 ppm, water 19.9 °C. pH had risen steadily from 8.6 (fresh tap water) and TDS from 42 ppm since the refill with no pump running: an alkaline input, probably pH-up dripping or siphoning from its line after the PRIME. Run treated as a bring-up from this baseline |

Starting conditions: stage VEGETATIVE (pH dead limits 5.5–6.5, TDS 550–850 ppm),
water-level stick needing initialisation (level unknown, so the controller used its
default 6.5 L volume; top-ups blocked), auto control resumed with the 1.9.21 clean
start (10 min sensor warm-up hold, then level → nutrient → pH on fresh readings).

## Results

**Status (14 Oct 07:50 AEDT): pH reached and held its band; TDS did not, because the
nutrient pump stopped delivering after the first dose. The run continues once the
nutrient line is fixed and the nutrient session cap is reset.**

| t (min) | Time (AEDT) | Event | TDS (ppm) | pH |
|---|---|---|---|---|
| 0 | 01:41:44 | Auto resume (clean start, 10 min hold) | 54 | 9.62 |
| 10.0 | 01:51:44 | First check: nutrient 12.7 mL | 66 | 9.94 |
| 30.3 | 02:12:00 | Recheck +69 ppm (5.4 ppm/mL; ~24 expected) → nutrient 11.5 mL | 135 | 8.63 |
| 50–112 | 02:32–03:53 | Six more nutrient doses (~11.4 mL each); TDS +0.0–0.3 ppm/mL each: **nothing delivered** | 138–141 | 8.54–8.40 |
| 132.1 | 04:53:51 | Nutrient session cap (100 mL logged, ~13 mL delivered) → pH check: pH-down 1.40 mL | 142 | 8.41 |
| 132–172 | 04:54–05:33 | pH dips to 6.3, then rebounds +0.6 in 30 min (carbonate: CO₂ stripped by the air stone) | 126 | 6.99 |
| 172.2 | 05:33:54 | pH-down 0.67 mL: dips to 6.5, rebounds to 6.76 | 127 | 6.99 |
| 207.2 | 06:08:55 | pH-down 0.54 mL | 127 | 6.76 |
| 268 | 06:09:43 | **pH held inside its dead limits** (5.71, then 5.72–5.81 for 1.7 h, no further dose) | 132 | 5.71 |
| 268–628 | 06:10–12:10 | No dose; pH drifts up 0.07/h (5.8 → 6.2), normal uptake; nutrient line being checked | 133–135 | 5.80–6.20 |
| ~628 | ~12:10 | Operator re-primed the nutrient line (9.3 mL logged; TDS 135 → 144) | 144 | 6.15 |
| 633.6 | 12:15:23 | Nutrient session cap reset: nutrient dosing allowed again | 144 | 6.15 |

- **Clean start (1.9.21) worked:** the first decision was taken on fresh readings exactly at
  the end of the 10 min hold, and it dosed nutrient first.
- **Nutrient pump not delivering:** after the first dose, eight doses (87 mL logged) moved
  TDS by 7 ppm in total. The pump is credited by run time, so the bottle counter is ~87 mL
  low. The nutrient loop has no no-response check (the pH loop does since 1.9.19), so it
  dosed for two hours until the 100 mL session cap stopped it.
- **pH:** from 9.94 (strip-confirmed baseline) to the band with 2.61 mL of pH-down in three
  doses. The first two each dipped below 6.5 and rebounded: the tank carried carbonate
  alkalinity, consistent with the alkaline input seen before the run. Measured response
  1.0, 0.35 and 1.9 pH/mL (tuned gain 1.5).
- **pH confirmed by hand:** strip, then a dropper pH test, ~12:15 (pH ~6.1–6.2 by the probe): the probe reading is accurate.

Doses: `actuator_events.csv` (from the controller's events, with readings before and at
each recheck); timeline: `milestones.csv`.

## Files

| File | Contents |
|---|---|
| `telemetry_full.csv.gz` | Every MQTT telemetry message from the start of the run, flattened (Pi SQLite `mqtt_messages`), one row per reading |
| `sensor_readings.csv` | The analysis columns of the same rows (time, pH, TDS, temperatures, pumps, sessions, solution monitor) |
| `control_events.csv` | Every control event (checks, doses, rechecks, faults, resume) |
| `actuator_events.csv` | One row per dose, from the controller's events: pump, requested and delivered mL, the reading it was planned from and the settled reading at its recheck |
| `commands.csv` | Commands sent to the controller and its acknowledgements |
| `milestones.csv` | Plants in, auto resume, first dose; pH and TDS each first inside (10 min) and held inside (60 min) their dead limits; settled |

Rebuilt with `python scripts/export_bringup_run.py` (see its docstring).
