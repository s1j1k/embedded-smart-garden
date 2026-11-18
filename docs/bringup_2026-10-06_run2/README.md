# Plain-water bring-up, run 2, 6 Oct 2026 (firmware 1.9.1) — FAILED (overshoot)

Vegetative band (600-800 ppm, dead limits 550-850; pH 5.5-6.5). Fresh tap water
(9 ppm, pH 6.54), 50 % level (7.8 L), 24.7 °C, no plants. Boot 11:50 UTC
(boot_id 10); the first nutrient check ran at the end of the 10 min boot hold.
Times below are from the first nutrient dose (11:59:44 UTC).

| File | Contents |
|---|---|
| `export_2026-10-06.csv.gz` | Pi MQTT export since 09:00 UTC (both runs, all fields) |
| `readings.csv`, `control_events.csv` | This run only, `t_min` from the first dose |
| `bringup_run2.png` | TDS and pH with the nutrient checks marked |
| `plot_run2.py` | Regenerates the CSVs and plot |
| `sim_final_design.py` | The design that replaces 1.9.1, simulated against this run's mixing |

## What happened

| t (min) | Event | TDS (ppm) | pH |
|---|---|---|---|
| 0 | Nutrient dose 18.9 mL (cap at 31 ppm/mL, 6.5 L-scaled to the level) | 9 | 6.54 |
| 9.4 | Recheck after the 9 min wait: gain booked 14.4 ppm/mL | 290 | 5.93 |
| 12.4 | pH check, no dose; nutrient dose 10.0 mL | 332 | 5.87 |
| 21.6 | Recheck: gain 20.0 ppm/mL | 593 | 5.69 |
| 24.6 | Nutrient and pH checks, both no dose ("settled" by C1) | 641 | 5.67 |
| ~38 | TDS crosses the 850 dead limit | 850 | 5.59 |
| 45.6 | Nutrient check: above dead high, no dilution path | 868 | 5.60 |
| 49.4 | ESP32 switched off; TDS levelling at ~890 | 889 | 5.59 |

- **Overshoot (C3 fail):** 29.1 mL raised TDS by ~880 ppm, about 30 ppm/mL
  (237 ppm·L/mL), the same gain as run 1 (31 ppm/mL at 8.4 L, 259 ppm·L/mL).
- **Cause: slow mixing.** Run 1 mixed in under a minute at 28 °C; this run took
  a first-order time constant of ~9 min (fit, `G` 28.7 ppm/mL). A 9 min wait saw
  ~60 % of a dose, so dose 2 was planned from 332 ppm while dose 1 alone was
  heading for ~560, and the "settled" checks at 24.6 min ran before the rise
  finished. The low booked gains (14, 20) were the same under-read.
- **The per-dose cap was not enough:** it bounds each dose against the reading at
  the time, so a reading still rising lets later doses stack.
- pH needed no reagent: the nutrient pulled it from 6.54 to 5.57 over ~45 min,
  just inside the 5.5 limit.

## Changes for the next run (firmware 1.9.3)

1. **Mass-balance budget (the no-overshoot guarantee).** The total nutrient a
   session may deliver is (850 − 100 − starting TDS) / worst-case gain, using the
   volume from the level reading derated 10 % and the largest worst-case gain seen
   in the session. TDS cannot end above 750 ppm however slowly the tank mixes
   unless the true gain exceeds the worst case by 10 % (289 ppm·L/mL; highest
   seen 259). Here: about 20 mL in total.
2. **Cap aims at the band's lower edge (600 ppm),** at the worst-case gain until a
   dose has been measured, then at the learned gain floored at 0.6 × worst case.
3. **20 min settling waits** for TDS and pH (40 min cycle); a 20 min wait sees
   ~90 % of a dose at this run's mixing.
4. Plain water (< 50 ppm) resets the nutrient session as well as the pH-salt
   observer.

`sim_final_design.py` against measured gains and mixing: 230-260 ppm·L/mL with a
0.2-15 min mixing time constant ends at 598-675 ppm with no overshoot, settled
in 40 min (60 min at the slowest mixing). Trade-off: a gain below ~215 ppm·L/mL
(not seen) would stop short of 550 rather than overshoot.
