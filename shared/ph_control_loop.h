/************************************************************
 * pH control loop — adaptive dead-band P
 *
 * Used identically in PH_BASELINE, SEEDLING, VEGETATIVE, HARVEST.
 * Caller owns pump actuation and settle waits. gainUp / gainDown are
 * the process gain estimates K_up / K_down in pH per mL of reagent.
 ************************************************************/

#ifndef SMART_GARDEN_PH_CONTROL_LOOP_H
#define SMART_GARDEN_PH_CONTROL_LOOP_H

#include <Arduino.h>
#include <math.h>
#include "control_design.h"
#include "grow_stage.h"
#include "proportional_dose.h"

namespace PhControl {

enum Direction {
  NONE = 0,
  UP,
  DOWN
};

struct State {
  float gainUp = 1.5f;           // K_up estimate, pH per mL
  float gainDown = 1.5f;         // K_down estimate, pH per mL
  bool gainUpConfirmed = false;
  bool gainDownConfirmed = false;
  float cumulativeSessionMl = 0.0f;
  bool sessionCapAlert = false;
  float lastDoseMl = 0.0f;
  Direction lastDirection = NONE;
  float phBeforeLastDose = NAN;
  bool baselineStablePending = false;
  unsigned long baselineStableSinceMs = 0;
  // No-response fault (see ControlDesign::PH_NO_RESPONSE_DOSES): doses in a row
  // that did not move pH, the mL they delivered, and the latched fault per
  // direction. The fault is cleared only by the operator (clearNoResponse).
  int noResponseUp = 0;
  int noResponseDown = 0;
  float noResponseMlUp = 0.0f;
  float noResponseMlDown = 0.0f;
  bool noResponseFaultUp = false;
  bool noResponseFaultDown = false;
  // Last settled dose: observed / predicted pH change (NAN if it did not move
  // pH or was too small to judge) and its volume. Read by the solution monitor.
  float lastResponseRatio = NAN;
  float lastResponseMl = 0.0f;
  uint32_t responseSeq = 0;      // bumped at every settled dose
};

struct CheckResult {
  Direction direction = NONE;
  float doseMl = 0.0f;
  bool inBand = false;
  bool noAction = true;
  bool baselineComplete = false;
  bool sessionCapHit = false;
  bool noResponseHit = false;  // dose wanted, but that direction's no-response fault is latched
  const char* reason = "";
};

inline void resetSession(State& state) {
  state.cumulativeSessionMl = 0.0f;
  state.sessionCapAlert = false;
  state.baselineStablePending = false;
  state.baselineStableSinceMs = 0;
}

inline void clearNoResponse(State& state, Direction dir) {
  if (dir != DOWN) {
    state.noResponseUp = 0;
    state.noResponseMlUp = 0.0f;
    state.noResponseFaultUp = false;
  }
  if (dir != UP) {
    state.noResponseDown = 0;
    state.noResponseMlDown = 0.0f;
    state.noResponseFaultDown = false;
  }
}

inline bool noResponseFault(const State& state, Direction dir) {
  return dir == UP ? state.noResponseFaultUp : dir == DOWN ? state.noResponseFaultDown : false;
}

inline void initGains(State& state, const GrowStage::Config& cfg) {
  state.gainUp = cfg.phGainUpInitial;
  state.gainDown = cfg.phGainDownInitial;
  state.gainUpConfirmed = false;
  state.gainDownConfirmed = false;
  state.lastDoseMl = 0.0f;
  state.lastDirection = NONE;
  state.phBeforeLastDose = NAN;
}

// Median of samples with simple outlier rejection (drop farthest from median).
inline float filteredReading(const float* samples, int count) {
  if (samples == nullptr || count <= 0) {
    return NAN;
  }
  if (count == 1) {
    return samples[0];
  }

  float sorted[16];
  int n = count > 16 ? 16 : count;
  for (int i = 0; i < n; i++) {
    sorted[i] = samples[i];
  }
  for (int i = 1; i < n; i++) {
    float key = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > key) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = key;
  }

