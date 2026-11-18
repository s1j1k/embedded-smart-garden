/************************************************************
 * EC / nutrient control loop — adaptive dead-band P
 *
 * Runs before pH correction in TRANSITIONAL, VEGETATIVE and HARVEST.
 * gainEc is the process gain estimate K_EC in ppm per mL of nutrient.
 ************************************************************/

#ifndef SMART_GARDEN_EC_CONTROL_LOOP_H
#define SMART_GARDEN_EC_CONTROL_LOOP_H

#include <Arduino.h>
#include <math.h>
#include "control_design.h"
#include "grow_stage.h"
#include "proportional_dose.h"

namespace EcControl {

struct State {
  float gainEc = 14.0f;          // K_EC estimate, ppm per mL
  bool gainConfirmed = false;    // updated from a dose-response pair since seeding
  float cumulativeSessionMl = 0.0f;
  bool sessionCapAlert = false;
  float lastDoseMl = 0.0f;       // delivered volume of the dose awaiting recheck
  float ecBeforeLastDose = NAN;
  // No-response fault (ControlDesign::EC_NO_RESPONSE_DOSES): doses in a row that
  // did not raise TDS, the mL they delivered, and the latched fault, cleared
  // only by the operator (clearNoResponse).
  int noResponseCount = 0;
  float noResponseMl = 0.0f;
  bool noResponseFault = false;
};

struct CheckResult {
  float doseMl = 0.0f;
  bool inBand = false;
  bool noAction = true;
  bool sessionCapHit = false;
  bool noResponseHit = false;   // dose wanted, but the no-response fault is latched
  const char* reason = "";
};

inline void clearNoResponse(State& state) {
  state.noResponseCount = 0;
  state.noResponseMl = 0.0f;
  state.noResponseFault = false;
}

inline void resetSession(State& state) {
  state.cumulativeSessionMl = 0.0f;
  state.sessionCapAlert = false;
}

inline void initGain(State& state, const GrowStage::Config& cfg) {
  state.gainEc = cfg.ecGainInitial;
  state.gainConfirmed = false;
  state.lastDoseMl = 0.0f;
  state.ecBeforeLastDose = NAN;
}

// volumeL: cap volume from ControlDesign::capVolumeL().
inline CheckResult evaluateCheck(float ecPpm, const GrowStage::Config& cfg, State& state,
                                 float volumeL = ControlDesign::V_ASSUMED_L) {
  CheckResult result;

  if (!cfg.ecControlEnabled || isnan(ecPpm)) {
    result.reason = cfg.ecControlEnabled ? "ec_invalid" : "ec_disabled";
    return result;
  }

  if (state.sessionCapAlert ||
      state.cumulativeSessionMl >= ControlDesign::EC_SESSION_CAP_ML) {
    state.sessionCapAlert = true;
    result.sessionCapHit = true;
    result.reason = "session_cap";
    return result;
  }

  result.inBand = ProportionalDose::inTargetBand(ecPpm, cfg.ecTargetMin, cfg.ecTargetMax);

  if (ProportionalDose::belowDeadLow(ecPpm, cfg.ecDeadLow)) {
    if (state.noResponseFault) {
      result.noResponseHit = true;
      result.reason = "ec_no_response";
      return result;
    }
    const float targetMid = 0.5f * (cfg.ecTargetMin + cfg.ecTargetMax);
    const float error = fabsf(targetMid - ecPpm);
    // Cap: the volume that would land TDS on capTarget at the worst-case gain.
    // The learned gain only shrinks doses (through lambda e / K_hat): a gain
    // learned from a part-mixed reading reads low and would open the cap. Far
    // below the band (more than one band-width under the middle) the cap aims at
    // the middle, keeping half the band for gain error and mixing not yet seen;
    // closer in it may aim at the band's top edge, so a small dose can still
    // reach the band.
    const float bandWidth = cfg.ecTargetMax - cfg.ecTargetMin;
    const float capTarget = (targetMid - ecPpm > bandWidth) ? targetMid : cfg.ecTargetMax;
    const float kTuned = ControlDesign::ecCapGain(volumeL);
    if (!ControlDesign::ADAPTIVE_GAINS) {
      state.gainEc = kTuned;
    }
    const float capMl = ProportionalDose::labelCapMl(capTarget - ecPpm, kTuned);
    result.doseMl = ProportionalDose::doseForError(
        error, state.gainEc, ControlDesign::LAMBDA, cfg.ecMinDoseMl, capMl, cfg.ecMaxDoseMl);
    // Never plan past the session cap.
    const float left = ControlDesign::EC_SESSION_CAP_ML - state.cumulativeSessionMl;
    if (result.doseMl > left) {
      result.doseMl = left;
    }
    if (result.doseMl < cfg.ecMinDoseMl) {
      result.doseMl = 0.0f;
    }
    result.noAction = result.doseMl <= 0.0f;
    result.reason = "ec_low";
    return result;
  }

  if (ProportionalDose::aboveDeadHigh(ecPpm, cfg.ecDeadHigh)) {
    result.reason = "ec_high_no_dilute";
    return result;
  }

  result.reason = result.inBand ? "in_band" : "dead_zone";
  return result;
}

inline void recordDoseStart(State& state, float ecBefore) {
  state.lastDoseMl = 0.0f;
  state.ecBeforeLastDose = ecBefore;
}

// Called when the pump stops, with the volume it actually delivered.
inline void recordDelivered(State& state, float deliveredMl) {
  if (isnan(deliveredMl) || deliveredMl <= 0.0f) {
    return;
  }
  state.lastDoseMl = deliveredMl;
  state.cumulativeSessionMl += deliveredMl;
}

// Counts a settled dose that did not raise TDS; true when this recheck latched
// the no-response fault. A dose too small to show neither counts nor resets.
inline bool recordResponse(State& state, float delta) {
  const float expected = state.lastDoseMl * state.gainEc;
  if (expected < ControlDesign::EC_NO_RESPONSE_MIN_EXPECTED_PPM) {
    return false;
  }
  const float floor = ControlDesign::EC_NO_RESPONSE_FRACTION * expected;
  if (delta >= floor) {
    state.noResponseCount = 0;
    state.noResponseMl = 0.0f;
    return false;
  }
  state.noResponseCount++;
  state.noResponseMl += state.lastDoseMl;
  if (!state.noResponseFault && state.noResponseCount >= ControlDesign::EC_NO_RESPONSE_DOSES) {
    state.noResponseFault = true;
    return true;
  }
  return false;
}

// Returns true when this recheck latched the no-response fault.
inline bool updateGainAfterRecheck(State& state, float ecAfter) {
  if (state.lastDoseMl <= 0.0f || isnan(state.ecBeforeLastDose) || isnan(ecAfter)) {
    return false;
  }
  // Nutrient only raises TDS; a smaller or negative change is noise or uptake.
  const float delta = ecAfter - state.ecBeforeLastDose;
  const bool tripped = recordResponse(state, delta);
  if (ControlDesign::ADAPTIVE_GAINS && delta >= ControlDesign::EC_MIN_DELTA_PPM &&
      ProportionalDose::updateProcessGain(state.gainEc, delta / state.lastDoseMl,
                                          ControlDesign::GAIN_ALPHA,
                                          ControlDesign::EC_GAIN_MIN,
                                          ControlDesign::EC_GAIN_MAX)) {
    state.gainConfirmed = true;
  }
  state.lastDoseMl = 0.0f;
  state.ecBeforeLastDose = NAN;
  return tripped;
}

}  // namespace EcControl

#endif  // SMART_GARDEN_EC_CONTROL_LOOP_H
