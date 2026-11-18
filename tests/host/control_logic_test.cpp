#include <algorithm>
// Host tests for the controller logic in shared/ (no ESP32 needed).
//
// Build and run (any C++17 compiler):
//   g++ -std=c++17 -I tests/host -I shared tests/host/control_logic_test.cpp -o control_logic_test
//   ./control_logic_test
//
// Covers the report's dose law (Eq. controllaw), gain update (Eq. gainupdate),
// dead limits from the dashboard, and the control-input Butterworth filter.
#include <cassert>
#include <cstdio>
#include <string>

#include "Arduino.h"
#include "control_design.h"
#include "ec_control_loop.h"
#include "grow_stage.h"
#define ENABLE_GROW_LIGHT 0
#include "grow_light.h"
#include "ph_control_loop.h"
#include "proportional_dose.h"
#include "sensor_ema.h"
#include "solution_monitor.h"
#include "level_smoother.h"

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                       \
    }                                                                   \
  } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

static void test_dose_law() {
  // u = clamp(lambda |e| / K_hat, umin, min(u_cap, uhard))
  NEAR(ProportionalDose::doseForError(686.0f, 14.0f, 0.8f, 0.1f, 50.0f, 50.0f), 39.2f, 1e-3f);
  // label cap wins
  NEAR(ProportionalDose::doseForError(686.0f, 14.0f, 0.8f, 0.1f, 26.0f, 32.5f), 26.0f, 1e-4f);
  // hard max wins when both the estimate and the cap are loose
  NEAR(ProportionalDose::doseForError(686.0f, 3.0f, 0.8f, 0.1f, 100.0f, 32.5f), 32.5f, 1e-4f);
  // minimum dose
  NEAR(ProportionalDose::doseForError(0.01f, 1.5f, 0.8f, 0.05f, 1.0f, 1.0f), 0.05f, 1e-6f);
  CHECK(ProportionalDose::doseForError(0.0f, 1.5f, 0.8f, 0.05f, 1.0f, 1.0f) == 0.0f);
  // label cap: distance / label gain
  NEAR(ProportionalDose::labelCapMl(686.0f, 26.0f), 26.3846f, 1e-3f);
  CHECK(ProportionalDose::labelCapMl(-1.0f, 26.0f) == 0.0f);
}

static void test_gain_update() {
  float k = 14.0f;
  CHECK(ProportionalDose::updateProcessGain(k, 18.0f, 0.5f, 3.0f, 60.0f));
  NEAR(k, 16.0f, 1e-5f);
  CHECK(ProportionalDose::updateProcessGain(k, 1000.0f, 0.5f, 3.0f, 60.0f));
  NEAR(k, 60.0f, 1e-5f);  // bounded
  CHECK(!ProportionalDose::updateProcessGain(k, -1.0f, 0.5f, 3.0f, 60.0f));
}

static void test_ec_loop() {
  const GrowStage::Config& cfg = GrowStage::config(GrowStage::VEGETATIVE);
  // Tuned nutrient gain scales with 1/V: 31.2 ppm/mL at 8.4 L (262 ppm L/mL).
  NEAR(ControlDesign::ecCapGain(8.4f), 31.2f, 1e-4f);
  const float kCap = ControlDesign::ecCapGain(ControlDesign::V_ASSUMED_L);
  NEAR(ControlDesign::capVolumeL(NAN), ControlDesign::V_ASSUMED_L, 1e-6f);
  NEAR(ControlDesign::capVolumeL(10.0f), 7.0f, 1e-5f);  // a high level reading cannot enlarge a dose
  NEAR(ControlDesign::capVolumeL(7.0f), 6.3f, 1e-5f);
  // Plain water, no level reading: 0.8 of the error to 700 at the tuned gain.
  EcControl::State s;
  EcControl::initGain(s, cfg);
  EcControl::CheckResult r = EcControl::evaluateCheck(14.0f, cfg, s);
  CHECK(!r.noAction);
  NEAR(r.doseMl, 0.8f * 686.0f / kCap, 1e-3f);
  NEAR(s.gainEc, kCap, 1e-4f);
  {
    // Level reads 9.3 L: clamped to 7.0 L.
    EcControl::State sv;
    EcControl::initGain(sv, cfg);
    NEAR(EcControl::evaluateCheck(14.0f, cfg, sv, ControlDesign::capVolumeL(8.4f / 0.9f)).doseMl,
         0.8f * 686.0f / ControlDesign::ecCapGain(7.0f), 1e-3f);
  }
  {
    // Near the band (520): lambda e / K (0.8 x 180) is below the cap to 800.
    EcControl::State sn;
    EcControl::initGain(sn, cfg);
    NEAR(EcControl::evaluateCheck(520.0f, cfg, sn).doseMl, 0.8f * 180.0f / kCap, 1e-3f);
  }
  // Fixed gains: a recheck logs the response but does not change the gain.
  EcControl::recordDoseStart(s, 14.0f);
  EcControl::recordDelivered(s, r.doseMl);
  EcControl::updateGainAfterRecheck(s, 14.0f + 12.0f * r.doseMl);
  NEAR(s.gainEc, kCap, 1e-4f);
  // Inside dead limits: no dose. Above: no dose (no dilution path).
  CHECK(EcControl::evaluateCheck(560.0f, cfg, s).noAction);
  CHECK(EcControl::evaluateCheck(900.0f, cfg, s).noAction);
}

