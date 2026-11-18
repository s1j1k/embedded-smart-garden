# Characterization figures, 25–27 September 2026

Vegetative closed-loop run and the following on-device log. Local time on every axis is AEST (UTC+10). On the two performance figures, the grey band is the no-dose deadband and the black line is the vegetative setpoint.

| Loop | Deadband | Setpoint |
| --- | --- | --- |
| pH | 5.4–5.9 | 5.65 |
| TDS | 750–1250 ppm | 1000 ppm |

`characterization_weekend_figures.pdf` is the same five figures, in the order below.

Firmware runs this same 3 min EMA on the burst pH and the burst TDS. Dosing uses those values. `ph_filtered` and `tds_ppm` stay the burst readings. `ph`, `ph_ema`, and `tds_ppm_ema` are the averages. A sample taken while a pump is drawing on the shared 5 V rail is left out, and the average holds.

Regenerate with:

```text
python scripts/plot_characterization_figures.py
```

## characterization_closed_loop.png

Closed-loop capture from 25 Sep 22:33 to 26 Sep 03:10 AEST (`docs/characterization_2026-09-25_211710/phase1/closed_loop_trace.csv`).

pH moves from about 5.85 down through the deadband toward 5.65. TDS stays near 1250 ppm, on the top edge of the deadband and above the 1000 ppm setpoint. The blank stretch before the last samples is a gap in the capture.

## characterization_weekend_log.png

On-device sensor log from 26 Sep 20:31 to 27 Sep 21:26 AEST (`docs/since_characterization_2026-09-25/sensor_readings_since_characterization.csv`).

pH holds near the deadband, then falls twice (to about 3.8, then about 3.4) before pH-up doses bring it back. TDS stays above the deadband, from about 1270 ppm to a peak near 1355 ppm. Blue markers are samples taken while the pH-up pump was on. Red markers are pH-down.

## characterization_ph_filters.png

Same 26–27 Sep window, from `docs/ph_last_24h/ph_last_24h.csv`.

- Top: pH-board voltage after the 40 mV trimmed mean.
- Middle: raw Nernst pH, the firmware burst filter (median of the 16-sample burst, then the mean of samples within 0.25 pH of that median), a causal 5-sample median, and a 3 min exponential moving average. Pump-on samples are still in the raw and burst traces. The average holds its last value on those samples.
- Bottom: burst filter minus the 3 min average. Dashed lines are ±0.15 pH.

## characterization_tds_filters.png

TDS over the same 26–27 Sep window.

- Top: Grove TDS ADC count after the intra-burst average.
- Middle: firmware ppm, a causal 5-sample median, and the same 3 min exponential moving average, held while a pump flag is set. The dashed line is the dead-zone maximum, 1250 ppm.
- Bottom: firmware ppm minus the 3 min average. The residual stays within about ±5 ppm.

## characterization_pump_voltage.png

pH-board voltage in a short window around each logged pump sample. The dashed line is the median of the samples just before the pump flag. Each title is the pump sample minus that median.

The 27 Sep 21:15 pH-up sample sits about 9 mV and 0.14 pH below that median, and the voltage rises afterward as the dose mixes. The other five pump samples stay within a few millivolts of the readings around them.
