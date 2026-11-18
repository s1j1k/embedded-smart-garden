# Grow log

## pH probe recalibration — 2026-10-10 (firmware 1.9.15)

First two-point calibration with buffer temperature compensation, done
over Serial on the PCB with the repaired (water-damaged) SEN0169 signal
board. DS18B20 in each buffer next to the pH probe; at least 1 min soak,
then captured once volts held within 2 mV.

| Buffer | Temp | Table pH | Board V | 25 °C line pH |
| --- | --- | --- | --- | --- |
| 4.00 | 22.5 °C | 4.000 | 1.6069 | 4.025 |
| 9.18 | 22.7 °C | 9.203 | 1.7273 | 9.186 |

Saved line: `pH = 42.8652 × V_board − 64.8542`. Span is only **0.120 V**
(23 mV/pH) vs 0.337 V (65 mV/pH) on the 16 Sep breadboard cal, and 9.18
took ~4 min to settle and was still creeping up afterwards: weak, sluggish
probe and/or the repaired board. Readings are ~3× noisier per mV.
Probe swap planned for 2026-10-11, then recalibrate.

## Iteration 2 — hardware refresh (JST-XH + pH divider)

| Item | Status |
| --- | --- |
| Interconnect | Plug-in breadboard → **soldered breadboard** with **JST-XH 2.5 mm** plugs |
| Pump harnesses | 3-way ×4 (signal / 5 V / GND) |
| Grow light | 3-way power from LED buck; 3-way to strip (+5 V / GND / DIN) |
| Board 5 V rail | 2-way from second buck |
| DS18B20 / pH | 3-way each |
| Grove water level + TDS | 4-way each (TDS has 1× NC) |
| Ambient | 7-way: GND, 3.3 V, 5 V, SDA, SCL, DHT, LDR |
| pH path | **1 kΩ / 2 kΩ** divider (tap after 1 kΩ); gain **1.5**, `ADC_11db`; cal reset |
| TDS | Grove v1.0 wizard (`ENTERTDS` / salt water + pen or 707 ppm) |

## Iteration 2 — 2026-08-31

Prototype focus: **grow light online**, **soldered breadboard** main board,
**ambient daughterboard** (JST-XH 7-way), pump/nutrient calibration, and getting
basil + lettuce to actually grow under DWC.

| Item | Status |
| --- | --- |
| Mechanical | Soldered breadboard; pumps in ABS lid; ambient board remote |
| WS2812B panel (GPIO 27, 120 LED serpentine) | Wired — separate 5 V / 3 A buck |
| DHT11 | GPIO **16** (RX2) — moved off strap pin GPIO 15 |
| pH-down pump | GPIO **17** (strap pin — revisit on PCB) |
| Ambient harness | JST-XH 7-way (DHT, LDR, CO2 I2C + rails) |
| Photoperiod | 16 h/day blurple (18 h firmware ceiling) |
| Basil (left) / lettuce (right) | Seeded 2026-08-14 — monitor veg response |
| Next | Tune `PHOTOPERIOD_START_HOUR_UTC`, confirm DLI vs stretch; PCB pin map |

## Cycle — 2026-08-14

| When (local) | Event |
| --- | --- |
| 2026-08-14 11:45 | Rockwool soaked (plain reservoir water) |
| (pending) | Seeds: **A (left) = basil**, **B (right) = lettuce** |

Next firmware step after seeding: `MODE SEEDLING` (pH + drawdown hold; no auto EC until `MODE VEGETATIVE`).
