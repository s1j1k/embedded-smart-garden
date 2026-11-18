/************************************************************
 * Solution monitor — when is the nutrient solution due for a change?
 *
 * A recirculating solution does not wear out on a calendar; it drifts.
 * Plants take up ions selectively (N, P, K fast; Ca, Mg, S, Na, Cl slowly),
 * so EC-based dosing that tops the total back up lets the slow ions build
 * up while the fast ones run short, even at a constant EC [Vought 2024;
 * Langenfeld & Bugbee 2026]. Every pH correction adds counter-ions
 * (potassium, phosphate) and buffer, so each later correction needs more
 * reagent [Dickson 2016]. The usual fix is a fixed renewal interval
 * [Ko 2013; Savvas & Gruda 2018]. This monitor instead watches the
 * symptoms the controller can measure, one indicator per mechanism:
 *
 *   age        days since the last change (fallback calendar, 3-4 weeks)
 *   turnover   top-up water since the change, in tank volumes: ions that
 *              come in with the water or are not taken up concentrate
 *   reagent    pH reagent that reached the tank, mL per L: salt and buffer
 *              added that plants do not take up in proportion
 *   buffer     how far each pH dose fell short of what the tuned gain
 *              predicts (observed / predicted change, filtered)
 *   ph_band    hours in the last 24 that pH sat outside its dead limits,
 *              by auto or hand dosing: the solution no longer holds pH
 *   tds_high   hours TDS has stayed above its dead-high limit: the
 *              firmware never dilutes, so ions are arriving faster than
 *              they are taken up
 *
 * Each indicator is OK, WATCH or DUE. The solution is DUE when any is DUE,
 * else WATCH when any is WATCH. Nothing here doses or blocks dosing: a
 * water change is a manual job, and the monitor says when and why.
 * Thresholds are provisional, set from the label chemistry and the
 * literature above, not yet validated on this tank.
 *
 * Pure logic (host-tested); persistence lives in solution.cpp.
 ************************************************************/

#ifndef SMART_GARDEN_SOLUTION_MONITOR_H
#define SMART_GARDEN_SOLUTION_MONITOR_H

#include <Arduino.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace SolutionMonitor {

// Thresholds: WATCH, then DUE.
constexpr float AGE_WATCH_D = 21.0f;      // Ko et al. 2013: 4-week renewal kept ion balance
constexpr float AGE_DUE_D = 28.0f;
constexpr float TURNOVER_WATCH = 1.0f;    // top-up volume / tank volume
constexpr float TURNOVER_DUE = 2.0f;
// Manutec label: 0.1 mL/L per pH unit, so 1.5 mL/L is ~15 pH units of correction.
constexpr float REAGENT_WATCH_ML_PER_L = 0.75f;
constexpr float REAGENT_DUE_ML_PER_L = 1.5f;
constexpr float BUFFER_WATCH = 0.6f;      // doses reach < 60 % of the predicted change
constexpr float BUFFER_DUE = 0.35f;
constexpr int BUFFER_MIN_SAMPLES = 3;
constexpr float BUFFER_ALPHA = 0.3f;      // filter on the observed / predicted ratio
constexpr int PH_OUT_WATCH_H = 6;         // of the last 24 h
constexpr int PH_OUT_DUE_H = 12;
constexpr float TDS_HIGH_WATCH_H = 12.0f;
constexpr float TDS_HIGH_DUE_H = 24.0f;
// A minute counts as out of band only past this margin outside the dead limits.
constexpr float PH_OUT_MARGIN = 0.1f;

enum Level : uint8_t { OK = 0, WATCH = 1, DUE = 2 };

enum Reason : uint8_t {
  AGE = 1 << 0,
  TURNOVER = 1 << 1,
  REAGENT = 1 << 2,
  BUFFER = 1 << 3,
  PH_BAND = 1 << 4,
  TDS_HIGH = 1 << 5,
};

inline const char* reasonName(uint8_t bit) {
  switch (bit) {
    case AGE: return "age";
    case TURNOVER: return "turnover";
    case REAGENT: return "reagent";
    case BUFFER: return "buffer";
    case PH_BAND: return "ph_band";
    case TDS_HIGH: return "tds_high";
    default: return "";
  }
}

// Saved across reboots (solution.cpp), cleared by a water change.
struct Saved {
  uint32_t startEpoch = 0;      // UTC seconds at the change; 0 until the clock is set
  float topUpMl = 0.0f;         // automatic top-up water delivered
  float reagentMl = 0.0f;       // pH reagent from doses that moved pH
  float bufferRatio = NAN;      // filtered observed / predicted pH change
  int bufferN = 0;
};

// RAM only: 24 h windows rebuild after a reboot.
struct Live {
  uint8_t outMin[24] = {0};     // minutes out of band, per hour slot
  uint32_t slotHour = 0;        // hour (uptime) of the current slot
  bool slotStarted = false;
  uint32_t lastSampleMin = 0;
  bool sampled = false;
  unsigned long tdsHighSinceMs = 0;
  bool tdsHigh = false;
};