  float median = (n % 2 == 1)
                     ? sorted[n / 2]
                     : 0.5f * (sorted[n / 2 - 1] + sorted[n / 2]);

  float sum = 0.0f;
  int kept = 0;
  const float maxDev = 0.25f;
  for (int i = 0; i < n; i++) {
    if (fabsf(sorted[i] - median) <= maxDev) {
      sum += sorted[i];
      kept++;
    }
  }
  if (kept == 0) {
    return median;
  }
  return sum / static_cast<float>(kept);
}

inline CheckResult evaluateCheck(
    float ph,
    const GrowStage::Config& cfg,
    State& state,
    bool baselineMode,
    unsigned long nowMs,
    float volumeL = ControlDesign::V_ASSUMED_L,
    float tdsPpm = NAN) {
  CheckResult result;

  if (isnan(ph)) {
    result.reason = "ph_invalid";
    return result;
  }

  if (state.sessionCapAlert || (cfg.phSessionCapMl > 0.0f &&
                                state.cumulativeSessionMl >= cfg.phSessionCapMl)) {
    state.sessionCapAlert = true;
    result.sessionCapHit = true;
    result.reason = "session_cap";
    return result;
  }

  result.inBand = ProportionalDose::inTargetBand(ph, cfg.phTargetMin, cfg.phTargetMax);

  if (baselineMode) {
    if (result.inBand) {
      if (!state.baselineStablePending) {
        state.baselineStablePending = true;
        state.baselineStableSinceMs = nowMs;
        result.reason = "baseline_band_enter";
        return result;
      }
      if (nowMs - state.baselineStableSinceMs >= cfg.phBaselineRecheckMs) {
        result.baselineComplete = true;
        result.inBand = true;
        result.reason = "baseline_stable";
        return result;
      }
      result.reason = "baseline_recheck_wait";
      return result;
    }
    state.baselineStablePending = false;
    state.baselineStableSinceMs = 0;
  }

  const bool low = ProportionalDose::belowDeadLow(ph, cfg.phDeadLow);
  const bool high = ProportionalDose::aboveDeadHigh(ph, cfg.phDeadHigh);
  if (low || high) {
    if (noResponseFault(state, low ? UP : DOWN)) {
      result.noResponseHit = true;
      result.reason = "ph_no_response";
      return result;
    }
    const float error = fabsf(ph - cfg.phCenter);
    result.direction = low ? UP : DOWN;
    // Gain for this volume: the largest response measured (control_design.h), so
    // lambda (0.8) |e| / K lands short in any tank buffered like the ones seen.
    const float kHat = ControlDesign::phTunedGain(volumeL, low, tdsPpm);
    if (low) {
      state.gainUp = kHat;
    } else {
      state.gainDown = kHat;
    }
    // Dose limit: the bottle rate's volume to the setpoint. The gain sizes the
    // dose; this only bounds a gain that is wrong.
    const float capMl = ProportionalDose::labelCapMl(error, ControlDesign::phLabelGain(volumeL));
    result.doseMl = ProportionalDose::doseForError(
        error, kHat, ControlDesign::LAMBDA_PH, cfg.phMinDoseMl, capMl, cfg.phMaxDoseMl);
    // Never plan past the session cap.
    if (cfg.phSessionCapMl > 0.0f) {
      const float left = cfg.phSessionCapMl - state.cumulativeSessionMl;
      if (result.doseMl > left) {
        result.doseMl = left;
      }
    }
    result.noAction = result.doseMl <= 0.0f;
    result.reason = low ? "ph_low" : "ph_high";
    return result;
  }

  result.reason = result.inBand ? "in_band" : "dead_zone";
  return result;
}

inline void recordDoseStart(State& state, Direction dir, float phBefore) {
  state.lastDirection = dir;
  state.lastDoseMl = 0.0f;
  state.phBeforeLastDose = phBefore;
  state.baselineStablePending = false;
  state.baselineStableSinceMs = 0;
}