static void test_ec_slow_mixing_no_overshoot() {
  const GrowStage::Config& cfg = GrowStage::config(GrowStage::VEGETATIVE);
  const float V = 7.84f;
  const float capV = ControlDesign::capVolumeL(V);
  for (float gamma : {150.0f, 180.0f, 230.0f, 260.0f, 289.0f}) {  // ppm L / mL
    for (float unseen : {0.0f, 30.0f, 60.0f}) {
      EcControl::State s;
      EcControl::initGain(s, cfg);
      const float base = 9.0f;
      float delivered = 0.0f;
      float seenPpm = base;
      for (int i = 0; i < 12; i++) {
        EcControl::CheckResult r = EcControl::evaluateCheck(seenPpm, cfg, s, capV);
        if (r.noAction || r.doseMl <= 0.0f) {
          break;
        }
        EcControl::recordDoseStart(s, seenPpm);
        EcControl::recordDelivered(s, r.doseMl);
        const float before = base + gamma / V * delivered;  // earlier doses mixed
        delivered += r.doseMl;
        const float finalPpm = base + gamma / V * delivered;
        const float rise = finalPpm - before;
        seenPpm = finalPpm - (rise < unseen ? rise : unseen);  // latest dose part-mixed
        EcControl::updateGainAfterRecheck(s, seenPpm);  // next dose planned from this too
      }
      const float finalPpm = base + gamma / V * delivered;
      CHECK(finalPpm < 850.0f);
      CHECK(finalPpm >= 550.0f);
    }
  }
}

