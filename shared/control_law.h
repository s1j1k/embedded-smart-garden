/************************************************************
 * Discrete dose control laws (threshold / P / PI / PID)
 *
 * These controllers run once per settle interval, not every loop.
 * The output is a requested dose volume (mL), which the sketch
 * converts to a pump run time using PumpCal rates.
 *
 * See CONTROL_REPORT.md for the prototype post-mortem, feed-forward
 * vs feedback, and other strategies (2DOF, hysteresis, Smith, MPC).
 *
 * TODO: ControlLaw::FF_P (feed-forward + P trim) for water volume
 * bookkeeping and nutrient dilution compensation once tank mL and
 * ppm-per-mL are measured. Do not add open-loop pH FF on Kratky.
 ************************************************************/

#ifndef SMART_GARDEN_CONTROL_LAW_H
#define SMART_GARDEN_CONTROL_LAW_H

#include <Arduino.h>
#include <math.h>

namespace ControlLaw {

enum Mode {
  THRESHOLD = 0,  // fixed pulse when outside the band
  P,              // dose_ml = Kp * |error|
  // Arduino.h #defines PI as 3.14159… — do not name an enumerator PI.
  PI_LAW,         // + integral of error over settle intervals
  PID_LAW         // + derivative of error
  // FF_P reserved — see CONTROL_REPORT.md §6 (not implemented yet)
};

struct State {
  float integral = 0.0f;
  float lastError = NAN;
};

struct Gains {
  float kp = 0.0f;
  float ki = 0.0f;
  float kd = 0.0f;
  float minMl = 0.15f;
  float maxMl = 2.0f;
};

inline const char* name(Mode mode) {
  switch (mode) {
    case P: return "P";
    case PI_LAW: return "PI";
    case PID_LAW: return "PID";
    default: return "threshold";
  }
}

inline void reset(State& state) {
  state.integral = 0.0f;
  state.lastError = NAN;
}

// error: signed process error (setpoint - measurement) in engineering units.
// dtSeconds: time since the last dose decision (the settle interval).
// Returns a non-negative dose volume in mL.
inline float doseMl(Mode mode, float error, float dtSeconds, State& state,
                    const Gains& gains) {
  const float absError = fabsf(error);
  if (mode == THRESHOLD || absError < 1e-6f) {
    return 0.0f;
  }

  if (dtSeconds < 1.0f) {
    dtSeconds = 1.0f;
  }

  float derivative = 0.0f;
  if (!isnan(state.lastError)) {
    derivative = (error - state.lastError) / dtSeconds;
  }

  float p = gains.kp * error;
  float i = 0.0f;
  float d = 0.0f;

  if (mode == PI_LAW || mode == PID_LAW) {
    i = gains.ki * state.integral;
  }
  if (mode == PID_LAW) {
    d = gains.kd * derivative;
  }

  float raw = p + i + d;
  float saturated = raw;
  if (saturated > gains.maxMl) {
    saturated = gains.maxMl;
  } else if (saturated < -gains.maxMl) {
    saturated = -gains.maxMl;
  }

  // Integrator anti-windup: only accumulate when the output is not hard-clamped
  // in the same direction as the error.
  if (mode == PI_LAW || mode == PID_LAW) {
    const bool clamped = fabsf(raw) > gains.maxMl + 1e-6f;
    if (!clamped) {
      state.integral += error * dtSeconds;
    }
  }

  state.lastError = error;

  float volume = fabsf(saturated);
  if (volume > 0.0f && volume < gains.minMl) {
    volume = gains.minMl;
  }
  return volume;
}

}  // namespace ControlLaw

#endif  // SMART_GARDEN_CONTROL_LAW_H
