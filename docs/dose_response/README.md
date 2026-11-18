# Process gains from every logged dose (30 Sep – 14 Oct 2026)

Where the firmware's dose-sizing gains come from (1.9.22).

| Loop | Valid doses | Range (per litre) | Gain used | From |
|---|---|---|---|---|
| pH-up | 14 | −2.9 to 14.8 pH L/mL | **14.8** | 1 Oct 10:07: 0.538 mL, +1.22 pH at 170 ppm (30 Sep run) |
| pH-down | 4 | 2.3 to 12.1 pH L/mL | **12.1** | 14 Oct 06:08: 0.544 mL, pH 6.73 → 5.72 |
| Nutrient | 12 | 141 to 262 ppm L/mL | **262** | 6 Oct 20:26 run 1: 32.7 mL, 22 → 1041 ppm at 8.4 L |

Each defining dose is measured from the raw readings as the mean over the settled
window minus the mean over the 5 min before, × V / u (top row of the figure).
The largest valid response is used: with u = λ|e|/K on an integrating tank the
next error is e(1 − λK_true/K), so no dose overshoots while K_true ≤ K/λ.

Excluded (crosses in the figure): the pH-down pump fault (11–13 Oct), the
nutrient pump fault (14 Oct 01:51–04:33), the pH probe front-end fault
(9–11 Oct), a 4.0 L level misread (6 Oct 22:59), and three unsettled 12-min pH-up
rechecks on 6 Oct (counted as one step). 10 Oct 22:08 uses the settled TDS (634
ppm); its recheck caught a 786 ppm spike from the failing probe board.

Until 1.9.22 pH-up used 2.52 pH L/mL × 700/TDS. At ~640 ppm the data has both
≈0 (7 Oct, citrate-buffered) and 8.1 (14 Oct, fresh): TDS does not predict the
pH gain.

| File | Contents |
|---|---|
| `all_doses_2026-10-06_to_14.csv` | Every settled dose in the Pi event log, check → recheck |
| `gains_curated.csv` | All measurements with status (used / excluded) and source |
| `gain_identification.png` | The figure (also in the report, Appendix) |
| `build_gains.py` | Rebuilds the two above: `python docs/dose_response/build_gains.py` |