// Fixed pH gains from the largest responses measured (pH-up 14.8, pH-down 12.1 pH L/mL,
// docs/dose_response/), independent of TDS. The dose limit is the bottle rate's full
// correction to 6.0; lambda 0.8 aims 20 % short of it.
static void test_ph_tuned() {
  const GrowStage::Config& cfg = GrowStage::config(GrowStage::VEGETATIVE);
  const float v = ControlDesign::capVolumeL(7.84f);  // 7.0 L (clamped)
  NEAR(v, 7.0f, 1e-5f);
  const float label = ControlDesign::phLabelGain(v);
  NEAR(label, 100.0f / 70.0f, 1e-5f);
  // Gains are the largest responses measured (pH L/mL over V), whatever the TDS.
  const float kUp = 14.8f / 7.0f, kDown = 12.1f / 7.0f;
  NEAR(ControlDesign::phTunedGain(v, true, 700.0f), kUp, 1e-5f);
  NEAR(ControlDesign::phTunedGain(v, true, 100.0f), kUp, 1e-5f);
  NEAR(ControlDesign::phTunedGain(v, true, NAN), kUp, 1e-5f);
  NEAR(ControlDesign::phTunedGain(v, false, 700.0f), kDown, 1e-5f);

  PhControl::State s;
  PhControl::initGains(s, cfg);
  // Dead band: no dose inside 5.5-6.5.
  CHECK(PhControl::evaluateCheck(5.6f, cfg, s, false, 0, v, 700.0f).noAction);
  CHECK(PhControl::evaluateCheck(6.4f, cfg, s, false, 0, v, 700.0f).noAction);
  // pH 5.0: 0.8 x 1.0 / 2.04 = 0.39 mL (bottle-rate limit 1.0 / 1.43 = 0.70).
  PhControl::CheckResult r = PhControl::evaluateCheck(5.0f, cfg, s, false, 0, v, 700.0f);
  CHECK(r.direction == PhControl::UP);
  NEAR(r.doseMl, 0.8f / kUp, 1e-4f);
  NEAR(s.gainUp, kUp, 1e-5f);
  // The TDS no longer changes the dose.
  NEAR(PhControl::evaluateCheck(5.0f, cfg, s, false, 0, v, 100.0f).doseMl, 0.8f / kUp, 1e-4f);
  // pH-down from 7.0: 0.8 / 1.8 = 0.44 mL.
  r = PhControl::evaluateCheck(7.0f, cfg, s, false, 0, v, 700.0f);
  CHECK(r.direction == PhControl::DOWN);
  NEAR(r.doseMl, 0.8f / kDown, 1e-4f);
  // Far off (pH 4.0): 1.6 / 2.04 = 0.78 mL, inside the 1.40 mL bottle-rate limit.
  NEAR(PhControl::evaluateCheck(4.0f, cfg, s, false, 0, v, 700.0f).doseMl, 1.6f / kUp, 1e-4f);
  // 14 Oct 13:14: pH 4.36 at 636 ppm in 6.5 L. The 1.9.8 gain (0.40 pH/mL) asked
  // 3.3 mL (sent 3.0, the guard) and the tank's 1.25 pH/mL took pH to 8.1. Now:
  {
    PhControl::State fresh;
    PhControl::initGains(fresh, cfg);
    const float dose = PhControl::evaluateCheck(4.36f, cfg, fresh, false, 0, 6.5f, 636.0f).doseMl;
    NEAR(dose, 0.8f * (6.0f - 4.36f) / (14.8f / 6.5f), 1e-3f);  // 0.60 mL
    const float landed = 4.36f + dose * 1.25f;                    // ~5.1: short, no overshoot
    CHECK(landed > 4.36f && landed < cfg.phCenter);
    // Repeat the check on that tank (1.25 pH/mL): each dose lands short, none passes
    // the setpoint, and it is inside the dead limits within three doses.
    float ph = landed;
    int doses = 1;
    while (ph < cfg.phDeadLow && doses < 6) {
      PhControl::State again;
      PhControl::initGains(again, cfg);
      ph += PhControl::evaluateCheck(ph, cfg, again, false, 0, 6.5f, 636.0f).doseMl * 1.25f;
      doses++;
      CHECK(ph < cfg.phCenter);
    }
    CHECK(ph >= cfg.phDeadLow && doses <= 3);
    // In the 7 Oct buffered tank (~0.05 pH/mL) the same dose lands short: no overshoot.
    CHECK(4.36f + dose * 0.05f < cfg.phCenter);
  }
  // Fixed gains: a recheck does not change them.
  PhControl::recordDoseStart(s, PhControl::UP, 5.0f);
  PhControl::recordDelivered(s, 2.2f);
  PhControl::updateGainAfterRecheck(s, 5.0f);
  NEAR(s.gainUp, kUp, 1e-5f);
  // Session cap limits the planned dose.
  PhControl::State capped;
  PhControl::initGains(capped, cfg);
  capped.cumulativeSessionMl = cfg.phSessionCapMl - 0.1f;
  NEAR(PhControl::evaluateCheck(4.5f, cfg, capped, false, 0, v, 700.0f).doseMl, 0.1f, 1e-4f);
}

// One settled pH dose: plan at phBefore, deliver ml, recheck at phAfter.
static PhControl::Direction ph_dose(PhControl::State& s, PhControl::Direction dir,
                                     float phBefore, float ml, float phAfter) {
  PhControl::recordDoseStart(s, dir, phBefore);
  PhControl::recordDelivered(s, ml);
  return PhControl::updateGainAfterRecheck(s, phAfter);
}

