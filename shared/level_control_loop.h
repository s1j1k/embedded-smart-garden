/************************************************************
 * Water level / scheduled drawdown control
 *
 * Pump only adds water. Maintains drawdown within tolerance of
 * the stage target — never refills back to fill_0.
 ************************************************************/

#ifndef SMART_GARDEN_LEVEL_CONTROL_LOOP_H
#define SMART_GARDEN_LEVEL_CONTROL_LOOP_H

#include <Arduino.h>
#include <math.h>
#include "tank_geometry.h"

namespace LevelControl {

struct CheckResult {
  bool topUpNeeded = false;
  float topUpMl = 0.0f;
  float currentDrawdownMm = 0.0f;
  int targetDepthMm = 0;
  int toleranceMm = 10;
  const char* reason = "";
};

inline float drawdownMm(int fill0Percent, int currentLevelPercent) {
  if (fill0Percent < 0 || currentLevelPercent < 0) {
    return NAN;
  }
  const int deltaPct = fill0Percent - currentLevelPercent;
  return static_cast<float>(deltaPct) * Tank::GROVE_MM_PER_PERCENT;
}

inline int levelPercentForDrawdownMm(int fill0Percent, float targetDrawdownMm) {
  const int offsetPct =
      static_cast<int>(targetDrawdownMm / Tank::GROVE_MM_PER_PERCENT + 0.5f);
  return Tank::clampPercent(fill0Percent - offsetPct);
}

inline CheckResult evaluateCheck(
    int fill0Percent,
    int currentLevelPercent,
    int targetDepthMm,
    int toleranceMm) {
  CheckResult result;
  result.targetDepthMm = targetDepthMm;
  result.toleranceMm = toleranceMm;

  if (fill0Percent < 0 || currentLevelPercent < 0) {
    result.reason = "level_unavailable";
    return result;
  }

  result.currentDrawdownMm = drawdownMm(fill0Percent, currentLevelPercent);
  if (isnan(result.currentDrawdownMm)) {
    result.reason = "drawdown_invalid";
    return result;
  }

  const float upper = static_cast<float>(targetDepthMm + toleranceMm);
  const float lower = static_cast<float>(targetDepthMm - toleranceMm);

  if (result.currentDrawdownMm > upper) {
    const float excessMm = result.currentDrawdownMm - static_cast<float>(targetDepthMm);
    result.topUpMl = excessMm * Tank::mlPerMm();
    if (result.topUpMl < 1.0f) {
      result.topUpMl = 1.0f;
    }
    result.topUpNeeded = true;
    result.reason = "drawdown_high";
    return result;
  }

  if (result.currentDrawdownMm < lower) {
    result.reason = "drawdown_low_wait";
    return result;
  }

  result.reason = "drawdown_ok";
  return result;
}

}  // namespace LevelControl

#endif  // SMART_GARDEN_LEVEL_CONTROL_LOOP_H