struct Inputs {
  float ageDays = NAN;          // NAN when the change time is unknown
  float turnover = 0.0f;        // top-up / tank volume
  float reagentMlPerL = 0.0f;
  float bufferRatio = NAN;
  int bufferN = 0;
  int phOutHours = 0;
  float tdsHighHours = 0.0f;
};

struct Result {
  Level level = OK;
  uint8_t due = 0;              // Reason bits at DUE
  uint8_t watch = 0;            // Reason bits at WATCH (not DUE)
};

inline void grade(Result& r, uint8_t bit, bool watch, bool due) {
  if (due) {
    r.due |= bit;
  } else if (watch) {
    r.watch |= bit;
  }
}

inline Result evaluate(const Inputs& in) {
  Result r;
  if (!isnan(in.ageDays)) {
    grade(r, AGE, in.ageDays >= AGE_WATCH_D, in.ageDays >= AGE_DUE_D);
  }
  grade(r, TURNOVER, in.turnover >= TURNOVER_WATCH, in.turnover >= TURNOVER_DUE);
  grade(r, REAGENT, in.reagentMlPerL >= REAGENT_WATCH_ML_PER_L,
        in.reagentMlPerL >= REAGENT_DUE_ML_PER_L);
  if (in.bufferN >= BUFFER_MIN_SAMPLES && !isnan(in.bufferRatio)) {
    grade(r, BUFFER, in.bufferRatio <= BUFFER_WATCH, in.bufferRatio <= BUFFER_DUE);
  }
  grade(r, PH_BAND, in.phOutHours >= PH_OUT_WATCH_H, in.phOutHours >= PH_OUT_DUE_H);
  grade(r, TDS_HIGH, in.tdsHighHours >= TDS_HIGH_WATCH_H, in.tdsHighHours >= TDS_HIGH_DUE_H);
  r.level = r.due ? DUE : r.watch ? WATCH : OK;
  return r;
}

inline const char* levelName(Level l) {
  return l == DUE ? "due" : l == WATCH ? "watch" : "ok";
}

// Comma-separated reason names for a bit mask, e.g. "buffer,ph_band".
inline void formatReasons(uint8_t mask, char* out, size_t n) {
  if (n == 0) return;
  out[0] = '\0';
  size_t used = 0;
  for (uint8_t bit = 1; bit != 0 && bit <= TDS_HIGH; bit <<= 1) {
    if (!(mask & bit)) continue;
    const int w = snprintf(out + used, n - used, "%s%s", used ? "," : "", reasonName(bit));
    if (w < 0 || static_cast<size_t>(w) >= n - used) break;
    used += static_cast<size_t>(w);
  }
}

// A settled pH dose. ratio = observed / predicted change, NAN when the dose did
// not move pH (a delivery fault, handled by the no-response fault, not here).
inline void recordPhResponse(Saved& s, float ratio, float deliveredMl) {
  if (isnan(ratio) || isnan(deliveredMl) || deliveredMl <= 0.0f) {
    return;
  }
  s.reagentMl += deliveredMl;
  const float clamped = ratio < 0.0f ? 0.0f : ratio > 2.0f ? 2.0f : ratio;
  s.bufferRatio = (s.bufferN == 0 || isnan(s.bufferRatio))
                      ? clamped
                      : (1.0f - BUFFER_ALPHA) * s.bufferRatio + BUFFER_ALPHA * clamped;
  s.bufferN++;
}

// Once a minute: is pH outside its dead limits (plus the margin)?
inline void samplePh(Live& l, unsigned long nowMs, float ph, float deadLow, float deadHigh) {
  const uint32_t minute = nowMs / 60000UL;
  if (l.sampled && minute == l.lastSampleMin) {
    return;
  }
  l.sampled = true;
  l.lastSampleMin = minute;
  const uint32_t hour = minute / 60UL;
  if (!l.slotStarted) {
    l.slotStarted = true;
    l.slotHour = hour;
  }
  // Clear the slots for any hours that passed since the last sample.
  while (l.slotHour < hour) {
    l.slotHour++;
    l.outMin[l.slotHour % 24] = 0;
  }
  if (isnan(ph)) {
    return;
  }
  if (ph < deadLow - PH_OUT_MARGIN || ph > deadHigh + PH_OUT_MARGIN) {
    uint8_t& m = l.outMin[hour % 24];
    if (m < 60) m++;
  }
}

// Hours (rounded) out of band in the last 24.
inline int phOutHours(const Live& l) {
  int minutes = 0;
  for (int i = 0; i < 24; i++) minutes += l.outMin[i];
  return (minutes + 30) / 60;
}

inline void sampleTds(Live& l, unsigned long nowMs, float tds, float deadHigh) {
  const bool high = !isnan(tds) && !isnan(deadHigh) && deadHigh > 0.0f && tds > deadHigh;
  if (high && !l.tdsHigh) {
    l.tdsHighSinceMs = nowMs;
  }
  l.tdsHigh = high;
}

inline float tdsHighHours(const Live& l, unsigned long nowMs) {
  return l.tdsHigh ? (nowMs - l.tdsHighSinceMs) / 3600000.0f : 0.0f;
}

}  // namespace SolutionMonitor

#endif  // SMART_GARDEN_SOLUTION_MONITOR_H
