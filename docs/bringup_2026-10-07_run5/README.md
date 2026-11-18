# Plain-water bring-up, run 5, 7 Oct 2026 (firmware 1.9.9) — PASSED

Vegetative band (600-800 ppm, dead limits 550-850; pH 5.5-6.5). Fresh tap water
(44 ppm, pH 7.46), 45-50 % level (~7.3 L), 21.9 °C, no plants. Air stone in the
middle of the tank; TDS probe moved away from the pump inlets. Boot 17:02 local
(boot_id 27, after a panic reset at ~17:02 — see below).

Firmware 1.9.9: fixed tuned gains (nutrient 260 ppm·L/mL ÷ V, lambda 0.8, cap to
700), nutrient phase finished before pH, 20 min waits with settle detection.

| t (min) | Time | Event | TDS (ppm) | pH |
|---|---|---|---|---|
| 0 | 17:12:48 | nutrient 12.2 mL (12.4 delivered) | 44 | 7.46 |
| 20.3 | 17:33:03 | recheck below 550 → nutrient 7.7 mL (8.0 delivered), no pH check | 340 | 6.47 |
| 40.4 | 17:53:13 | recheck 597: nutrient phase done | 597 | 6.01 |
| 40.4 | 17:53:14 | nutrient check no dose; pH check no dose | 597 | 6.01 |

- **C1 met: settled 40 min after the first dose** (limit 60 min; first run 57.5 h).
- **C3 met: no overshoot** (TDS peak ~600 ppm, limit 850); no pH reagent needed —
  the nutrient brought pH from 7.46 to 6.01.
- Response ~24 ppm/mL (~175 ppm·L/mL) with the probe away from the inlets, below
  the 31 ppm/mL measured near them in runs 1-3: the earlier readings were biased
  high by the unmixed zone at the inlets, so the tuned gain (260) is conservative
  and TDS lands at the bottom of the band.
- The ESP32 reset with `last_reset=panic` ~2 min after the first boot (also once
  this morning); the run started after that reset. Cause not yet found.

Plots: `../poster/closed_loop_1.9.9.png`, `../poster/dosing_1.9.9.png`
(`../poster/poster_plots.py export_run5.csv.gz 27 "2026-10-07T06:12:48Z" 1.9.9 ...`).