static void test_ph_no_response() {
  const GrowStage::Config& cfg = GrowStage::config(GrowStage::VEGETATIVE);
  const float v = ControlDesign::V_ASSUMED_L;
  PhControl::State s;
  PhControl::initGains(s, cfg);
  CHECK(PhControl::evaluateCheck(6.9f, cfg, s, false, 0, v, 640.0f).direction == PhControl::DOWN);

  // 11-12 Oct: ~0.45 mL pH-down doses, pH creeping up instead of falling.
  CHECK(ph_dose(s, PhControl::DOWN, 6.51f, 0.45f, 6.53f) == PhControl::NONE);
  CHECK(ph_dose(s, PhControl::DOWN, 6.53f, 0.45f, 6.52f) == PhControl::NONE);
  CHECK(s.noResponseDown == 2);
  CHECK(ph_dose(s, PhControl::DOWN, 6.52f, 0.45f, 6.57f) == PhControl::DOWN);  // third: latched
  CHECK(s.noResponseFaultDown);
  NEAR(s.noResponseMlDown, 1.35f, 1e-4f);
  CHECK(!s.noResponseFaultUp);

  // Latched: no more pH-down, but pH-up still doses.
  PhControl::CheckResult r = PhControl::evaluateCheck(6.9f, cfg, s, false, 0, v, 640.0f);
  CHECK(r.noAction && r.noResponseHit && r.direction == PhControl::NONE);
  CHECK(std::string(r.reason) == "ph_no_response");
  CHECK(PhControl::evaluateCheck(5.0f, cfg, s, false, 0, v, 640.0f).direction == PhControl::UP);
  // A stage change re-seeds the gains but keeps the fault.
  PhControl::initGains(s, cfg);
  PhControl::resetSession(s);
  CHECK(s.noResponseFaultDown);
  // Operator clears it.
  PhControl::clearNoResponse(s, PhControl::NONE);
  CHECK(!s.noResponseFaultDown && s.noResponseDown == 0);
  CHECK(PhControl::evaluateCheck(6.9f, cfg, s, false, 0, v, 640.0f).direction == PhControl::DOWN);

  // A dose that works resets the streak.
  PhControl::State ok;
  PhControl::initGains(ok, cfg);
  ph_dose(ok, PhControl::DOWN, 7.0f, 0.45f, 7.0f);
  ph_dose(ok, PhControl::DOWN, 7.0f, 0.45f, 7.0f);
  ph_dose(ok, PhControl::DOWN, 7.0f, 0.45f, 6.6f);
  CHECK(ok.noResponseDown == 0 && !ok.noResponseFaultDown);
  // A dose too small to show (expected < 0.15 pH) neither counts nor resets.
  ph_dose(ok, PhControl::DOWN, 7.0f, 0.45f, 7.0f);
  ph_dose(ok, PhControl::DOWN, 7.0f, 0.02f, 7.0f);
  CHECK(ok.noResponseDown == 1);
}

// One settled nutrient dose planned at before, delivered ml, rechecked at after.
static bool ec_dose(EcControl::State& s, float before, float ml, float after) {
  EcControl::recordDoseStart(s, before);
  EcControl::recordDelivered(s, ml);
  return EcControl::updateGainAfterRecheck(s, after);
}

static void test_ec_no_response() {
  const GrowStage::Config& cfg = GrowStage::config(GrowStage::VEGETATIVE);
  const float v = ControlDesign::V_ASSUMED_L;
  EcControl::State s;
  EcControl::initGain(s, cfg);
  CHECK(!EcControl::evaluateCheck(66.0f, cfg, s, v).noAction);   // sets the tuned gain
  // 14 Oct: one partial dose, then doses that moved TDS by a few ppm.
  CHECK(!ec_dose(s, 66.1f, 12.66f, 134.8f));   // 14 % of predicted: counts
  CHECK(!ec_dose(s, 134.8f, 11.49f, 138.2f));
  CHECK(s.noResponseCount == 2);
  CHECK(ec_dose(s, 138.2f, 11.42f, 139.1f));   // third: latched
  CHECK(s.noResponseFault);
  NEAR(s.noResponseMl, 35.57f, 0.01f);
  EcControl::CheckResult r = EcControl::evaluateCheck(139.0f, cfg, s, v);
  CHECK(r.noAction && r.noResponseHit && std::string(r.reason) == "ec_no_response");
  // A session reset alone keeps it; the operator clear lifts it.
  EcControl::resetSession(s);
  CHECK(s.noResponseFault);
  EcControl::clearNoResponse(s);
  CHECK(!EcControl::evaluateCheck(139.0f, cfg, s, v).noAction);

  // Working line (14 Oct after the re-prime): 11 mL raised TDS 147 -> 455.
  EcControl::State ok;
  EcControl::initGain(ok, cfg);
  EcControl::evaluateCheck(147.0f, cfg, ok, v);
  CHECK(!ec_dose(ok, 147.0f, 11.05f, 455.6f));
  CHECK(!ec_dose(ok, 455.6f, 5.09f, 634.2f));
  CHECK(ok.noResponseCount == 0 && !ok.noResponseFault);
}

