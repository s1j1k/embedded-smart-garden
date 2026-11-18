# Recovery test with plants (18 Oct 2026)

Firmware 1.9.24 on the Main PCB, basil and lettuce in the tank, pH probe
recalibrated in pH 4.01 and 9.18 buffers that day (78 mV/pH). One unattended
cycle in the order the controller runs it: water level, then nutrient, then pH.

It is a recovery test rather than a clean dilution test (requirement C2): the
tank was already at about 380 ppm, below its 550 ppm limit, after a morning of
nutrient doses through a partly blocked line (TDS 415 -> 384 ppm by 07:35 UTC),
and the top-up itself only diluted it to about 350 ppm. The 530 ppm at 20:04 is
the filter starting up after the controller restarted, not a real reading.

| Local time | Event |
|---|---|
| 20:07–20:13 | Six water top-up bursts; level rises from about 20 % to 40 %, fresh water dilutes TDS to about 350 ppm |
| 20:20 | Nutrient dose 6.55 mL (TDS 345 ppm) |
| 20:32 | Nutrient dose 5.39 mL (408 ppm) |
| 20:49 | Nutrient dose 3.97 mL (485 ppm) |
| 21:03 | Nutrient dose 2.98 mL (538 ppm) |
| 21:05 | TDS (3 min median) past the 550 ppm dose limit: 44 min after the first dose |
| 21:18 | Recheck 589 ppm: nutrient done. pH check: 6.19, inside 5.5–6.5, no dose. Settled by the C2 definition: 71 min after the top-up (limit 60 min) |
| ~21:20 on | TDS settles at about 610 ppm in the 600–800 target; pH 6.2–6.4 |

Each nutrient dose was smaller than the one before (6.6, 5.4, 4.0, 3.0 mL): the
proportional law sizes it from the remaining error. The nutrient brought pH down
from 6.81 into range by itself, so the pH loop had nothing to do.

The level trace shows the sensor's 5 % (5 mm) steps and its jumps of several
steps with no change in the water (see the report's limitations).

After 22:30 the level read 10 %, TDS 826 ppm and the pH probe dropped out: not
part of this cycle, and not plotted.

## Files

- `full_cycle.png`: level, TDS and pH with the top-up bursts and doses marked
  (`python plot_full_cycle.py`).
- `run_readings.csv`: the 10 s readings, 18 Oct 08:30–12:00 UTC.
- `run_events.json`: controller events in that window (level checks separately).
- `export_run.py`: the read-only export, run on the Pi as
  `python export_run.py 2026-10-18T08:30:00 2026-10-18T12:00:00`.
