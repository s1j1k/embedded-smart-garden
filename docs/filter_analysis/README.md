# Sensor filter analysis

Why the pH and TDS control input moved from a 3 min EMA to a 2nd-order
Butterworth low-pass at 2 mHz (firmware 1.9, `shared/sensor_ema.h`).

Data: `docs/autonomous_run_2026-09-30/sensor_readings.csv` (10 s telemetry).

```text
python docs/filter_analysis/filter_analysis.py     # figures + filter_summary.csv
```

`filter_analysis.m` is the MATLAB version (Signal Processing Toolbox) for
cross-checking; it prints the same noise and settling numbers.

| Figure | Shows |
| --- | --- |
| `filter_psd.png` | Welch PSD of pH and TDS (20.5 h quiet window) with the signal, transition and noise-floor bands, the old EMA and the chosen Butterworth, and both filters' gain |
| `filter_tradeoff.png` | Lag (settle to 5 %) against output noise for EMA and Butterworth-2/4 of varying cutoff |
| `filter_dose_steps.png` | The filters replayed causally on a real pH-up step and a nutrient step |
| `filter_burst_response.png` | 16-sample ADC burst average: zeros at 50, 100, 150 Hz |
| `filter_other_sensors.png` | DS18B20, water level, DHT11, CO2 and LDR with their existing filtering |

Headline (from `filter_summary.csv`):

| Filter | Step settle 5 % | pH noise | TDS noise | pH dose step* | TDS dose step* |
| --- | --- | --- | --- | --- | --- |
| burst reading | 0 | 0.0143 | 0.51 ppm | 8.1 min | 3.0 min |
| EMA 3 min | 8.8 min | 0.0083 | 0.20 ppm | 13.1 min | 10.0 min |
| Butterworth-2 2 mHz | 3.8 min | 0.0098 | 0.26 ppm | 10.1 min | 5.0 min |

\* time to stay within 0.1 pH / 10 ppm of the final value after a real dose.

The firmware filter is checked against `scipy.signal.butter(2, 0.002, fs=0.1)`
in `tests/host/control_logic_test.cpp`.

## Settling time of every real dose (`settle_times.py`)

Each dose from 30 Sep - 5 Oct replayed through the three control inputs
(pump-on samples held, pre-dose drift removed). Settled = stays within 10 %
of the step or the noise floor (2 ppm, 0.03 pH). Output: `settle_times.csv`,
`settle_times.png`.

| Doses | Burst reading | EMA 3 min | Butterworth-2 2 mHz |
| --- | --- | --- | --- |
| 25 normal nutrient | median 1.5, p90 14.7 min | median 4.5, p90 7.2 min | median 3.3, p90 4.3 min |
| 3 large nutrient steps | 1.7 min | 7.2-7.5 min | 3.8-4.4 min |
| 1 clean pH-up step (+1.2 pH) | 7.2 min | 11.9 min | 9.0 min |

The burst reading's long tail is noise crossing the band on small steps; the
Butterworth removes it. 17 of 18 pH-down doses (about 0.5 mL booked each)
moved pH by less than 0.1, about 15x less than the label predicts: check the
pH-down line delivers before relying on it.
