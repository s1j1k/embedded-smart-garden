# TDS / pH reagent coupling evidence

## Nutrient dose sizing (firmware)

Nutrient control does **not** use a fixed mL per dose. It uses the same adaptive
dead-band proportional law as pH:

`dose_mL = clamp(gain_ec × |TDS − band_mid|, ecMinDoseMl, ecMaxDoseMl)`

then quantises to calibrated 500 ms bursts. Vegetative clamps are **0.2–2.0 mL**
(harvest 0.15–1.5 mL); initial gain **0.004 mL/ppm**. The TDS nutrient loop has
not been field-tested yet (stage archive is `sensor_baseline` only).

## Influx pull (2026-08-01 → 2026-09-20)

Re-queried Influx Cloud bucket:

| Query | Result |
| --- | --- |
| `ph_session_ml > 0` | empty |
| `ph_up_pump_on` / `ph_down_pump_on` > 0 | empty |
| `control_event` measurement | empty |
| Distinct `stage` tag | `sensor_baseline` only |

Preliminary dosing from ~one month earlier is **not** in this bucket. Best
field series for co-plotted pH/TDS is `window_2026-09-16_calibration.csv`
(probe calibration / tank immersion — **not** an auto pH dose).

## Firmware observer (now implemented)

Matches the report equation:

`TDS_nutrient = TDS_sensor − (κ_up·∫F_up + κ_down·∫F_down) / V(t)`

- Header: `shared/tds_nutrient_observer.h`
- Default κ (provisional): up **250**, down **200** ppm·L/mL — replace via
  `SETKAPPA UP|DOWN <v>` after a bench measurement
- Live EC loop uses `TDS_nutrient` when `TDSCORR ON` (default)
- Accumulators reset on `CONFIRM` fill_0 (full refill)
- Telemetry: `tds_ppm` (raw), `tds_ppm_nutrient`, `tds_ph_salt_ppm`, κ, cum mL

## MATLAB figure

```matlab
cd docs/tds_ph_coupling
plot_tds_ph_coupling
```

Panel 1 = field window (annotated: no auto-dose). Panel 2 = model of TDS rising
after a pH-Down pulse and the observer recovering nutrient TDS.

A pre-rendered `tds_ph_coupling_figure.png` (and `.svg`) is included so the
figure is available without MATLAB; re-run the `.m` script to regenerate from
the CSVs.
