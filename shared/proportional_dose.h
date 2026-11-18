/************************************************************
 * Adaptive dead-band proportional dose helpers
 *
 * The gain is held as a process gain estimate K_hat (change in the
 * reading per mL dosed: ppm/mL or pH/mL), and the dose divides by it:
 *
 *   u = clamp(lambda * |e| / K_hat,  u_min,  min(u_cap, u_hard))
 *   u_cap = (distance to the middle of the dead range) / K_label
 *
 * u_cap is the volume the reagent label says the error needs (fixed assumed
 * volume), so a wrong K_hat cannot dose more than that; u_hard is an
 * absolute ceiling (label full-strength / one-pH-unit dose).
 *
 * The update uses the same units, so the estimate and the observed
 * response can be averaged directly:
 *
 *   K_hat <- (1 - alpha) K_hat + alpha |dy| / u_delivered
 ************************************************************/

#ifndef SMART_GARDEN_PROPORTIONAL_DOSE_H
#define SMART_GARDEN_PROPORTIONAL_DOSE_H

#include <Arduino.h>
#include <math.h>

namespace ProportionalDose {

inline bool inTargetBand(float value, float targetMin, float targetMax) {
  return value >= targetMin && value <= targetMax;
}

inline bool belowDeadLow(float value, float deadLow) {
  return value < deadLow;
}

inline bool aboveDeadHigh(float value, float deadHigh) {
  return value > deadHigh;
}

inline float clampMl(float ml, float minMl, float maxMl) {
  if (ml < minMl) {
    return minMl;
  }
  if (ml > maxMl) {
    return maxMl;
  }
  return ml;
}

// Label cap (mL): the volume the reagent label says would move the reading by
// distance, at the label gain kLabel (units per mL at the assumed volume).
inline float labelCapMl(float distance, float kLabel) {
  if (distance <= 0.0f || kLabel <= 0.0f) {
    return 0.0f;
  }
  return distance / kLabel;
}

// Dose (mL) for an error |e|: standard P law u = Kp * |e| with controller gain
// Kp = lambda / kHat (mL per unit of error), where kHat is the process gain
// model (units per mL). lambda = Kp * K when the model is exact (1 = deadbeat).
// capMl is the label cap; hardMaxMl the absolute per-dose ceiling. If the
// cap is below the minimum dose, the cap wins.
inline float doseForError(float absError, float kHat, float lambda,
                          float minMl, float capMl, float hardMaxMl) {
  if (absError <= 0.0f || kHat <= 0.0f || isnan(kHat)) {
    return 0.0f;
  }
  float maxMl = capMl < hardMaxMl ? capMl : hardMaxMl;
  if (maxMl <= 0.0f) {
    return 0.0f;
  }
  if (minMl > maxMl) {
    minMl = maxMl;
  }
  return clampMl(lambda * absError / kHat, minMl, maxMl);
}

// First-order update of the process gain estimate, bounded to [kMin, kMax].
// Returns true when the estimate was updated.
inline bool updateProcessGain(float& kHat, float observed, float alpha,
                              float kMin, float kMax) {
  if (observed <= 0.0f || isnan(observed)) {
    return false;
  }
  if (alpha < 0.0f) {
    alpha = 0.0f;
  }
  if (alpha > 1.0f) {
    alpha = 1.0f;
  }
  float next = (1.0f - alpha) * kHat + alpha * observed;
  if (next < kMin) {
    next = kMin;
  }
  if (next > kMax) {
    next = kMax;
  }
  kHat = next;
  return true;
}

}  // namespace ProportionalDose

#endif  // SMART_GARDEN_PROPORTIONAL_DOSE_H
