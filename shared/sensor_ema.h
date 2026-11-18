/************************************************************
 * Control-input low-pass filter for pH and TDS
 *
 * 2nd-order Butterworth low-pass, cutoff FC_HZ = 2 mHz, run on the burst
 * readings (one sample per sensor cycle, nominally 10 s). It replaced the
 * 3 min exponential moving average (firmware <= 1.8) after the filter
 * analysis in docs/filter_analysis/ (report section "Sensor filtering"):
 *
 *                       settle to 95 %   pH noise   TDS noise
 *   EMA, tau = 3 min        8.8 min       0.0083     0.20 ppm
 *   Butterworth-2, 2 mHz    3.8 min       0.0098     0.26 ppm
 *
 * About the same smoothing with less than half the lag; the 4 % step
 * overshoot is over long before a control check reads the value.
 *
 * Form: bilinear transform with pre-warping, Direct Form II transposed, with
 * fixed coefficients for the nominal 10 s sensor interval. The coefficients
 * are deliberately not recomputed from the actual interval: the DF2T state is
 * only valid for the coefficients that produced it, and changing them between
 * samples injects a spurious transient (the host test caught a jump the wrong
 * way of 0.8 pH). A late sample is simply one step later.
 * The caller skips the update while a pump draws on the shared 5 V rail, so
 * a sagged sample is not folded in and the last value holds.
 *
 * Warm-up: after boot (or reset() on a sensor fault) the output stays NAN
 * until WARMUP_MS of consecutive valid samples, then the filter is seeded at
 * steady state on their mean. Control reads NAN as "no reading".
 *
 * The namespace and the phValueEma / tdsPpmEma / ph_ema / tds_ppm_ema names
 * are kept from the EMA so the journal, MQTT, Influx and dashboard fields do
 * not change; they now hold this filter's output.
 ************************************************************/

#ifndef SMART_GARDEN_SENSOR_EMA_H
#define SMART_GARDEN_SENSOR_EMA_H

#include <Arduino.h>
#include <math.h>

namespace SensorEma {

constexpr float FC_HZ = 0.002f;          // Butterworth cutoff
constexpr const char* FILTER_NAME = "butterworth2_2mHz";
constexpr unsigned long WARMUP_MS = 60000UL;
constexpr float NOMINAL_DT_S = 10.0f;    // SENSOR_READ_INTERVAL_MS

struct State {
  float y = NAN;
  float z1 = 0.0f;   // DF2T state
  float z2 = 0.0f;
  unsigned long lastMs = 0;
  bool seeded = false;
  bool warming = false;
  unsigned long warmStartMs = 0;
  float warmSum = 0.0f;
  int warmCount = 0;
};

inline void reset(State& state) {
  state = State{};
}

inline bool ready(const State& state) {
  return state.seeded && !isnan(state.y);
}

// Seconds of warm-up collected so far (0 before the first valid sample).
inline unsigned long warmupElapsedS(const State& state, unsigned long nowMs) {
  if (ready(state)) {
    return WARMUP_MS / 1000UL;
  }
  return state.warming ? (nowMs - state.warmStartMs) / 1000UL : 0UL;
}

struct Coeffs {
  float b0, b1, b2, a1, a2;
};

// 2nd-order Butterworth low-pass for sample interval dtS.
inline Coeffs design(float dtS) {
  const float k = tanf(static_cast<float>(M_PI) * FC_HZ * dtS);
  const float q = 1.41421356f;  // 1 / Q, Q = 1/sqrt(2)
  const float norm = 1.0f / (1.0f + q * k + k * k);
  Coeffs c;
  c.b0 = k * k * norm;
  c.b1 = 2.0f * c.b0;
  c.b2 = c.b0;
  c.a1 = 2.0f * (k * k - 1.0f) * norm;
  c.a2 = (1.0f - q * k + k * k) * norm;
  return c;
}

inline const Coeffs& coeffs() {
  static const Coeffs c = design(NOMINAL_DT_S);
  return c;
}

// Set the DF2T state so the filter sits at steady state on value y0.
inline void seedSteadyState(State& state, float y0, const Coeffs& c) {
  state.y = y0;
  state.z1 = y0 * (1.0f - c.b0);
  state.z2 = y0 * (c.b2 - c.a2);
}

inline float update(State& state, float sample, unsigned long nowMs) {
  if (isnan(sample)) {
    return state.y;
  }
  if (!state.seeded || isnan(state.y)) {
    if (!state.warming) {
      state.warming = true;
      state.warmStartMs = nowMs;
      state.warmSum = 0.0f;
      state.warmCount = 0;
    }
    state.warmSum += sample;
    state.warmCount++;
    if (nowMs - state.warmStartMs < WARMUP_MS) {
      return NAN;
    }
    seedSteadyState(state, state.warmSum / static_cast<float>(state.warmCount), coeffs());
    state.lastMs = nowMs;
    state.seeded = true;
    state.warming = false;
    return state.y;
  }

  if (nowMs == state.lastMs) {
    return state.y;
  }
  state.lastMs = nowMs;
  const Coeffs& c = coeffs();
  // Direct Form II transposed.
  const float y = c.b0 * sample + state.z1;
  state.z1 = c.b1 * sample - c.a1 * y + state.z2;
  state.z2 = c.b2 * sample - c.a2 * y;
  state.y = y;
  return state.y;
}

inline float forControl(float filtered, float burst) {
  return isnan(filtered) ? burst : filtered;
}

}  // namespace SensorEma

#endif  // SMART_GARDEN_SENSOR_EMA_H