static void test_solution_monitor() {
  using namespace SolutionMonitor;
  Inputs fresh;
  fresh.ageDays = 2.0f;
  CHECK(evaluate(fresh).level == OK);

  // Each indicator on its own.
  Inputs in = fresh;
  in.ageDays = 22.0f;
  CHECK(evaluate(in).level == WATCH && evaluate(in).watch == AGE);
  in.ageDays = 29.0f;
  CHECK(evaluate(in).level == DUE && evaluate(in).due == AGE);
  in = fresh;
  in.turnover = 2.1f;
  CHECK(evaluate(in).due == TURNOVER);
  in = fresh;
  in.reagentMlPerL = 0.8f;
  CHECK(evaluate(in).watch == REAGENT);
  in = fresh;
  in.tdsHighHours = 30.0f;
  CHECK(evaluate(in).due == TDS_HIGH);

  // Buffer: needs three answered doses before it counts.
  Saved sv;
  recordPhResponse(sv, 0.3f, 0.5f);
  recordPhResponse(sv, NAN, 0.5f);     // dose that did not move pH: not buffer evidence
  recordPhResponse(sv, 0.3f, 0.5f);
  NEAR(sv.reagentMl, 1.0f, 1e-5f);
  in = fresh;
  in.bufferRatio = sv.bufferRatio;
  in.bufferN = sv.bufferN;
  CHECK(evaluate(in).level == OK);     // n = 2
  recordPhResponse(sv, 0.3f, 0.5f);
  in.bufferN = sv.bufferN;
  in.bufferRatio = sv.bufferRatio;
  CHECK(evaluate(in).due == BUFFER);

  // 12-13 Oct: hand doses take pH to ~6.4, it rebounds to ~8.2 within hours.
  Live l;
  unsigned long t = 0;
  for (int h = 0; h < 24; h++) {
    for (int m = 0; m < 60; m++, t += 60000UL) {
      const float ph = (h % 4 == 0) ? 6.4f : 8.2f;   // in band one hour in four
      samplePh(l, t, ph, 5.5f, 6.5f);
    }
  }
  CHECK(phOutHours(l) == 18);
  in = fresh;
  in.phOutHours = phOutHours(l);
  Result r = evaluate(in);
  CHECK(r.level == DUE && r.due == PH_BAND);
  // Old hours drop out of the 24 h window.
  for (int m = 0; m < 24 * 60; m++, t += 60000UL) samplePh(l, t, 6.0f, 5.5f, 6.5f);
  CHECK(phOutHours(l) == 0);

  char buf[64];
  formatReasons(BUFFER | PH_BAND, buf, sizeof(buf));
  CHECK(std::string(buf) == "buffer,ph_band");
}

