/************************************************************
 * Controller design constants (adaptive dead-band P)
 *
 * One place for every constant the report's controller design
 * defines. Each line names the report symbol it implements
 * (report Section "Embedded software and closed-loop control",
 * table "Report constants and their implementation").
 *
 * Per-stage values (bands, dead limits, gain seeds, minimum doses,
 * absolute dose ceilings, settle waits, check intervals) live in
 * grow_stage.h.
 ************************************************************/

#ifndef SMART_GARDEN_CONTROL_DESIGN_H
#define SMART_GARDEN_CONTROL_DESIGN_H

namespace ControlDesign {

// lambda: fraction of the error each dose aims to correct. The overshoot cap
// below, not a smaller first dose, protects against a wrong gain estimate.
constexpr float LAMBDA = 0.8f;     // nutrient
// pH aims at the whole error: the nutrient keeps pulling pH down for ~30 min
// after its dose, so 0.8 left pH just above the lower limit (6 Oct data).
// The worst-case cap still stops a dose short of the dead-range middle.
constexpr float LAMBDA_PH = 0.8f;  // 7 Oct: tuned gains, 20 % margin against overshoot

// Fixed, hand-tuned process gains (7 Oct). The online gain estimate gave wrong
// answers in a buffered, slowly mixing tank (part-mixed rechecks read low, the
// nutrient's acid release hid pH-up doses), so the gains are set from the step
// data and the recheck only logs the response.
constexpr bool ADAPTIVE_GAINS = false;
// pH gains for dose sizing (pH L / mL, divided by the volume): the largest
// response measured over every logged dose 6-14 Oct (docs/dose_response/,
// gains_curated.csv and gain_identification.png; doses during the pH-down pump
// fault, the nutrient pump fault and the pH probe fault excluded). A dose is
// lambda |e| / K and the tank integrates it, so the next error is
// e (1 - lambda K_true / K): no dose overshoots while K_true <= K / lambda, and a
// more buffered tank only lands short, and the next check doses again.
//   pH-up 14.8: 1 Oct 10:07, near plain water (170 ppm), 0.538 mL +1.22 pH (6.5 L);
//   next largest 8.1 on 14 Oct 13:14 (fresh solution at 636 ppm, 3.0 mL 4.36 -> 8.1).
//   pH-down 12.1: 14 Oct 06:08, 0.544 mL, pH 6.73 to 5.72, once the carbonate
//   was spent.
// Until 1.9.22 pH-up used 2.52 pH L/mL x (700 / TDS), from the 7 Oct tank, which
// was buffered by the citric pH-down of the buffer test: buffering depends on
// which weak acids are present and on the pH, not on the TDS (0.0 pH/mL on 7 Oct
// and 1.25 pH/mL on 14 Oct, both at ~640 ppm). On 14 Oct that gain read 0.40
// pH/mL and its 3 mL dose took pH from 4.36 to 8.1.
// Each: mean over the settled window minus the 5 min before the dose, x V / u.
constexpr float PH_UP_GAIN_PHL_PER_ML = 14.8f;
constexpr float PH_DOWN_GAIN_PHL_PER_ML = 12.1f;

// alpha: first-order filter coefficient of the online gain update.
constexpr float GAIN_ALPHA = 0.5f;

// K_EC bounds (ppm per mL of nutrient) for the online estimate.
constexpr float EC_GAIN_MIN = 3.0f;
constexpr float EC_GAIN_MAX = 60.0f;

// K_up / K_down bounds (pH per mL of reagent) for the online estimate.
constexpr float PH_GAIN_MIN = 0.2f;
constexpr float PH_GAIN_MAX = 6.0f;

// Smallest settled change used for a gain update (below this it is noise).
constexpr float EC_MIN_DELTA_PPM = 5.0f;
constexpr float PH_MIN_DELTA = 0.05f;

// No-response fault: a pH dose the tuned gain says should move pH by at least
// PH_NO_RESPONSE_MIN_EXPECTED, but that moves it by less than PH_MIN_DELTA the
// right way, counts as unanswered. PH_NO_RESPONSE_DOSES in a row in one
// direction stops dosing that way: the pump or its line is not delivering, or
// (much less likely) the tank is far more buffered than the tuned gain.
// 11-12 Oct: 31 pH-down doses (23 mL) were logged with no change at all.
constexpr int PH_NO_RESPONSE_DOSES = 3;
constexpr float PH_NO_RESPONSE_MIN_EXPECTED = 0.15f;

// Nutrient no-response fault: a settled nutrient dose whose TDS rise is under
// EC_NO_RESPONSE_FRACTION of what the tuned gain predicts (and under
// EC_MIN_DELTA_PPM above it) is unanswered; EC_NO_RESPONSE_DOSES in a row stop
// nutrient dosing until an operator checks the line. 14 Oct: after one partial
// dose, eight more (87 mL logged) moved TDS by 7 ppm until the session cap.
constexpr int EC_NO_RESPONSE_DOSES = 3;
constexpr float EC_NO_RESPONSE_FRACTION = 0.25f;
constexpr float EC_NO_RESPONSE_MIN_EXPECTED_PPM = 30.0f;

// Dose cap from the Manutec labels: the volume the label chemistry says would
// move the reading from where it is to the middle of the dead range, in a
// tank of the fixed assumed volume:
//   u_cap = (distance to the middle of the dead range) / K_label
// The controller's own dose (lambda |e| / K_hat) is clamped to it, so a wrong
// gain estimate cannot dose more than the label says the error needs.
// Both gains scale as 1/V. The caps use the volume from the level reading
// (derated 10 %, so a level error gives a smaller dose); V_ASSUMED_L is used
// when no level reading is available.
constexpr float V_ASSUMED_L = 6.5f;
constexpr float CAP_VOLUME_DERATE = 0.90f;
constexpr float CAP_VOLUME_MIN_L = 2.0f;
// Never assume more than 7.0 L (42 % level) for a cap: a level reading stuck
// high cannot then enlarge a dose. At the usual 50 % fill (7.8 L) the derated
// level volume is 7.06 L, so this changes nothing in normal running.
constexpr float CAP_VOLUME_MAX_L = 7.0f;
// Nutrient: worst-case (largest) gain measured, 262 ppm L/mL (below). Sized and
// capped at this gain, a dose never passes the middle of the dead range.
constexpr float EC_GAIN_REF_VOLUME_L = 8.4f;
// 6 Oct 20:26 run 1: 32.7 mL, 22 -> 1041 ppm settled at 8.4 L = 262 ppm L/mL, the
// largest of every logged nutrient dose (docs/dose_response/).
constexpr float EC_GAIN_LABEL = 31.2f;  // ppm per mL at EC_GAIN_REF_VOLUME_L
// pH: 10-15 mL per 100 L per pH unit; the strong end (10) gives the smaller cap.
constexpr float PH_LABEL_ML_PER_100L_PER_UNIT = 10.0f;
constexpr float PH_GAIN_LABEL =
    100.0f / (PH_LABEL_ML_PER_100L_PER_UNIT * V_ASSUMED_L);  // 1.54 pH/mL at 6.5 L

// Volume the caps assume: measured volume derated, or V_ASSUMED_L if unknown.
inline float capVolumeL(float measuredL) {
  if (!(measuredL > 0.0f)) {
    return V_ASSUMED_L;
  }
  float v = measuredL * CAP_VOLUME_DERATE;
  if (v < CAP_VOLUME_MIN_L) v = CAP_VOLUME_MIN_L;
  if (v > CAP_VOLUME_MAX_L) v = CAP_VOLUME_MAX_L;
  return v;
}

inline float ecCapGain(float volumeL) {
  return EC_GAIN_LABEL * EC_GAIN_REF_VOLUME_L / volumeL;
}

// Settle detection: after the nutrient wait, dose again only once the filtered
// TDS has stopped rising (slope over the last 5 min below this). With first-order
// mixing of time constant tau, a slope below r means less than r x tau still to
// come, so a slow-mixing tank (6 Oct run 2, tau ~9 min) cannot hide a large rise.
// Still rising at ecSettleMs (20 min): the recheck is judged on the reading then.
constexpr float EC_SETTLED_PPM_PER_MIN = 2.0f;
constexpr unsigned long EC_SLOPE_WINDOW_MS = 5UL * 60UL * 1000UL;

// pH settle detection: a pH check (after a nutrient or pH dose) waits until the
// filtered pH drifts < 0.006 pH/min over 5 min, and checks anyway at
// phSettleMs (20 min). On 7 Oct the nutrient kept pulling pH down ~0.01 pH/min
// for an hour, so 0.57 mL pH-up doses given during the drift showed no net
// change and the gain never updated.
constexpr float PH_SETTLED_PER_MIN = 0.006f;
constexpr unsigned long PH_SLOPE_WINDOW_MS = 5UL * 60UL * 1000UL;

// Settle waits end as soon as the reading has settled, not at a fixed time: a
// recheck may come SETTLE_MIN_MS after the dose (one slope window of samples)
// once the slope is below its threshold, polled every SETTLE_POLL_MS. The
// nominal wait (ecSettleMs / phSettleMs, 20 min) is the longest wait (1.9.23;
// it used to stretch to 40 min). 14 Oct: nutrient reached 90 % of its change in 1.4-1.7 min and
// pH-down in under 1 min, but every recheck waited 20-40 min; pH-up (carbonate)
// took 19 min and pH-down then rebounded for ~30 min, which the slope test sees.
constexpr unsigned long SETTLE_MIN_MS = 5UL * 60UL * 1000UL;
constexpr unsigned long SETTLE_POLL_MS = 60UL * 1000UL;

// A cycle started by a reading outside its dead limits that ends without a dose
// (session cap, no-response fault, empty bottle, TDS probe dry) waits this long
// before the limits can start another, so a stuck reading is not re-checked on
// every pass.
constexpr unsigned long LIMIT_CYCLE_RETRY_MS = 20UL * 60UL * 1000UL;

// Bottle rate: 10-15 mL per 100 L per pH unit; the strong end (10) gives the
// larger effect per mL, so a dose sized from it cannot pass the target.
inline float phLabelGain(float volumeL) {
  return 100.0f / (PH_LABEL_ML_PER_100L_PER_UNIT * volumeL);
}


// pH gain (pH per mL) for dose sizing at this volume: PH_UP_GAIN_PHL_PER_ML /
// PH_DOWN_GAIN_PHL_PER_ML over V. tdsPpm is kept for the call sites and unused:
// the TDS does not predict the buffering (see above).
inline float phTunedGain(float volumeL, bool up, float tdsPpm) {
  (void)tdsPpm;
  return (up ? PH_UP_GAIN_PHL_PER_ML : PH_DOWN_GAIN_PHL_PER_ML) / volumeL;
}

// Nutrient session cap (mL delivered per grow-stage session). About twice the
// nutrient needed for a full bring-up from plain water (~45 mL at 6.5 L).
constexpr float EC_SESSION_CAP_ML = 100.0f;

// A TDS probe out of the water reads 0; even rain or RO water reads over 10 ppm.
// Below this, no nutrient is dosed: the reading is a dry probe, not plain water.
constexpr float TDS_PROBE_DRY_PPM = 5.0f;

// Dead-limit margins used when the dashboard sends only a target band.
constexpr float EC_DEAD_MARGIN_PPM = 50.0f;
constexpr float PH_DEAD_HALF_WIDTH = 0.5f;   // dead limits = setpoint +/- 0.5
constexpr float PH_TARGET_MIN_HALF_WIDTH = 0.2f;

// T_w (water): after a refill the top-up has diluted the tank, so the nutrient
// check waits until TDS stops moving: |slope| within the larger of
// EC_SETTLED_PPM_PER_MIN and WATER_MIX_SETTLED_FRACTION_PER_MIN of the reading,
// over WATER_MIX_SLOPE_SPAN_MS of samples, at most WATER_SETTLE_MS. (Before
// 1.9.24 this was a fixed 20 min.) The level itself needs no wait: the stick
// reads live, and the pump stops as soon as the level reaches the target.
constexpr unsigned long WATER_SETTLE_MS = 20UL * 60UL * 1000UL;
constexpr unsigned long WATER_MIX_SLOPE_SPAN_MS = 2UL * 60UL * 1000UL;
constexpr float WATER_MIX_SETTLED_FRACTION_PER_MIN = 0.005f;  // 0.5 % of the reading per min

// Top-up refill (1.9.24). Each top-up is sized from the water pump's
// calibration (the remaining deficit) and runs at most TOPUP_CHUNK_MAX_MS, so
// the peristaltic pump is never on for long; it rests TOPUP_REST_MS between
// chunks and the level is re-checked straight after, until the drawdown is
// back at the target. A refill that has not reached the target after
// TOPUP_REFILL_LIMIT_MS stops and hands over to nutrient and pH, and no new
// refill starts until the next level interval.
constexpr unsigned long TOPUP_CHUNK_MAX_MS = 3UL * 60UL * 1000UL;
constexpr unsigned long TOPUP_REST_MS = 30UL * 1000UL;
constexpr unsigned long TOPUP_REFILL_LIMIT_MS = 90UL * 60UL * 1000UL;

// Level loop (1.9.24): on/off with hysteresis, the band one sensor step wide.
// The Grove stick reads in 5 mm steps, so a refill starts once the smoothed
// drawdown has been past target + tolerance for LEVEL_HIGH_CONFIRM_MS (no
// timer needed; LEVEL_CHECK_MS stays as a backstop), and the pump stops once
// the smoothed level has been at the target for TOPUP_CUTOFF_HOLD_MS.
constexpr unsigned long LEVEL_HIGH_CONFIRM_MS = 60UL * 1000UL;
constexpr unsigned long TOPUP_CUTOFF_HOLD_MS = 10UL * 1000UL;
constexpr float LEVEL_STEP_MM = 5.0f;
// The tank's mL per mm of level is learned from each refill: the volume pumped
// between two step crossings / the step height, averaged half and half with
// the estimate, within these bounds (model 112 mL/mm for 280 x 400 mm).
// Seed: the README's measured inner size, 280 x 432 mm (water_fill_cal), not
// tank_geometry.h (280 x 400 mm), which also sizes chemistry doses and is left as is.
constexpr float REFILL_ML_PER_MM_SEED = 121.0f;
constexpr float REFILL_ML_PER_MM_MIN = 50.0f;
constexpr float REFILL_ML_PER_MM_MAX = 250.0f;
// More than this many steps' worth of water with no step change: the stick,
// a leak or an empty source is wrong, so the refill holds for the operator.
constexpr float REFILL_NO_RISE_STEPS = 2.0f;

// Post-boot hold before the first automatic action (sensor filters warm up).
constexpr unsigned long POST_BOOT_HOLD_MS = 10UL * 60UL * 1000UL;

}  // namespace ControlDesign

#endif  // SMART_GARDEN_CONTROL_DESIGN_H
