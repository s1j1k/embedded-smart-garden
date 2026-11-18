/************************************************************
 * Grow stage definitions and per-stage control parameters
 *
 * Stages transition on explicit trigger (Serial / API).
 * pH / EC / level loops reuse the same code; stage config
 * selects target drawdown, check intervals, and aggressiveness.
 ************************************************************/

#ifndef SMART_GARDEN_GROW_STAGE_H
#define SMART_GARDEN_GROW_STAGE_H

#include <Arduino.h>
#include <math.h>
#include "control_design.h"

namespace GrowStage {

enum Id {
  FILLING = 0,
  PH_BASELINE,
  SEEDLING,
  TRANSITIONAL,  // half-strength EC; control law identical to VEGETATIVE
  VEGETATIVE,
  HARVEST,
  SENSOR_BASELINE,  // intentional probe-drift capture (logging only)
  IDLE,             // unset / no control until operator picks a stage
  COUNT
};

inline const char* name(Id stage) {
  switch (stage) {
    case FILLING: return "filling";
    case PH_BASELINE: return "ph_baseline";
    case SEEDLING: return "seedling";
    case TRANSITIONAL: return "transitional";
    case VEGETATIVE: return "vegetative";
    case HARVEST: return "harvest";
    case SENSOR_BASELINE: return "sensor_baseline";
    case IDLE: return "idle";
    default: return "unknown";
  }
}

inline bool parseName(const char* token, Id& out) {
  if (token == nullptr) {
    return false;
  }
  String t(token);
  t.trim();
  t.toUpperCase();
  if (t == "IDLE" || t == "UNSET" || t == "NONE" || t == "OFF" || t == "STOP") {
    out = IDLE;
    return true;
  }
  if (t == "FILL" || t == "FILLING" || t == "WATER_FILL" || t == "INIT") {
    out = FILLING;
    return true;
  }
  if (t == "PH_BASELINE" || t == "PRIMING" || t == "PRIMING_PH" || t == "PH") {
    out = PH_BASELINE;
    return true;
  }
  if (t == "SEEDLING" || t == "SEED") {
    out = SEEDLING;
    return true;
  }
  if (t == "TRANSITIONAL" || t == "TRANSITION" || t == "HALF" || t == "HALF_STRENGTH") {
    out = TRANSITIONAL;
    return true;
  }
  // FLOWERING is a legacy name alias onto vegetative (not a separate stage).
  if (t == "VEGETATIVE" || t == "VEG" || t == "FLOWERING" || t == "FLOWER") {
    out = VEGETATIVE;
    return true;
  }
  if (t == "HARVEST") {
    out = HARVEST;
    return true;
  }
  if (t == "BASELINE" || t == "SENSOR_BASELINE" || t == "SENSOR" || t == "MONITOR") {
    out = SENSOR_BASELINE;
    return true;
  }
  return false;
}

struct Config {
  int targetDrawdownMm;
  int drawdownToleranceMm;
  unsigned long levelCheckIntervalMs;
  unsigned long phCheckIntervalMs;
  unsigned long ecCheckIntervalMs;
  bool ecControlEnabled;
  float ecTargetMin;
  float ecTargetMax;
  float ecDeadLow;
  float ecDeadHigh;
  float ecGainInitial;
  float ecMinDoseMl;
  float ecMaxDoseMl;
  unsigned long ecSettleMs;
  float phTargetMin;
  float phTargetMax;
  float phDeadLow;
  float phDeadHigh;
  float phCenter;
  float phGainUpInitial;
  float phGainDownInitial;
  float phMinDoseMl;
  float phMaxDoseMl;
  unsigned long phSettleMs;
  float phSessionCapMl;
  unsigned long phBaselineRecheckMs;
};

/*
 * Gain seeds, dose limits and timing (adaptive dead-band P, firmware 1.9).
 *
 * Gains are process gains: the change in the reading per mL dosed.
 * The controller doses u = lambda |e| / K_hat (ControlDesign), so a seed that
 * is too high under-doses (slow but safe) and one too low over-doses. Either
 * way, the label cap (ControlDesign::*_GAIN_LABEL) limits a dose to the
 * volume the reagent label says the error needs.
 *
 * K_EC seed 14 ppm/mL: 30 Sep - 5 Oct bring-up, 6.5 L working volume.
 *   Median of 26 normal doses 11 ppm/mL; whole run 18 ppm/mL including the
 *   pH-reagent salts; nutrient-only about 14 ppm/mL.
 * K_up / K_down seed 1.5 pH/mL: Manutec label (10 mL / 100 L per pH unit,
 *   strong end) at 6.55 L is 0.66 mL per pH unit = 1.5 pH/mL. The one clean
 *   bench step (2.2 pH/mL at 170 ppm, low buffer) is higher, so in near-plain
 *   water a pH dose sized from the seed is up to 1.47x too large; the label
 *   cap still keeps it from carrying pH past the opposite dead limit (from
 *   pH 4.9 at 2.2 pH/mL the capped dose ends at 6.47).
 *
 * Absolute per-dose ceilings from the Manutec labels at 6.5 L (fault
 * fail-safe, e.g. a probe stuck at zero; the usual limit is the label cap):
 *   nutrient 32.5 mL = one full-strength tank dose (10 mL per 2 L),
 *   pH 1.0 mL = one pH unit at the label's weak end (15 mL per 100 L).
 *
 * Settle waits T_w: how long a dose takes to mix and reach the control input
 * through the 2 mHz Butterworth filter (sensor_ema.h), from every dose of the
 * 30 Sep - 5 Oct run replayed through it (docs/filter_analysis/settle_times.py):
 *   nutrient: median 3.3 min, 90 % within 4.3 min, large steps 3.8-4.4 min
 *             -> 6 min
 *   pH:       the one clean step settled in 9.0 min -> 12 min (to confirm in
 *             the characterisation run; it rests on a single dose)
 */
constexpr float EC_GAIN_SEED_PPM_PER_ML = 14.0f;
constexpr float PH_GAIN_SEED_PER_ML = 1.5f;
constexpr float NUTRIENT_MIN_DOSE_ML = 0.1f;
constexpr float NUTRIENT_HARD_MAX_DOSE_ML = 25.0f;  // 25 x 31 = 775 ppm < 850 from plain water
constexpr float PH_MIN_DOSE_ML = 0.05f;
constexpr float PH_HARD_MAX_DOSE_ML = 3.0f;        // fault guard; the full-correction cap sizes doses
// 6 Oct bring-ups: TDS settled to 5 % in 8.7 min in one run but was still
// rising 15+ min after each dose in the next (slower mixing at 24.7 C), and
// the nutrient kept pulling pH down for ~30 min. 20 min covers both. Since
// 1.9.22 a recheck comes as soon as the reading has settled (from
// SETTLE_MIN_MS, 5 min); since 1.9.23 these are also the longest waits.
constexpr unsigned long EC_SETTLE_MS = 20UL * 60 * 1000;
constexpr unsigned long PH_SETTLE_MS = 20UL * 60 * 1000;
// No timer for the chemistry loops (check interval 0): a cycle starts as soon
// as the filtered TDS or pH leaves its dead limits, or on a manual CYCLE, and
// runs level -> nutrient -> pH, waiting only for each dose to settle. The level
// has its own timer because evaporation gives no limit to trigger on.
constexpr unsigned long LEVEL_CHECK_MS = 30UL * 60 * 1000;
// PH_BASELINE still checks pH on a timer: each check records whether the plain
// water's pH has held stable over the stability window.
constexpr unsigned long PH_BASELINE_CHECK_MS = 40UL * 60 * 1000;

// TRANSITIONAL EC = VEGETATIVE's live EC/TDS band × this fraction (pH unchanged).
// Tracks catalogue-applied vegetative setpoints — never a separately hardcoded EC.
constexpr float TRANSITIONAL_STRENGTH_FRACTION = 0.5f;

// pH defaults (every stage): setpoint 6.0, target band 5.8-6.2, dead limits
// 5.5-6.5. The dead limits are the plants' tolerance range, not the optimum:
// leafy greens grow normally anywhere in 5.5-6.5 and the plants themselves
// move pH within it, so the controller acts only outside it. The dashboard
// plant catalogue overwrites these for VEGETATIVE (and TRANSITIONAL).
//
// Field order: drawdown, tolerance, level/pH/EC check intervals,
// ecControlEnabled, EC target min/max, EC dead low/high, K_EC seed,
// EC min dose, EC hard max dose, EC settle, pH target min/max, pH dead
// low/high, pH setpoint, K_up seed, K_down seed, pH min dose, pH hard max
// dose, pH settle, pH session cap, PH_BASELINE stability window.
constexpr Config STAGE_CONFIGS[COUNT] = {
  // FILLING — level only; no auto chemistry
  {
    0, 10,
    30UL * 60 * 1000,   // level check every 30 min (manual fill watch)
    0, 0,
    false, 0, 0, 0, 0, 0, 0, 0, 0,
    5.8f, 6.2f, 5.5f, 6.5f, 6.0f,
    PH_GAIN_SEED_PER_ML, PH_GAIN_SEED_PER_ML, PH_MIN_DOSE_ML, 0.5f,
    PH_SETTLE_MS, 0.0f, 30UL * 60 * 1000
  },
  // PH_BASELINE — pH only on plain water
  {
    0, 10,
    LEVEL_CHECK_MS,
    PH_BASELINE_CHECK_MS,
    0,
    false, 0, 0, 0, 0, 0, 0, 0, 0,
    5.8f, 6.2f, 5.5f, 6.5f, 6.0f,
    PH_GAIN_SEED_PER_ML, PH_GAIN_SEED_PER_ML, PH_MIN_DOSE_ML, PH_HARD_MAX_DOSE_ML,
    PH_SETTLE_MS, 5.0f, 30UL * 60 * 1000
  },
  // SEEDLING — hold near fill_0; low-disturbance pH; no EC dosing
  {
    0, 10,
    LEVEL_CHECK_MS,
    0,                  // pH: on its dead limits, no timer
    0,
    false, 0, 0, 0, 0, 0, 0, 0, 0,
    5.8f, 6.2f, 5.5f, 6.5f, 6.0f,
    PH_GAIN_SEED_PER_ML, PH_GAIN_SEED_PER_ML, PH_MIN_DOSE_ML, 0.5f,
    PH_SETTLE_MS, 5.0f, 30UL * 60 * 1000
  },
  // TRANSITIONAL — same control as VEGETATIVE; EC scaled at runtime from veg
  // (placeholder EC numbers below are overwritten by config() each call).
  // Gain seeds and dose limits match vegetative: the law divides the error by
  // K_hat, so a half-strength band needs no separate tuning.
  {
    20, 5,      // air gap below net pots (mm), tolerance
    LEVEL_CHECK_MS,
    0, 0,               // pH, EC: on their dead limits, no timer
    true, 300.0f, 400.0f, 250.0f, 450.0f,
    EC_GAIN_SEED_PPM_PER_ML, NUTRIENT_MIN_DOSE_ML, NUTRIENT_HARD_MAX_DOSE_ML,
    EC_SETTLE_MS,
    5.8f, 6.2f, 5.5f, 6.5f, 6.0f,
    PH_GAIN_SEED_PER_ML, PH_GAIN_SEED_PER_ML, PH_MIN_DOSE_ML, PH_HARD_MAX_DOSE_ML,
    PH_SETTLE_MS, 10.0f, 30UL * 60 * 1000
  },
  // VEGETATIVE — 20 mm air gap; nutrient then pH. EC defaults are the
  // basil + lettuce catalogue overlap (600-800 ppm500).
  {
    20, 5,      // air gap below net pots (mm), tolerance
    LEVEL_CHECK_MS,
    0, 0,               // pH, EC: on their dead limits, no timer
    true, 600.0f, 800.0f, 550.0f, 850.0f,
    EC_GAIN_SEED_PPM_PER_ML, NUTRIENT_MIN_DOSE_ML, NUTRIENT_HARD_MAX_DOSE_ML,
    EC_SETTLE_MS,
    5.8f, 6.2f, 5.5f, 6.5f, 6.0f,
    PH_GAIN_SEED_PER_ML, PH_GAIN_SEED_PER_ML, PH_MIN_DOSE_ML, PH_HARD_MAX_DOSE_ML,
    PH_SETTLE_MS, 10.0f, 30UL * 60 * 1000
  },
  // HARVEST — 30 mm air gap; low-EC flush, longer intervals
  {
    30, 5,      // air gap below net pots (mm), tolerance
    LEVEL_CHECK_MS,
    0, 0,               // pH, EC: on their dead limits, no timer
    true, 0.0f, 200.0f, 0.0f, 250.0f,
    EC_GAIN_SEED_PPM_PER_ML, NUTRIENT_MIN_DOSE_ML, 10.0f,
    EC_SETTLE_MS,
    5.8f, 6.2f, 5.5f, 6.5f, 6.0f,
    PH_GAIN_SEED_PER_ML, PH_GAIN_SEED_PER_ML, PH_MIN_DOSE_ML, 0.5f,
    PH_SETTLE_MS, 5.0f, 45UL * 60 * 1000
  },
  // SENSOR_BASELINE — passive logging only (no pumps; TDS/pH probe drift capture)
  {
    0, 10,
    0, 0, 0,
    false, 0, 0, 0, 0, 0, 0, 0, 0,
    5.8f, 6.2f, 5.5f, 6.5f, 6.0f,
    PH_GAIN_SEED_PER_ML, PH_GAIN_SEED_PER_ML, PH_MIN_DOSE_ML, 0.5f,
    PH_SETTLE_MS, 0.0f, 30UL * 60 * 1000
  },
  // IDLE — no stage chosen yet; sensors only, no auto control
  {
    0, 10,
    0, 0, 0,
    false, 0, 0, 0, 0, 0, 0, 0, 0,
    5.8f, 6.2f, 5.5f, 6.5f, 6.0f,
    PH_GAIN_SEED_PER_ML, PH_GAIN_SEED_PER_ML, PH_MIN_DOSE_ML, 0.5f,
    PH_SETTLE_MS, 0.0f, 30UL * 60 * 1000
  }
};

/** Mutable per-stage copy (defaults from STAGE_CONFIGS). Dashboard MQTT
 *  `setpoints` may rewrite chemistry bands without a firmware flash. */
inline Config* runtimeConfigs() {
  static Config configs[COUNT];
  static bool ready = false;
  if (!ready) {
    for (int i = 0; i < COUNT; i++) {
      configs[i] = STAGE_CONFIGS[i];
    }
    ready = true;
  }
  return configs;
}

/** Scale EC/TDS fields in-place; leave pH and dose/gain clamps unchanged. */
inline void scaleEcBand(Config& cfg, float fraction) {
  cfg.ecTargetMin *= fraction;
  cfg.ecTargetMax *= fraction;
  cfg.ecDeadLow *= fraction;
  cfg.ecDeadHigh *= fraction;
}

/**
 * Effective stage config. TRANSITIONAL is a live view of VEGETATIVE with EC
 * scaled by TRANSITIONAL_STRENGTH_FRACTION — refreshed every call so catalogue
 * setpoint updates to vegetative are visible immediately.
 */
inline const Config& config(Id stage) {
  if (stage >= COUNT) {
    return runtimeConfigs()[SEEDLING];
  }
  if (stage == TRANSITIONAL) {
    static Config transitionalView;
    transitionalView = runtimeConfigs()[VEGETATIVE];
    scaleEcBand(transitionalView, TRANSITIONAL_STRENGTH_FRACTION);
    return transitionalView;
  }
  return runtimeConfigs()[stage];
}

inline Config& mutableConfig(Id stage) {
  // TRANSITIONAL has no independent mutable store — edits go to VEGETATIVE.
  if (stage == TRANSITIONAL) {
    return runtimeConfigs()[VEGETATIVE];
  }
  if (stage >= COUNT) {
    return runtimeConfigs()[SEEDLING];
  }
  return runtimeConfigs()[stage];
}

inline void resetConfig(Id stage) {
  if (stage >= COUNT) {
    return;
  }
  if (stage == TRANSITIONAL) {
    // Reset underlying vegetative defaults (transitional view re-derives).
    runtimeConfigs()[VEGETATIVE] = STAGE_CONFIGS[VEGETATIVE];
    return;
  }
  runtimeConfigs()[stage] = STAGE_CONFIGS[stage];
}

/**
 * Apply resolved EC (ppm 500-scale) + pH bands to a stage.
 *
 * ecMin/ecMax and phMin/phMax are the target (optimal) bands; the setpoint is
 * the middle of each (or phSetpoint when given). The dead limits are where the
 * controller starts dosing. The dashboard sends them explicitly (the plants'
 * tolerance range); when it does not, they fall back to the target band
 * +/- EC_DEAD_MARGIN_PPM and setpoint +/- PH_DEAD_HALF_WIDTH.
 *
 * A zero-width pH target (e.g. basil 5.5-6.0 with lettuce 6.0-7.0 overlaps
 * only at 6.0) is a setpoint, not a band: the target band is widened to
 * setpoint +/- PH_TARGET_MIN_HALF_WIDTH so "in band" has a meaning.
 *
 * TRANSITIONAL redirects to VEGETATIVE (scaled view tracks automatically).
 * Returns false if the ranges are invalid.
 */
inline bool applyChemistryBands(
    Id stage,
    float ecMinPpm,
    float ecMaxPpm,
    float phMin,
    float phMax,
    float ecDeadLow = NAN,
    float ecDeadHigh = NAN,
    float phDeadLow = NAN,
    float phDeadHigh = NAN,
    float phSetpoint = NAN) {
  if (stage == TRANSITIONAL) {
    stage = VEGETATIVE;
  }
  if (stage >= COUNT || isnan(ecMinPpm) || isnan(ecMaxPpm) || isnan(phMin) ||
      isnan(phMax) || ecMinPpm > ecMaxPpm || phMin > phMax) {
    return false;
  }
  const float center = isnan(phSetpoint) ? 0.5f * (phMin + phMax) : phSetpoint;
  if (center < phMin - 0.5f || center > phMax + 0.5f) {
    return false;
  }
  if (isnan(ecDeadLow)) {
    ecDeadLow = ecMinPpm - ControlDesign::EC_DEAD_MARGIN_PPM;
  }
  if (isnan(ecDeadHigh)) {
    ecDeadHigh = ecMaxPpm + ControlDesign::EC_DEAD_MARGIN_PPM;
  }
  if (ecDeadLow < 0.0f) {
    ecDeadLow = 0.0f;
  }
  if (isnan(phDeadLow)) {
    phDeadLow = center - ControlDesign::PH_DEAD_HALF_WIDTH;
  }
  if (isnan(phDeadHigh)) {
    phDeadHigh = center + ControlDesign::PH_DEAD_HALF_WIDTH;
  }
  // Dead limits must enclose the target band and the setpoint.
  if (ecDeadLow > ecMinPpm || ecDeadHigh < ecMaxPpm || phDeadLow > center ||
      phDeadHigh < center) {
    return false;
  }
  float tMin = phMin;
  float tMax = phMax;
  if (tMax - tMin < 2.0f * ControlDesign::PH_TARGET_MIN_HALF_WIDTH) {
    tMin = center - ControlDesign::PH_TARGET_MIN_HALF_WIDTH;
    tMax = center + ControlDesign::PH_TARGET_MIN_HALF_WIDTH;
  }
  if (tMin < phDeadLow) {
    tMin = phDeadLow;
  }
  if (tMax > phDeadHigh) {
    tMax = phDeadHigh;
  }
  Config& cfg = mutableConfig(stage);
  cfg.ecTargetMin = ecMinPpm;
  cfg.ecTargetMax = ecMaxPpm;
  cfg.ecDeadLow = ecDeadLow;
  cfg.ecDeadHigh = ecDeadHigh;
  cfg.phTargetMin = tMin;
  cfg.phTargetMax = tMax;
  cfg.phDeadLow = phDeadLow;
  cfg.phDeadHigh = phDeadHigh;
  cfg.phCenter = center;
  return true;
}

inline bool allowsEc(Id stage) {
  return config(stage).ecControlEnabled;
}

inline bool allowsPh(Id stage) {
  return stage == PH_BASELINE || (stage >= SEEDLING && stage < SENSOR_BASELINE);
}

inline bool allowsLevelAuto(Id stage) {
  return stage >= SEEDLING && stage < SENSOR_BASELINE;
}

/** No auto chemistry / level dosing (idle unset, or intentional sensor baseline). */
inline bool isPassiveLogging(Id stage) {
  return stage == SENSOR_BASELINE || stage == IDLE;
}

inline bool isIdle(Id stage) {
  return stage == IDLE;
}

}  // namespace GrowStage

#endif  // SMART_GARDEN_GROW_STAGE_H