static void test_dashboard_bands() {
  // Basil + lettuce: optimum overlap is the single point 6.0.
  CHECK(GrowStage::applyChemistryBands(GrowStage::VEGETATIVE, 600, 800, 6.0f, 6.0f, 550, 850,
                                       5.5f, 6.5f, 6.0f));
  const GrowStage::Config& c = GrowStage::config(GrowStage::VEGETATIVE);
  NEAR(c.phDeadLow, 5.5f, 1e-6f);
  NEAR(c.phDeadHigh, 6.5f, 1e-6f);
  NEAR(c.phTargetMin, 5.8f, 1e-6f);  // zero-width target widened to +/- 0.2
  NEAR(c.phTargetMax, 6.2f, 1e-6f);
  // Old dashboards send only targets: setpoint +/- 0.5 fallback.
  CHECK(GrowStage::applyChemistryBands(GrowStage::VEGETATIVE, 600, 800, 6.0f, 6.0f));
  NEAR(GrowStage::config(GrowStage::VEGETATIVE).phDeadLow, 5.5f, 1e-6f);
  // Dead limits that exclude the setpoint are refused.
  CHECK(!GrowStage::applyChemistryBands(GrowStage::VEGETATIVE, 600, 800, 6.0f, 6.0f, 550, 850,
                                        6.1f, 6.5f, 6.0f));
  GrowStage::resetConfig(GrowStage::VEGETATIVE);
}

static void test_filter() {
  // Coefficients for dt = 10 s, fc = 2 mHz must match scipy.signal.butter(2, 0.002, fs=0.1):
  // b = [0.0036217, 0.0072434, 0.0036217], a = [1, -1.8226949, 0.8371817]
  const SensorEma::Coeffs c = SensorEma::design(10.0f);
  NEAR(c.b0, 0.0036217f, 2e-6f);
  NEAR(c.a1, -1.8226949f, 2e-6f);
  NEAR(c.a2, 0.8371817f, 2e-6f);
  // Warm-up then steady state; unit step settles within 5 % by ~3.8 min.
  SensorEma::State s;
  unsigned long t = 0;
  float y = NAN;
  for (int i = 0; i <= 6; i++, t += 10000) {
    y = SensorEma::update(s, 6.0f, t);
  }
  CHECK(SensorEma::ready(s));
  NEAR(y, 6.0f, 1e-5f);
  float settle = -1.0f, peak = 0.0f;
  for (int i = 1; i <= 120; i++) {
    t += 10000;
    y = SensorEma::update(s, 7.0f, t);
    peak = std::max(peak, y);
    if (std::fabs(y - 7.0f) > 0.05f) {
      settle = i * 10.0f / 60.0f;
    }
  }
  CHECK(settle > 3.0f && settle < 4.5f);
  CHECK(peak < 7.06f);  // ~4.3 % overshoot
  // A late sample (30 s gap, e.g. a cycle held for a pump) at a steady value
  // must not disturb the output.
  t += 30000;
  NEAR(SensorEma::update(s, 7.0f, t), 7.0f, 1e-3f);
  std::printf("filter: settles within 5 %% at %.2f min, peak %.3f\n", settle, peak);
}

