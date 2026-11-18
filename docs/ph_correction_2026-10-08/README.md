# Unattended pH correction, 8 Oct 2026 (firmware 1.9.12)

No plants. pH drifted below the 5.5 dead limit after the 7 Oct step test.

| UTC | Event | pH | TDS (ppm) |
|---|---|---|---|
| 04:50:26 | pH check → pH-up 1.61 mL delivered (gain 0.325 pH/mL at the default 6.5 L: level sensor reading nothing) | 5.41 | 834 → ~915 |
| 05:00–06:50 | controller moved to the new main PCB: six reboots (boot 29 → 38), data gaps. Water-temperature sensor lost (new layout misses some of its pins), so TDS/pH are compensated at the 25 °C fallback from 05:01 | 5.47 → 4.92 | 918 → 878 (compensation change, not real) |
| 06:15:24 | first check after the reboots → pH-up 2.90 mL (gain 0.311) | 4.94 | 874 → 1062 |
| 06:54 onward | in range, every check no dose | 5.5–5.7 | 1060–1077 |

- pH response ~0.04 and ~0.22 pH/mL, against the tuned 0.31–0.33: the citrate buffer.
- **TDS rose ~80 and ~190 ppm (40–65 ppm/mL, κ ≈ 300–450 ppm·L/mL)**, which takes TDS past
  the 850 dead limit. Theory agrees: each mL of 3.62 M K₂CO₃ leaves 7.2 mmol K⁺ and turns as
  much buffer acid into ions, about 60 ppm per mL in 7 L. Small doses near pH 6 (0.57 mL) were
  within noise on 7 Oct.
- This explains the overnight TDS rise from ~834 to ~1077.
- Without the temperature sensor, TDS reads ~4.5 % low at 22.7 °C (1062 shown ≈ 1110 true);
  the pH effect is under 0.02. Part of the pH fall to 4.92 may be the board change (the pH
  input was not re-checked in buffer after the swap).

`readings.csv` is the Pi SQLite export from 02:00–12:30 UTC. `plot_ph_correction.py`
produces `ph_correction_2026-10-08.png` / `.pdf`.