// Called when the pump stops, with the volume it actually delivered.
inline void recordDelivered(State& state, float deliveredMl) {
  if (isnan(deliveredMl) || deliveredMl <= 0.0f) {
    return;
  }
  state.lastDoseMl = deliveredMl;
  state.cumulativeSessionMl += deliveredMl;
}

// Counts a settled dose that did not move pH; returns the direction whose
// no-response fault this recheck latched (NONE if none). A dose too small to
// show (expected change < PH_NO_RESPONSE_MIN_EXPECTED) neither counts nor resets.
inline Direction recordResponse(State& state, float signedDelta) {
  const bool up = state.lastDirection == UP;
  const float gain = up ? state.gainUp : state.gainDown;
  const float expected = state.lastDoseMl * gain;
  int& count = up ? state.noResponseUp : state.noResponseDown;
  float& ml = up ? state.noResponseMlUp : state.noResponseMlDown;
  bool& fault = up ? state.noResponseFaultUp : state.noResponseFaultDown;
  state.lastResponseMl = state.lastDoseMl;
  state.responseSeq++;
  state.lastResponseRatio =
      signedDelta >= ControlDesign::PH_MIN_DELTA &&
              expected >= ControlDesign::PH_NO_RESPONSE_MIN_EXPECTED
          ? signedDelta / expected
          : NAN;
  if (signedDelta >= ControlDesign::PH_MIN_DELTA) {
    count = 0;
    ml = 0.0f;
    return NONE;
  }
  if (expected < ControlDesign::PH_NO_RESPONSE_MIN_EXPECTED) {
    return NONE;
  }
  count++;
  ml += state.lastDoseMl;
  if (!fault && count >= ControlDesign::PH_NO_RESPONSE_DOSES) {
    fault = true;
    return state.lastDirection;
  }
  return NONE;
}

// Returns the direction whose no-response fault this recheck latched, if any.
inline Direction updateGainAfterRecheck(State& state, float phAfter) {
  state.lastResponseRatio = NAN;
  state.lastResponseMl = 0.0f;
  if (state.lastDirection == NONE || state.lastDoseMl <= 0.0f ||
      isnan(state.phBeforeLastDose) || isnan(phAfter)) {
    return NONE;
  }
  // The recheck runs once pH has stopped drifting (settle detection), so the
  // change is the dose's net effect, including what the buffer absorbed. A
  // weak, zero or wrong-way change is recorded as a weak gain (floored at
  // PH_GAIN_MIN) rather than ignored: in a buffered tank (nutrient phosphate,
  // potassium carbonate pH-up) the estimate then falls and the next dose grows,
  // up to the hard limit, until pH moves - the integral action a P loop lacks.
  // On 7 Oct ignoring these left every pH-up dose at the unbuffered size.
  const float delta = phAfter - state.phBeforeLastDose;
  const float signedDelta = state.lastDirection == UP ? delta : -delta;
  const Direction tripped = recordResponse(state, signedDelta);
  if (ControlDesign::ADAPTIVE_GAINS) {
    const float observed = signedDelta >= ControlDesign::PH_MIN_DELTA
                               ? signedDelta / state.lastDoseMl
                               : ControlDesign::PH_GAIN_MIN;
    if (state.lastDirection == UP) {
      if (ProportionalDose::updateProcessGain(state.gainUp, observed,
                                              ControlDesign::GAIN_ALPHA,
                                              ControlDesign::PH_GAIN_MIN,
                                              ControlDesign::PH_GAIN_MAX)) {
        state.gainUpConfirmed = true;
      }
    } else if (ProportionalDose::updateProcessGain(state.gainDown, observed,
                                                   ControlDesign::GAIN_ALPHA,
                                                   ControlDesign::PH_GAIN_MIN,
                                                   ControlDesign::PH_GAIN_MAX)) {
      state.gainDownConfirmed = true;
    }
  }
  state.lastDoseMl = 0.0f;
  state.lastDirection = NONE;
  state.phBeforeLastDose = NAN;
  return tripped;
}

}  // namespace PhControl

#endif  // SMART_GARDEN_PH_CONTROL_LOOP_H