static void test_light_makeup() {
  using namespace GrowLight;
  const long long day = 1790000000LL - 1790000000LL % DAY_S;  // a UTC midnight
  const long long on = day + 22 * 3600LL;                      // ON 22:00 -> 14:00 UTC
  // Off for the whole dark period: nothing missed.
  CHECK(missedOnSeconds(on - 8 * 3600LL, on) == 0);
  // Off 06:00 -> 09:00 UTC, inside the ON window: 3 h missed.
  CHECK(missedOnSeconds(day + 6 * 3600LL, day + 9 * 3600LL) == 3 * 3600L);
  // Off 12:00 -> 23:00 UTC: 2 h before the OFF, 1 h after the next ON.
  CHECK(missedOnSeconds(day + 12 * 3600LL, day + 23 * 3600LL) == 3 * 3600L);
  // A whole day off misses one ON window; backwards spans miss nothing.
  CHECK(missedOnSeconds(on, on + DAY_S) == ON_S);
  CHECK(missedOnSeconds(on + 10, on) == 0);

  // Make-up runs only in the slot right after the scheduled OFF.
  CHECK(MAKEUP_MAX_PER_NIGHT_S == 2 * 3600L);
  Photoperiod p = withMakeup(photoperiodAt(ON_S + 600, true), ON_S + 600, true, 3 * 3600L);
  CHECK(p.wantOn && p.makeup);
  CHECK(p.secondsToChange == MAKEUP_MAX_PER_NIGHT_S - 600);  // slot ends first
  p = withMakeup(photoperiodAt(ON_S + 600, true), ON_S + 600, true, 300);
  CHECK(p.wantOn && p.secondsToChange == 300);                // debt ends first
  p = withMakeup(photoperiodAt(ON_S + 3 * 3600L, true), ON_S + 3 * 3600L, true, 3600);
  CHECK(!p.wantOn && !p.makeup);                              // past the slot: dark
  p = withMakeup(photoperiodAt(ON_S + 600, true), ON_S + 600, false, 3600);
  CHECK(!p.wantOn);                                           // not started
  p = withMakeup(photoperiodAt(ON_S + 600, false), ON_S + 600, true, 3600);
  CHECK(!p.wantOn);                                           // no clock
  p = withMakeup(photoperiodAt(600, true), 600, true, 3600);
  CHECK(p.wantOn && !p.makeup);                               // day: schedule alone

  // Manual on: lit in the dark period for its time, then the schedule again.
  const long dark = ON_S + 3600;                              // 1 h into the dark period
  p = withManual(photoperiodAt(dark, true), 2 * 3600L);
  CHECK(p.wantOn && p.manual && p.secondsToChange == 2 * 3600L);
  p = withManual(photoperiodAt(dark, true), 0);
  CHECK(!p.wantOn && !p.manual);                              // ended: dark
  // Runs into the next ON window: off only at that window's scheduled OFF.
  p = withManual(photoperiodAt(DAY_S - 1800, true), 3600);
  CHECK(p.wantOn && p.secondsToChange == 1800 + ON_S);
  // During the day: on until the later of its end and the scheduled OFF.
  p = withManual(photoperiodAt(ON_S - 600, true), 3600);
  CHECK(p.wantOn && p.secondsToChange == 3600);
  p = withManual(photoperiodAt(600, true), 3600);
  CHECK(p.wantOn && p.secondsToChange == ON_S - 600);
  // Without the clock it still switches on.
  p = withManual(photoperiodAt(ON_S + 10, false), 600);
  CHECK(p.wantOn && p.manual && !p.fromClock);
  CHECK(MANUAL_MAX_S == ON_S);
}

static void test_level_smoother() {
  LevelSmoother::State st;
  CHECK(LevelSmoother::median(st, 0) == -1);
  // 18 Oct refill: readings every 2 s around 35 % with ripple spikes.
  const int seq[] = {35, 35, 40, 35, 30, 35, 35, 45, 35, 35, 35, 40, 35, 35, 35};
  unsigned long t = 0;
  for (int v : seq) {
    LevelSmoother::add(st, v, t);
    t += 2000;
  }
  CHECK(LevelSmoother::median(st, t) == 35);         // spikes do not move the median
  CHECK(LevelSmoother::spread(st, t) == 15);         // 30..45
  CHECK(LevelSmoother::noisy(st, t));
  // A steady stick is not noisy, and old readings age out of the window.
  LevelSmoother::State calm;
  for (int i = 0; i < 40; i++) {
    LevelSmoother::add(calm, i < 10 ? 30 : 35, i * 2000UL);
  }
  CHECK(LevelSmoother::median(calm, 80000) == 35);
  CHECK(LevelSmoother::spread(calm, 80000) == 0);     // the 30 % readings are > 60 s old
  CHECK(!LevelSmoother::noisy(calm, 80000));
  // One honest step (35 -> 40) is not noise.
  LevelSmoother::State step;
  for (int i = 0; i < 20; i++) {
    LevelSmoother::add(step, i < 10 ? 35 : 40, i * 2000UL);
  }
  CHECK(!LevelSmoother::noisy(step, 40000));
  LevelSmoother::add(step, -1, 40000);                // out of range readings are ignored
  CHECK(LevelSmoother::median(step, 40000) == 40);
}

int main() {
  test_dose_law();
  test_gain_update();
  test_ec_loop();
  test_ec_slow_mixing_no_overshoot();
  test_ph_tuned();
  test_ph_no_response();
  test_ec_no_response();
  test_solution_monitor();
  test_dashboard_bands();
  test_filter();
  test_light_makeup();
  test_level_smoother();
  if (failures == 0) {
    std::printf("all control logic tests passed\n");
    return 0;
  }
  std::printf("%d failure(s)\n", failures);
  return 1;
}
