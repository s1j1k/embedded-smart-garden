/************************************************************
 * Stage machine + control cycle orchestration
 *
 * Reuses pH / EC / level loops across stages. EC runs before pH
 * in VEGETATIVE and HARVEST. Top-ups trigger a chemistry re-check.
 ************************************************************/

#ifndef SMART_GARDEN_STAGE_CONTROLLER_H
#define SMART_GARDEN_STAGE_CONTROLLER_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>
#include "control_design.h"
#include "grow_stage.h"
#include "ph_control_loop.h"
#include "ec_control_loop.h"
#include "level_control_loop.h"
#include "level_smoother.h"
#include "control_event_log.h"
#include "tds_nutrient_observer.h"
#include "tank_geometry.h"

namespace StageController {

constexpr const char* FILL0_PATH = "/fill_0.txt";
constexpr const char* STAGE_PATH = "/grow_stage.txt";
constexpr const char* READY_PATH = "/ready_to_seed.txt";
constexpr const char* BASELINE_CAPTURE_PATH = "/baseline_capture.txt";
constexpr const char* VEG_SETPOINTS_PATH = "/veg_setpoints.txt";
// Latched pH no-response faults ("up down" as 0/1), so a reboot or a firmware
// flash cannot quietly restart dosing into a line that is not delivering.
constexpr const char* PH_NO_RESPONSE_PATH = "/ph_no_response.txt";
constexpr const char* EC_NO_RESPONSE_PATH = "/ec_no_response.txt";
// Learned tank area for the level loop (mL per mm of level).
constexpr const char* LEVEL_GAIN_PATH = "/level_ml_per_mm.txt";
// Loops paused one at a time by the operator ("level nutrient ph" as 0/1), so a
// reboot keeps them paused.
constexpr const char* LOOPS_PAUSED_PATH = "/loops_paused.txt";
constexpr unsigned long WATER_SETTLE_MS = ControlDesign::WATER_SETTLE_MS;

enum Activity {
  IDLE = 0,
  WAIT_WATER_SETTLE,
  WAIT_EC_SETTLE,
  WAIT_PH_SETTLE,
  AWAIT_RECHECK,
  // After a nutrient dose's TDS recheck: the nutrient also lowers pH, and the
  // pH reading takes phSettleMs to show it, so the pH check waits until then.
  WAIT_EC_PH_MIX,
  // Between top-up chunks of a refill: the peristaltic pump rests.
  WAIT_TOPUP_REST
};

enum ActionType {
  ACTION_NONE = 0,
  ACTION_LOG_ONLY,
  ACTION_TOP_UP_WATER,
  ACTION_DOSE_EC,
  ACTION_DOSE_PH_UP,
  ACTION_DOSE_PH_DOWN
};

struct Action {
  ActionType type = ACTION_NONE;
  float volumeMl = 0.0f;
  float phBefore = NAN;
  float ecBefore = NAN;
  const char* reason = "";
};

struct Sensors {
  float ph = NAN;
  float ecPpm = NAN;
  float waterTempC = NAN;
  int levelPercent = -1;    // smoothed (median of the last 30 s of stick readings)
  int levelSpreadPct = 0;   // highest minus lowest stick reading over the last 60 s
};

struct Context {
  GrowStage::Id stage = GrowStage::IDLE;
  bool stagePersisted = false;  // true once /grow_stage.txt has been written
  int fill0Percent = -1;
  bool readyToSeed = false;
  PhControl::State ph;
  EcControl::State ec;
  Activity activity = IDLE;
  unsigned long waitUntilMs = 0;
  unsigned long lastLevelCheckMs = 0;
  unsigned long lastPhCheckMs = 0;
  unsigned long lastEcCheckMs = 0;
  bool manualCyclePending = false;
  bool chemistryAfterTopUpPending = false;
  bool runPhAfterEcSettle = false;
  // Set when a dose has settled: the loop is re-checked on the next pass
  // instead of waiting for the maintenance interval, until a check finds
  // the reading inside its dead limits (or the cap stops dosing).
  bool ecRecheckPending = false;
  bool phRecheckPending = false;
  // A filtered reading outside its dead limits started this cycle. If such a
  // cycle ends without a dose, the limits cannot start another until
  // LIMIT_CYCLE_RETRY_MS after limitRetryFromMs.
  bool limitCyclePending = false;
  bool limitRetryHold = false;
  unsigned long limitRetryFromMs = 0;
  // The last nutrient dose was refused (empty bottle, pump hold, pumps off):
  // set by the firmware, cleared when the next nutrient dose is planned.
  bool nutrientDoseRefused = false;
  unsigned long ecDoseEndMs = 0;
  unsigned long bootHoldUntilMs = 0;
  bool bootHoldActive = false;
  float pendingPhBefore = NAN;
  float pendingEcBefore = NAN;
  PhControl::Direction pendingPhDir = PhControl::NONE;
  float pendingPhDoseMl = 0.0f;
  float pendingEcDoseMl = 0.0f;
  float lastTopUpMl = 0.0f;
  // The last top-up was refused (water pump offline, level fault, stock): the
  // level loop steps aside until the next level interval so nutrient and pH
  // still run. Set by the firmware, cleared once a top-up is no longer needed.
  bool topUpRefused = false;
  unsigned long topUpRefusedAtMs = 0;
  // A refill is under way: top-ups follow each other (pump chunks, short rests)
  // until the drawdown is back at the target, then the mixing wait, then
  // nutrient and pH. refillStartedMs bounds it (TOPUP_REFILL_LIMIT_MS).
  bool refilling = false;
  unsigned long refillStartedMs = 0;
  unsigned long waterMixStartedMs = 0;
  // A refill waits for the operator (refill_proceed) when the stick is noisy
  // or the level does not rise; nutrient and pH carry on meanwhile.
  bool refillHold = false;
  const char* refillHoldReason = "";
  bool refillConfirmed = false;   // operator confirmed: no noise hold for this refill
  bool proceedPending = false;    // check the level on the next pass after a confirm
  unsigned long levelHighSinceMs = 0;
  int refillStartPct = -1;
  float refillPlannedMl = 0.0f;
  float refillDeliveredMl = 0.0f;
  int refillStepPct = -1;         // smoothed level at the last step crossing
  float refillMlAtStep = 0.0f;    // delivered mL at that crossing
  bool refillStepSeen = false;    // the first crossing only anchors (start point unknown)
  bool refillDoneLogPending = false;
  unsigned long refillDoneMin = 0;
  // Last finished refill (dashboard "refill done" notice); 0 = none since boot.
  unsigned long refillLastDoneMs = 0;
  float refillLastDeliveredMl = 0.0f;
  float mlPerMmEst = ControlDesign::REFILL_ML_PER_MM_SEED;
  // Operator pause of one loop; the others keep running in their sequence.
  bool levelPaused = false;
  bool nutrientPaused = false;
  bool phPaused = false;
  // Filtered TDS sampled once a minute while the nutrient mixes (settle detection).
  float ecHist[6] = {NAN, NAN, NAN, NAN, NAN, NAN};
  unsigned long ecHistMs[6] = {0, 0, 0, 0, 0, 0};
  int ecHistN = 0;
  // Nutrient phase: set when a nutrient dose goes in, kept while each recheck
  // still reads below the dead limit. pH is not checked until it clears, so the
  // nutrient (which shifts pH) is finished before pH is corrected.
  bool nutrientPhase = false;
  // Filtered pH sampled once a minute after any dose (pH settle detection).
  float phHist[6] = {NAN, NAN, NAN, NAN, NAN, NAN};
  unsigned long phHistMs[6] = {0, 0, 0, 0, 0, 0};
  int phHistN = 0;
  unsigned long phWaitNominalEndMs = 0;  // when the pH wait would end without drift
  uint32_t fillSeq = 0;                  // bumped at every confirmed refill (solution monitor)
};

inline Context& ctx() {
  static Context c;
  return c;
}

inline const GrowStage::Config& stageCfg() {
  return GrowStage::config(ctx().stage);
}

// Forward declarations: Arduino/ESP32 builds a single translation unit but
// still requires declarations before use in functions defined earlier.
inline bool saveFill0(int pct);
inline bool saveStage(GrowStage::Id stage);
inline bool saveChemistryBands(GrowStage::Id stage);
inline bool loadChemistryBands();

inline void savePhNoResponse() {
  const PhControl::State& ph = ctx().ph;
  File f = LittleFS.open(PH_NO_RESPONSE_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[STAGE] WARNING: could not save the pH no-response fault to LittleFS");
    return;
  }
  f.print(ph.noResponseFaultUp ? 1 : 0);
  f.print(' ');
  f.println(ph.noResponseFaultDown ? 1 : 0);
  f.close();
}

inline void saveLevelGain() {
  File f = LittleFS.open(LEVEL_GAIN_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[STAGE] WARNING: could not save the level gain to LittleFS");
    return;
  }
  f.println(ctx().mlPerMmEst, 1);
  f.close();
}

inline void loadLevelGain() {
  File f = LittleFS.open(LEVEL_GAIN_PATH, FILE_READ);
  if (!f) {
    return;
  }
  const float v = f.parseFloat();
  f.close();
  if (v >= ControlDesign::REFILL_ML_PER_MM_MIN && v <= ControlDesign::REFILL_ML_PER_MM_MAX) {
    ctx().mlPerMmEst = v;
    Serial.print("[STAGE] Level gain restored: ");
    Serial.print(v, 1);
    Serial.println(" mL per mm");
  }
}

inline void saveLoopsPaused() {
  const Context& c = ctx();
  File f = LittleFS.open(LOOPS_PAUSED_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[STAGE] WARNING: could not save the loop pauses to LittleFS");
    return;
  }
  f.print(c.levelPaused ? 1 : 0);
  f.print(' ');
  f.print(c.nutrientPaused ? 1 : 0);
  f.print(' ');
  f.println(c.phPaused ? 1 : 0);
  f.close();
}

inline void loadLoopsPaused() {
  File f = LittleFS.open(LOOPS_PAUSED_PATH, FILE_READ);
  if (!f) {
    return;
  }
  Context& c = ctx();
  c.levelPaused = f.parseInt() == 1;
  c.nutrientPaused = f.parseInt() == 1;
  c.phPaused = f.parseInt() == 1;
  f.close();
  if (c.levelPaused || c.nutrientPaused || c.phPaused) {
    Serial.print("[STAGE] Loops paused (saved):");
    Serial.print(c.levelPaused ? " level" : "");
    Serial.print(c.nutrientPaused ? " nutrient" : "");
    Serial.println(c.phPaused ? " pH" : "");
  }
}

inline void loadPhNoResponse() {
  PhControl::State& ph = ctx().ph;
  File f = LittleFS.open(PH_NO_RESPONSE_PATH, FILE_READ);
  if (!f) {
    return;
  }
  ph.noResponseFaultUp = f.parseInt() == 1;
  ph.noResponseFaultDown = f.parseInt() == 1;
  f.close();
  if (ph.noResponseFaultUp || ph.noResponseFaultDown) {
    Serial.print("[STAGE] pH no-response fault restored (");
    Serial.print(ph.noResponseFaultUp ? "pH-up " : "");
    Serial.print(ph.noResponseFaultDown ? "pH-down " : "");
    Serial.println(") — that direction is not dosed until SESSION RESET PH");
  }
}

inline void saveEcNoResponse() {
  File f = LittleFS.open(EC_NO_RESPONSE_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[STAGE] WARNING: could not save the nutrient no-response fault to LittleFS");
    return;
  }
  f.println(ctx().ec.noResponseFault ? 1 : 0);
  f.close();
}

inline void loadEcNoResponse() {
  File f = LittleFS.open(EC_NO_RESPONSE_PATH, FILE_READ);
  if (!f) {
    return;
  }
  ctx().ec.noResponseFault = f.parseInt() == 1;
  f.close();
  if (ctx().ec.noResponseFault) {
    Serial.println("[STAGE] Nutrient no-response fault restored — no nutrient dosing until "
                   "SESSION RESET EC");
  }
}

inline bool loadPersisted() {
  Context& c = ctx();
  c.stagePersisted = false;
  c.stage = GrowStage::IDLE;
  if (LittleFS.exists(FILL0_PATH)) {
    File f = LittleFS.open(FILL0_PATH, FILE_READ);
    if (f) {
      c.fill0Percent = f.parseInt();
      f.close();
    }
  }
  if (LittleFS.exists(STAGE_PATH)) {
    File f = LittleFS.open(STAGE_PATH, FILE_READ);
    if (f) {
      String raw = f.readStringUntil('\n');
      f.close();
      raw.trim();
      GrowStage::Id loaded = GrowStage::IDLE;
      bool ok = false;
      if (raw.length() > 0 && (raw[0] < '0' || raw[0] > '9')) {
        ok = GrowStage::parseName(raw.c_str(), loaded);
      } else {
        // Legacy integer layout (pre-TRANSITIONAL insert):
        // 0 FILLING … 2 SEEDLING, 3 VEGETATIVE, 4 HARVEST, 5 SENSOR_BASELINE, 6 IDLE
        const int s = raw.toInt();
        static const GrowStage::Id kLegacy[] = {
            GrowStage::FILLING,         GrowStage::PH_BASELINE,
            GrowStage::SEEDLING,        GrowStage::VEGETATIVE,
            GrowStage::HARVEST,         GrowStage::SENSOR_BASELINE,
            GrowStage::IDLE};
        if (s >= 0 && s < 7) {
          loaded = kLegacy[s];
          ok = true;
        }
      }
      if (ok) {
        c.stage = loaded;
        c.stagePersisted = true;
      }
    }
  }
  // No stage file → stay IDLE (sensors only). Operator must set a stage
  // (dashboard / Serial MODE) before any auto control runs; that write persists.
  c.readyToSeed = LittleFS.exists(READY_PATH);
  loadChemistryBands();
  PhControl::initGains(c.ph, GrowStage::config(c.stage));
  loadPhNoResponse();
  EcControl::initGain(c.ec, GrowStage::config(c.stage));
  loadEcNoResponse();
  loadLevelGain();
  loadLoopsPaused();
  TdsNutrientObserver::load();
  if (c.stage == GrowStage::IDLE) {
    Serial.println("[STAGE] Idle — no auto control until MODE / dashboard sets a stage");
  } else {
    Serial.print("[STAGE] Restored persisted stage: ");
    Serial.println(GrowStage::name(c.stage));
  }
  return true;
}

// Call once after boot load. The sensor averages need a few minutes to warm up
// before a reading is trusted, so no automatic action runs until
// POST_BOOT_HOLD_MS has passed; then every loop is checked straight away.
// Serial CYCLE still bypasses the hold via manualCyclePending.
// A clean start: forget every wait, pending check and half-finished dose from
// before, hold for POST_BOOT_HOLD_MS while the sensor averages settle, then run
// the full level -> nutrient -> pH cycle on fresh readings. Used at boot and when
// auto control resumes, because a pause can last through a water change: a
// nutrient "still mixing" skip or a pending pH recheck from the old tank must
// not decide the first action in the new one.
inline void armFreshStart(unsigned long nowMs) {
  Context& c = ctx();
  c.pendingPhBefore = NAN;
  c.pendingEcBefore = NAN;
  c.pendingPhDir = PhControl::NONE;
  c.pendingPhDoseMl = 0.0f;
  c.pendingEcDoseMl = 0.0f;
  c.ecHistN = 0;
  c.phHistN = 0;
  c.ph.lastDirection = PhControl::NONE;
  c.ph.lastDoseMl = 0.0f;
  c.ph.phBeforeLastDose = NAN;
  c.ec.lastDoseMl = 0.0f;
  c.ec.ecBeforeLastDose = NAN;
  c.lastLevelCheckMs = 0;
  c.lastPhCheckMs = 0;
  c.lastEcCheckMs = 0;
  c.activity = IDLE;
  c.waitUntilMs = 0;
  c.manualCyclePending = false;
  c.chemistryAfterTopUpPending = false;
  c.runPhAfterEcSettle = false;
  c.ecRecheckPending = false;
  c.phRecheckPending = false;
  c.limitCyclePending = false;
  c.limitRetryHold = false;
  c.nutrientDoseRefused = false;
  c.topUpRefused = false;
  c.refilling = false;
  c.refillHold = false;
  c.refillHoldReason = "";
  c.refillConfirmed = false;
  c.levelHighSinceMs = 0;
  c.nutrientPhase = false;
  c.bootHoldUntilMs = nowMs + ControlDesign::POST_BOOT_HOLD_MS;
  c.bootHoldActive = true;
}

inline void armPostBootHold(unsigned long nowMs) {
  armFreshStart(nowMs);
  Serial.print("[STAGE] Boot hold — first automatic check in ");
  Serial.print(ControlDesign::POST_BOOT_HOLD_MS / 60000UL);
  Serial.println(" min (sensor averages warm up). Type CYCLE to run one control cycle now.");
}

inline bool saveFill0(int pct) {
  File f = LittleFS.open(FILL0_PATH, FILE_WRITE);
  if (!f) {
    return false;
  }
  f.println(pct);
  f.close();
  ctx().fill0Percent = pct;
  return true;
}

inline bool saveStage(GrowStage::Id stage) {
  File f = LittleFS.open(STAGE_PATH, FILE_WRITE);
  if (!f) {
    return false;
  }
  // Persist by name so inserting stages (e.g. TRANSITIONAL) does not remap ints.
  f.println(GrowStage::name(stage));
  f.close();
  ctx().stage = stage;
  ctx().stagePersisted = true;
  PhControl::initGains(ctx().ph, GrowStage::config(stage));
  EcControl::initGain(ctx().ec, GrowStage::config(stage));
  return true;
}

inline bool saveChemistryBands(GrowStage::Id stage) {
  // Persist vegetative plant-catalogue bands (other stages keep compile defaults).
  if (stage != GrowStage::VEGETATIVE) {
    return true;
  }
  const GrowStage::Config& cfg = GrowStage::config(stage);
  File f = LittleFS.open(VEG_SETPOINTS_PATH, FILE_WRITE);
  if (!f) {
    return false;
  }
  // target bands, then dead limits and pH setpoint (1.9+).
  f.printf(
      "%.3f,%.3f,%.4f,%.4f,%.3f,%.3f,%.4f,%.4f,%.4f\n",
      cfg.ecTargetMin,
      cfg.ecTargetMax,
      cfg.phTargetMin,
      cfg.phTargetMax,
      cfg.ecDeadLow,
      cfg.ecDeadHigh,
      cfg.phDeadLow,
      cfg.phDeadHigh,
      cfg.phCenter);
  f.close();
  return true;
}

inline bool loadChemistryBands() {
  if (!LittleFS.exists(VEG_SETPOINTS_PATH)) {
    return false;
  }
  File f = LittleFS.open(VEG_SETPOINTS_PATH, FILE_READ);
  if (!f) {
    return false;
  }
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  float v[9];
  for (float& x : v) {
    x = NAN;
  }
  const int n = sscanf(line.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8]);
  if (n != 4 && n != 9) {
    return false;
  }
  // A 4-value file is from firmware before 1.9: its dead limits were target
  // +/- 0.1 pH, so only the targets are restored and the design margins apply.
  if (!GrowStage::applyChemistryBands(
          GrowStage::VEGETATIVE, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8])) {
    return false;
  }
  const GrowStage::Config& cfg = GrowStage::config(GrowStage::VEGETATIVE);
  Serial.print("[SETPOINTS] Restored vegetative bands EC ");
  Serial.print(cfg.ecTargetMin, 1);
  Serial.print("-");
  Serial.print(cfg.ecTargetMax, 1);
  Serial.print(" ppm (dead ");
  Serial.print(cfg.ecDeadLow, 1);
  Serial.print("-");
  Serial.print(cfg.ecDeadHigh, 1);
  Serial.print(")  pH ");
  Serial.print(cfg.phCenter, 2);
  Serial.print(" (dead ");
  Serial.print(cfg.phDeadLow, 2);
  Serial.print("-");
  Serial.print(cfg.phDeadHigh, 2);
  Serial.println(")");
  return true;
}

inline void setReadyToSeed(bool ready) {
  ctx().readyToSeed = ready;
  if (ready) {
    File f = LittleFS.open(READY_PATH, FILE_WRITE);
    if (f) {
      f.println("1");
      f.close();
    }
  } else if (LittleFS.exists(READY_PATH)) {
    LittleFS.remove(READY_PATH);
  }
}

inline void logEvent(
    const char* event,
    const Sensors& s,
    const char* phDir,
    float phDose,
    float ecDose,
    float phAfter,
    float ecAfter,
    const char* alerts,
    const char* recordedAtUtc) {
  ControlEventLog::Row row;
  row.recordedAtUtc = recordedAtUtc;
  row.uptimeMs = millis();
  row.event = event;
  row.stage = GrowStage::name(ctx().stage);
  row.currentLevelPct = s.levelPercent;
  row.currentDrawdownMm =
      LevelControl::drawdownMm(ctx().fill0Percent, s.levelPercent);
  row.targetDepthMm = stageCfg().targetDrawdownMm;
  row.ecBefore = s.ecPpm;
  row.ecDoseMl = isnan(ecDose) ? NAN : ecDose;
  row.ecAfter = ecAfter;
  row.phBefore = s.ph;
  row.phDoseDir = phDir != nullptr ? phDir : "";
  row.phDoseMl = isnan(phDose) ? NAN : phDose;
  row.phAfter = phAfter;
  row.gainUp = ctx().ph.gainUp;
  row.gainDown = ctx().ph.gainDown;
  row.gainEc = ctx().ec.gainEc;
  row.waterTempC = s.waterTempC;
  row.cumulativeSessionMl = ctx().ph.cumulativeSessionMl + ctx().ec.cumulativeSessionMl;
  row.alertFlags = alerts != nullptr ? alerts : "";
  ControlEventLog::append(row);
  ControlEventLog::logToSerial(row);
#if defined(SMART_GARDEN_EVENT_HOOK)
  SMART_GARDEN_EVENT_HOOK(row);
#endif
}

// Operator override of the session cap, after a person has checked why it was
// reached (a wrong reading, or a dosing line that was not delivering). The loop
// starts a fresh session, so the cap applies again from zero. Logged as an event.
inline void resetDosingSession(bool ph, bool ec, const Sensors& s, const char* recordedAtUtc) {
  Context& c = ctx();
  if (ph) {
    PhControl::resetSession(c.ph);
    // The operator has checked the line, so the no-response fault clears too.
    PhControl::clearNoResponse(c.ph, PhControl::NONE);
    savePhNoResponse();
    logEvent("ph_session_reset", s, "", NAN, NAN, NAN, NAN, "operator_override", recordedAtUtc);
  }
  if (ec) {
    EcControl::resetSession(c.ec);
    EcControl::clearNoResponse(c.ec);  // the operator has checked the line
    saveEcNoResponse();
    logEvent("ec_session_reset", s, "", NAN, NAN, NAN, NAN, "operator_override", recordedAtUtc);
  }
}

inline void transitionTo(GrowStage::Id next) {
  Context& c = ctx();
  if (c.stage == next) {
    return;
  }
  saveStage(next);
  PhControl::resetSession(c.ph);
  EcControl::resetSession(c.ec);
  c.activity = IDLE;
  c.waitUntilMs = 0;
  c.manualCyclePending = false;
  c.chemistryAfterTopUpPending = false;
  c.runPhAfterEcSettle = false;
  c.ecRecheckPending = false;
  c.phRecheckPending = false;
  c.nutrientPhase = false;
  // A new stage is checked straight away (after any boot hold): a bring-up
  // from plain water starts dosing on the next pass, not after 2 h.
  c.lastLevelCheckMs = 0;
  c.lastPhCheckMs = 0;
  c.lastEcCheckMs = 0;
  if (next == GrowStage::PH_BASELINE) {
    setReadyToSeed(false);
  }
  Serial.print("[STAGE] → ");
  Serial.println(GrowStage::name(next));
}

inline bool markBaselineCaptureStart() {
  File f = LittleFS.open(BASELINE_CAPTURE_PATH, FILE_WRITE);
  if (!f) {
    return false;
  }
  f.println(millis());
  f.close();
  return true;
}

inline bool markBaselineCaptureEnd() {
  if (!LittleFS.exists(BASELINE_CAPTURE_PATH)) {
    return false;
  }
  File f = LittleFS.open(BASELINE_CAPTURE_PATH, FILE_APPEND);
  if (!f) {
    return false;
  }
  f.print("end=");
  f.println(millis());
  f.close();
  return true;
}

inline bool confirmFillComplete(int levelPercent) {
  if (levelPercent < 0) {
    return false;
  }
  // Full reservoir refill — pH-reagent salt pool is discarded with the old
  // solution, so the TDS nutrient observer accumulators reset here.
  TdsNutrientObserver::resetAccumulators();
  ctx().fillSeq++;
  saveFill0(levelPercent);
  transitionTo(GrowStage::PH_BASELINE);
  Serial.print("[STAGE] fill_0 recorded at ");
  Serial.print(levelPercent);
  Serial.println("%");
  return true;
}

inline bool recordFill0Only(int levelPercent) {
  if (levelPercent < 0) {
    return false;
  }
  saveFill0(levelPercent);
  Serial.print("[STAGE] fill_0 recorded at ");
  Serial.print(levelPercent);
  Serial.println("% (sensor baseline — no pH control)");
  return true;
}

inline bool isBusy(unsigned long nowMs) {
  const Context& c = ctx();
  if (c.activity != IDLE && static_cast<long>(nowMs - c.waitUntilMs) < 0) {
    return true;
  }
  return false;
}

inline void requestManualCycle() {
  ctx().manualCyclePending = true;
  InfluxCloud::preemptForControl();
}

// One loop now (1.9.28): "level", "nutrient" (or "ec") or "ph"; false for
// anything else. It runs through the same planner as a full cycle, so the
// level is checked first and pH still waits for TDS to be in range.
inline bool requestLoopRun(const char* loop) {
  Context& c = ctx();
  if (strcmp(loop, "level") == 0) {
    c.proceedPending = true;
  } else if (strcmp(loop, "nutrient") == 0 || strcmp(loop, "ec") == 0) {
    c.ecRecheckPending = true;
  } else if (strcmp(loop, "ph") == 0) {
    c.phRecheckPending = true;
  } else {
    return false;
  }
  InfluxCloud::preemptForControl();
  return true;
}

inline bool intervalElapsed(unsigned long& lastMs, unsigned long intervalMs, unsigned long nowMs) {
  if (intervalMs == 0) {
    return false;
  }
  if (lastMs == 0 || nowMs - lastMs >= intervalMs) {
    return true;
  }
  return false;
}

// Level (percent on the stick) a refill stops at: the stage's target drawdown.
// -1 when the level loop is not set up (no fill_0).
inline int refillTargetPercent() {
  const Context& c = ctx();
  if (c.fill0Percent < 0) {
    return -1;
  }
  return LevelControl::levelPercentForDrawdownMm(c.fill0Percent, stageCfg().targetDrawdownMm);
}

// Level reached: the top-up diluted the tank, so the nutrient check waits until
// TDS stops moving (serviceWaits, WAIT_WATER_SETTLE).
inline void startWaterMix(unsigned long nowMs) {
  Context& c = ctx();
  if (c.refilling) {
    c.refillDoneLogPending = true;  // logged with readings on the next serviceWaits
    c.refillDoneMin = (nowMs - c.refillStartedMs) / 60000UL;
    c.refillLastDoneMs = nowMs == 0 ? 1 : nowMs;
    c.refillLastDeliveredMl = c.refillDeliveredMl;
  }
  c.refillConfirmed = false;
  c.refilling = false;
  c.activity = WAIT_WATER_SETTLE;
  c.waterMixStartedMs = nowMs;
  c.waitUntilMs = nowMs + ControlDesign::SETTLE_POLL_MS;
  c.ecHistN = 0;
  Serial.println("[STAGE-CTRL] Level at target - waiting for TDS to settle before the nutrient check");
}

// The refill stops and waits for the operator (refill_proceed); nutrient and
// pH carry on. reason: noisy_level (stick readings span two or more steps) or
// no_level_rise (two steps' worth of water and no step change).
inline void holdRefill(const Sensors& s, const char* reason, const char* recordedAtUtc) {
  Context& c = ctx();
  c.refilling = false;
  c.refillHold = true;
  c.refillHoldReason = reason;
  c.chemistryAfterTopUpPending = true;
  logEvent("refill_hold", s, "", NAN, NAN, NAN, NAN, reason, recordedAtUtc);
  Serial.print("[STAGE-CTRL] Refill on hold (");
  Serial.print(reason);
  Serial.println(") - check the level stick, then REFILL PROCEED or the dashboard button. "
                 "Nutrient and pH carry on.");
}

// Operator: go ahead with the refill. The noise check is off for this refill;
// the no-rise check starts again from here.
inline void confirmRefill() {
  Context& c = ctx();
  c.refillHold = false;
  c.refillHoldReason = "";
  c.refillConfirmed = true;
  c.proceedPending = true;
  c.levelHighSinceMs = 0;
}

inline void addRefillDelivered(float ml) {
  if (ml > 0.0f) {
    ctx().refillDeliveredMl += ml;
  }
}

// During a refill: each time the smoothed level rises a step, the volume since
// the previous crossing gives the tank's mL per mm. The first crossing only
// anchors, because the refill starts somewhere inside a step.
inline void trackRefillSteps(const Sensors& s, const char* recordedAtUtc) {
  Context& c = ctx();
  const int stepPct = static_cast<int>(ControlDesign::LEVEL_STEP_MM / Tank::GROVE_MM_PER_PERCENT + 0.5f);
  if (c.refillStepPct < 0 || s.levelPercent < c.refillStepPct + stepPct) {
    return;
  }
  const float ml = c.refillDeliveredMl - c.refillMlAtStep;
  const float mm = (s.levelPercent - c.refillStepPct) * Tank::GROVE_MM_PER_PERCENT;
  if (c.refillStepSeen && mm > 0.0f) {
    const float sample = ml / mm;
    if (sample >= ControlDesign::REFILL_ML_PER_MM_MIN * 0.5f &&
        sample <= ControlDesign::REFILL_ML_PER_MM_MAX * 2.0f) {
      const float before = c.mlPerMmEst;
      float est = 0.5f * before + 0.5f * sample;
      est = fminf(fmaxf(est, ControlDesign::REFILL_ML_PER_MM_MIN), ControlDesign::REFILL_ML_PER_MM_MAX);
      c.mlPerMmEst = est;
      saveLevelGain();
      static char note[64];
      snprintf(note, sizeof(note), "sample=%.0f est=%.0f->%.0f mL/mm", sample, before, est);
      logEvent("level_gain", s, "", NAN, NAN, NAN, NAN, note, recordedAtUtc);
      Serial.print("[STAGE-CTRL] Level step: ");
      Serial.print(ml, 0);
      Serial.print(" mL for ");
      Serial.print(mm, 0);
      Serial.print(" mm -> tank gain ");
      Serial.print(est, 0);
      Serial.println(" mL/mm");
    }
  }
  c.refillStepSeen = true;
  c.refillStepPct = s.levelPercent;
  c.refillMlAtStep = c.refillDeliveredMl;
}

inline Action planLevelCheck(const Sensors& s, unsigned long nowMs, const char* recordedAtUtc) {
  Action action;
  const GrowStage::Config& cfg = stageCfg();
  Context& c = ctx();

  if (!GrowStage::allowsLevelAuto(c.stage) || c.fill0Percent < 0) {
    c.refilling = false;
    return action;
  }
  // Disconnected / faulted stick: Sensors.levelPercent is -1; still bail early
  // so we do not log bogus level_check events every interval.
  if (s.levelPercent < 0) {
    c.refilling = false;
    c.levelHighSinceMs = 0;
    return action;
  }
  if (c.refillHold) {
    return action;  // waiting for the operator; nutrient and pH carry on
  }
  // Event start: the smoothed drawdown past target + tolerance for a minute.
  const float ddNow = LevelControl::drawdownMm(c.fill0Percent, s.levelPercent);
  if (!isnan(ddNow) && ddNow > cfg.targetDrawdownMm + cfg.drawdownToleranceMm) {
    if (c.levelHighSinceMs == 0) {
      c.levelHighSinceMs = nowMs;
    }
  } else {
    c.levelHighSinceMs = 0;
  }
  const bool levelEvent = c.levelHighSinceMs != 0 &&
                          nowMs - c.levelHighSinceMs >= ControlDesign::LEVEL_HIGH_CONFIRM_MS;
  // A refused top-up is not asked for again until the next level interval:
  // asking every pass kept the nutrient and pH checks from ever running.
  if (c.topUpRefused && nowMs - c.topUpRefusedAtMs < cfg.levelCheckIntervalMs) {
    return action;
  }
  // A refill that runs too long (slow pump, leak, wrong reading) must not hold
  // nutrient and pH back: stop it, hand over, and wait for the next interval.
  if (c.refilling && nowMs - c.refillStartedMs >= ControlDesign::TOPUP_REFILL_LIMIT_MS) {
    c.refilling = false;
    c.refillConfirmed = false;
    c.topUpRefused = true;
    c.topUpRefusedAtMs = nowMs;
    c.chemistryAfterTopUpPending = true;
    logEvent("topup_time_limit", s, "", NAN, NAN, NAN, NAN, "level_not_reached", recordedAtUtc);
    Serial.print("[STAGE-CTRL] Refill stopped after ");
    Serial.print(ControlDesign::TOPUP_REFILL_LIMIT_MS / 60000UL);
    Serial.println(" min without reaching the target - nutrient and pH run now; check the "
                   "water pump and its source");
    return action;
  }
  // No timer (1.9.24): the level is checked when the smoothed drawdown has been
  // past its band for a minute, straight after every refill chunk, after an
  // operator confirm, and at the start of every chemistry check (the volume
  // sets every concentration, so a top-up comes first).
  if (!c.refilling && !levelEvent && !c.proceedPending &&
      !c.manualCyclePending && !c.limitCyclePending &&
      !c.chemistryAfterTopUpPending && !c.ecRecheckPending && !c.phRecheckPending) {
    return action;
  }

  c.lastLevelCheckMs = nowMs;
  c.proceedPending = false;
  if (c.refilling) {
    trackRefillSteps(s, recordedAtUtc);
  }
  // A refill starts past target + tolerance and then fills to the target itself.
  const int tolerance = c.refilling ? 0 : cfg.drawdownToleranceMm;
  LevelControl::CheckResult lvl = LevelControl::evaluateCheck(
      c.fill0Percent, s.levelPercent, cfg.targetDrawdownMm, tolerance);

  logEvent("level_check", s, "", NAN, NAN, NAN, NAN,
           c.refilling ? "refill" : "", recordedAtUtc);

  if (lvl.topUpNeeded) {
    // Volume from the learned tank gain, not the fixed model.
    lvl.topUpMl = fmaxf(1.0f, (lvl.currentDrawdownMm - cfg.targetDrawdownMm) * c.mlPerMmEst);
  }

  if (lvl.topUpNeeded && lvl.topUpMl > 0.0f) {
    // A noisy stick (or one that has not moved for two steps' worth of water)
    // cannot be trusted to stop a refill: hold for the operator.
    if (s.levelSpreadPct >= LevelSmoother::NOISY_SPAN_PCT && !c.refillConfirmed) {
      holdRefill(s, "noisy_level", recordedAtUtc);
      return action;
    }
    if (c.refilling && c.refillDeliveredMl - c.refillMlAtStep >
                           ControlDesign::REFILL_NO_RISE_STEPS * ControlDesign::LEVEL_STEP_MM *
                               c.mlPerMmEst) {
      holdRefill(s, "no_level_rise", recordedAtUtc);
      return action;
    }
    if (!c.refilling) {
      c.refilling = true;
      c.refillStartedMs = nowMs;
      c.refillStartPct = s.levelPercent;
      c.refillPlannedMl = lvl.topUpMl;
      c.refillDeliveredMl = 0.0f;
      c.refillStepPct = s.levelPercent;
      c.refillMlAtStep = 0.0f;
      c.refillStepSeen = false;
      Serial.print("[STAGE-CTRL] Refill started: drawdown ");
      Serial.print(lvl.currentDrawdownMm, 0);
      Serial.print(" mm, target ");
      Serial.print(cfg.targetDrawdownMm);
      Serial.print(" mm, about ");
      Serial.print(lvl.topUpMl, 0);
      Serial.println(" mL");
    }
    action.type = ACTION_TOP_UP_WATER;
    action.volumeMl = lvl.topUpMl;
    action.reason = lvl.reason;
    c.lastTopUpMl = lvl.topUpMl;
    c.chemistryAfterTopUpPending = true;
  } else {
    c.topUpRefused = false;
    if (c.refilling) {
      startWaterMix(nowMs);
    }
  }
  return action;
}

// The firmware could not start the planned top-up. The cycle goes on to the
// nutrient and pH checks (chemistryAfterTopUpPending is still set), and the
// top-up is tried again at the next level interval.
inline void onTopUpRefused(unsigned long nowMs) {
  Context& c = ctx();
  if (!c.topUpRefused) {
    Serial.println("[STAGE-CTRL] Top-up not started - nutrient and pH run without it; "
                   "top up by hand");
  }
  c.topUpRefused = true;
  c.topUpRefusedAtMs = nowMs;
  c.refilling = false;
}

// The firmware could not start the planned pH dose (pump offline, low stock).
// The out-of-limits pH would otherwise plan the same dose on every pass, so the
// limits wait LIMIT_CYCLE_RETRY_MS before starting another cycle.
inline void onPhDoseRefused(unsigned long nowMs) {
  Context& c = ctx();
  c.pendingPhDir = PhControl::NONE;
  c.limitRetryHold = true;
  c.limitRetryFromMs = nowMs;
}

// Plain water (TDS under PLAIN_WATER_PPM) means the tank was changed: both
// session caps and the pumps' no-response warnings start again, as with an
// operator SESSION RESET. A pump that really does not deliver warns again after
// PH_NO_RESPONSE_DOSES / EC_NO_RESPONSE_DOSES doses. Nothing is logged when
// there is nothing to clear, so a plain-water tank does not log every check.
inline void resetSessionsForPlainWater(const Sensors& s, const char* recordedAtUtc) {
  Context& c = ctx();
  const bool ph = c.ph.cumulativeSessionMl > 0.0f || c.ph.sessionCapAlert ||
                  c.ph.noResponseFaultUp || c.ph.noResponseFaultDown ||
                  c.ph.noResponseUp > 0 || c.ph.noResponseDown > 0;
  const bool ec = c.ec.cumulativeSessionMl > 0.0f || c.ec.sessionCapAlert ||
                  c.ec.noResponseFault || c.ec.noResponseCount > 0;
  if (ph) {
    PhControl::resetSession(c.ph);
    PhControl::clearNoResponse(c.ph, PhControl::NONE);
    savePhNoResponse();
    logEvent("ph_session_reset", s, "", NAN, NAN, NAN, NAN, "plain_water", recordedAtUtc);
    Serial.println("[STAGE-CTRL] plain water detected - pH session and warnings reset");
  }
  if (ec) {
    EcControl::resetSession(c.ec);
    EcControl::clearNoResponse(c.ec);
    saveEcNoResponse();
    logEvent("ec_session_reset", s, "", NAN, NAN, NAN, NAN, "plain_water", recordedAtUtc);
    Serial.println("[STAGE-CTRL] plain water detected - nutrient session and warnings reset");
  }
}

inline Action planEcCheck(const Sensors& s, unsigned long nowMs, const char* recordedAtUtc) {
  Action action;
  Context& c = ctx();
  const GrowStage::Config& cfg = stageCfg();

  if (!GrowStage::allowsEc(c.stage)) {
    return action;
  }
  // An EC dose has settled and pH is owed: pH runs before EC is looked at again.
  if (c.runPhAfterEcSettle && GrowStage::allowsPh(c.stage)) {
    return action;
  }

  const bool eventDriven = c.manualCyclePending || c.limitCyclePending ||
                           c.chemistryAfterTopUpPending || c.ecRecheckPending;
  if (!eventDriven &&
      !intervalElapsed(c.lastEcCheckMs, cfg.ecCheckIntervalMs, nowMs)) {
    return action;
  }

  c.lastEcCheckMs = nowMs;
  c.ecRecheckPending = false;
  // A dry probe reads 0 ppm, which would look like plain water and earn a full dose.
  if (!isnan(s.ecPpm) && s.ecPpm < ControlDesign::TDS_PROBE_DRY_PPM) {
    logEvent("ec_check", s, "", NAN, NAN, NAN, NAN, "tds_probe_dry", recordedAtUtc);
    Serial.println("[STAGE-CTRL] TDS reads ~0 ppm - probe out of the water? No nutrient dose.");
    return action;
  }
  if (TdsNutrientObserver::resetIfPlainWater(s.ecPpm)) {
    Serial.println("[TDS-OBS] plain water detected - pH-reagent salt accumulators cleared");
  }
  // Plain water is a new tank: a fresh session, the same as SESSION RESET.
  if (s.ecPpm < TdsNutrientObserver::PLAIN_WATER_PPM) {
    resetSessionsForPlainWater(s, recordedAtUtc);
  }
  // Control acts on the total TDS reading. The pH-reagent salt correction was
  // removed in 1.9.11: its constants were never measured, it drifted with water
  // changes, and the salt is tens of ppm (the potassium is itself a nutrient).
  const float V = TdsNutrientObserver::volumeLFromLevelPct(s.levelPercent);
  const float ecForControl = s.ecPpm;
  const float capV = ControlDesign::capVolumeL(s.levelPercent >= 0 ? V : NAN);
  EcControl::CheckResult ec = EcControl::evaluateCheck(ecForControl, cfg, c.ec, capV);

  const char* alerts = ec.sessionCapHit ? "ec_session_cap"
                       : ec.noResponseHit ? "nutrient_no_response"
                                          : "";
  logEvent("ec_check", s, "", NAN, ec.noAction ? NAN : ec.doseMl, NAN, NAN, alerts, recordedAtUtc);

  c.nutrientPhase = !ec.noAction && ec.doseMl > 0.0f;
  if (!ec.noAction && ec.doseMl > 0.0f) {
    action.type = ACTION_DOSE_EC;
    action.volumeMl = ec.doseMl;
    action.ecBefore = ecForControl;
    action.reason = ec.reason;
    EcControl::recordDoseStart(c.ec, ecForControl);
    c.nutrientDoseRefused = false;
    c.pendingEcBefore = ecForControl;
    c.pendingEcDoseMl = 0.0f;  // set to the delivered volume when the pump stops
  }
  return action;
}

// The nutrient loop cannot bring the TDS in: no-response fault, session cap,
// a refused dose, or a dry TDS probe. pH is then corrected on its own to keep
// the plants healthy until it is fixed.
inline bool nutrientLoopFaulted(float ecPpm) {
  const Context& c = ctx();
  return c.nutrientPaused || c.ec.noResponseFault || c.ec.sessionCapAlert || c.nutrientDoseRefused ||
         (!isnan(ecPpm) && ecPpm < ControlDesign::TDS_PROBE_DRY_PPM);
}

// pH doses only once TDS control has finished with the TDS in range. While the
// nutrient loop is dosing, mixing or still has TDS below its dead limit to
// correct, the nutrient that follows
// would shift the pH again. Above the high limit counts as finished: the
// controller never dilutes.
inline bool phWaitsForNutrient(float ecPpm) {
  const Context& c = ctx();
  if (!GrowStage::allowsEc(c.stage)) {
    return false;
  }
  if (isnan(ecPpm)) {
    return true;  // TDS warming up: the nutrient loop has not looked yet
  }
  if (nutrientLoopFaulted(ecPpm)) {
    return false;
  }
  return c.nutrientPhase || ecPpm < stageCfg().ecDeadLow;
}

inline Action planPhCheck(const Sensors& s, unsigned long nowMs, const char* recordedAtUtc) {
  Action action;
  Context& c = ctx();
  const GrowStage::Config& cfg = stageCfg();

  if (!GrowStage::allowsPh(c.stage)) {
    return action;
  }
  // Nutrient first: pH is not touched until the nutrient loop has brought the
  // TDS into range, unless a fault stops that loop. An owed pH check is dropped
  // so the nutrient loop is looked at again; once TDS is in range, a pH outside
  // its dead limits starts the pH loop.
  if (phWaitsForNutrient(s.ecPpm)) {
    if (!c.nutrientPhase) {
      c.runPhAfterEcSettle = false;
      c.phRecheckPending = false;
    }
    return action;
  }

  const bool baselineMode = (c.stage == GrowStage::PH_BASELINE);
  const bool eventDriven = c.manualCyclePending || c.limitCyclePending ||
                           c.chemistryAfterTopUpPending || c.runPhAfterEcSettle ||
                           c.phRecheckPending;

  if (!eventDriven &&
      !intervalElapsed(c.lastPhCheckMs, cfg.phCheckIntervalMs, nowMs)) {
    return action;
  }
  c.runPhAfterEcSettle = false;
  c.phRecheckPending = false;

  c.lastPhCheckMs = nowMs;
  const float capV = ControlDesign::capVolumeL(
      s.levelPercent >= 0 ? TdsNutrientObserver::volumeLFromLevelPct(s.levelPercent) : NAN);
  // pH-up's tuned gain depends on the nutrient buffer, so pass the TDS too.
  PhControl::CheckResult ph = PhControl::evaluateCheck(
      s.ph, cfg, c.ph, baselineMode, nowMs, capV, s.ecPpm);

  const char* alerts = ph.sessionCapHit   ? "ph_session_cap"
                       : ph.noResponseHit ? "ph_no_response"
                                          : "";
  logEvent("ph_check", s, "", NAN, NAN, NAN, NAN, alerts, recordedAtUtc);

  if (ph.baselineComplete) {
    setReadyToSeed(true);
    logEvent("ph_baseline_complete", s, "", NAN, NAN, s.ph, NAN, "ready_to_seed",
             recordedAtUtc);
    Serial.println("[STAGE] PH_BASELINE complete — ready to seed (dip rockwool, then MODE SEEDLING)");
    return action;
  }

  if (!ph.noAction && ph.doseMl > 0.0f) {
    action.type = (ph.direction == PhControl::UP) ? ACTION_DOSE_PH_UP : ACTION_DOSE_PH_DOWN;
    action.volumeMl = ph.doseMl;
    action.phBefore = s.ph;
    action.reason = ph.reason;
    PhControl::recordDoseStart(c.ph, ph.direction, s.ph);
    c.pendingPhBefore = s.ph;
    c.pendingPhDir = ph.direction;
    c.pendingPhDoseMl = 0.0f;  // set to the delivered volume when the pump stops
  }
  return action;
}

// True when the filtered TDS is below its dose limit or the pH is outside its
// dead limits: the readings that start a cycle (there is no chemistry timer).
inline bool chemistryOutOfLimits(const Sensors& s) {
  const Context& c = ctx();
  const GrowStage::Config& cfg = stageCfg();
  if (GrowStage::allowsEc(c.stage) && !c.nutrientPaused && !isnan(s.ecPpm) &&
      s.ecPpm >= ControlDesign::TDS_PROBE_DRY_PPM && s.ecPpm < cfg.ecDeadLow) {
    return true;
  }
  return GrowStage::allowsPh(c.stage) && !c.phPaused && !isnan(s.ph) &&
         (s.ph < cfg.phDeadLow || s.ph > cfg.phDeadHigh);
}

inline void clearCycleTriggers() {
  Context& c = ctx();
  c.manualCyclePending = false;
  c.limitCyclePending = false;
  c.chemistryAfterTopUpPending = false;
}

// Run full control cycle: level → EC (if enabled) → pH. Returns first action.
// Nutrient is dosed before pH because the nutrient concentrate shifts pH: an
// EC dose waits for TDS to settle (at most ecSettleMs), and pH is checked only
// once TDS is in range. A cycle starts on a manual CYCLE or as soon as a
// filtered reading leaves its dead limits; between doses it waits only for the
// readings to settle.
inline Action planControlCycle(const Sensors& s, unsigned long nowMs, const char* recordedAtUtc) {
  Context& c = ctx();
  if (isBusy(nowMs)) {
    return Action{};
  }

  if (c.stage == GrowStage::FILLING) {
    return Action{};
  }

  if (c.bootHoldActive) {
    if (static_cast<long>(nowMs - c.bootHoldUntilMs) < 0 && !c.manualCyclePending) {
      return Action{};
    }
    c.bootHoldActive = false;
  }

  const bool eventPending = c.manualCyclePending || c.chemistryAfterTopUpPending ||
                            c.ecRecheckPending || c.phRecheckPending ||
                            c.runPhAfterEcSettle || c.limitCyclePending;
  const bool retryHeld = c.limitRetryHold &&
                         nowMs - c.limitRetryFromMs < ControlDesign::LIMIT_CYCLE_RETRY_MS;
  if (!eventPending && !retryHeld && chemistryOutOfLimits(s)) {
    c.limitCyclePending = true;
  }
  const bool limitCycle = c.limitCyclePending;

  // A paused loop is skipped; the others still run in order (level, nutrient, pH).
  Action a;
  if (!c.levelPaused) {
    a = planLevelCheck(s, nowMs, recordedAtUtc);
  } else {
    c.refilling = false;
    c.levelHighSinceMs = 0;
  }
  if (a.type != ACTION_NONE) {
    c.limitRetryHold = false;
    return a;
  }
  if (c.activity != IDLE) {
    return Action{};  // a refill just reached the target: its mixing wait comes first
  }

  if (GrowStage::allowsEc(c.stage) && !c.nutrientPaused) {
    a = planEcCheck(s, nowMs, recordedAtUtc);
    if (a.type != ACTION_NONE) {
      // The cycle continues via runPhAfterEcSettle once the dose has mixed:
      // one nutrient dose, then pH. Clearing the triggers here stops the
      // post-settle pass from re-running the level and EC checks first.
      clearCycleTriggers();
      c.limitRetryHold = false;
      return a;
    }
  }

  a = c.phPaused ? Action{} : planPhCheck(s, nowMs, recordedAtUtc);
  if (a.type != ACTION_NONE) {
    clearCycleTriggers();
    c.limitRetryHold = false;
    return a;
  }

  clearCycleTriggers();
  if (limitCycle) {
    // Out of limits but nothing could be dosed: try again after the retry wait.
    c.limitRetryHold = true;
    c.limitRetryFromMs = nowMs;
  }
  return Action{};
}

// reachedTarget: the live level was at the refill target when the pump stopped
// (it stops early there). Otherwise, during a refill, the pump rests and the
// level is checked again; a top-up outside a refill goes to the mixing wait.
inline void onWaterTopUpFinished(bool reachedTarget) {
  Context& c = ctx();
  const unsigned long nowMs = millis();
  if (c.refilling && !reachedTarget) {
    c.activity = WAIT_TOPUP_REST;
    c.waitUntilMs = nowMs + ControlDesign::TOPUP_REST_MS;
    return;
  }
  startWaterMix(nowMs);
}

// deliveredMl: volume the pump actually delivered (calibrated rate x run time).
// The gain update and the session caps use it, not the requested volume.
inline void onEcDoseFinished(float deliveredMl) {
  const GrowStage::Config& cfg = stageCfg();
  EcControl::recordDelivered(ctx().ec, deliveredMl);
  ctx().pendingEcDoseMl = deliveredMl;
  ctx().ecDoseEndMs = millis();
  ctx().activity = WAIT_EC_SETTLE;
  // First look after one slope window; the recheck runs once TDS has settled
  // (serviceWaits), at the latest ecSettleMs after the dose.
  ctx().waitUntilMs = ctx().ecDoseEndMs + ControlDesign::SETTLE_MIN_MS;
  (void)cfg;
  ctx().ecHistN = 0;
  ctx().phHistN = 0;
}

inline void onPhDoseFinished(float deliveredMl) {
  const GrowStage::Config& cfg = stageCfg();
  Context& c = ctx();
  PhControl::recordDelivered(c.ph, deliveredMl);
  c.pendingPhDoseMl = deliveredMl;
  // Accumulate reagent volume for the TDS salt observer (eq. TDS_nutrient).
  if (c.pendingPhDir == PhControl::UP) {
    TdsNutrientObserver::addPhUpMl(deliveredMl);
  } else if (c.pendingPhDir == PhControl::DOWN) {
    TdsNutrientObserver::addPhDownMl(deliveredMl);
  }
  c.activity = WAIT_PH_SETTLE;
  const unsigned long nowMs = millis();
  c.waitUntilMs = nowMs + ControlDesign::SETTLE_MIN_MS;  // first look; recheck once settled
  c.phWaitNominalEndMs = nowMs + cfg.phSettleMs;
  c.phHistN = 0;
}

inline void onSettleComplete(const Sensors& s, const char* recordedAtUtc) {
  Context& c = ctx();
  const Activity prev = c.activity;
  c.activity = AWAIT_RECHECK;

  if (prev == WAIT_PH_SETTLE) {
    const PhControl::Direction tripped = PhControl::updateGainAfterRecheck(c.ph, s.ph);
    const char* dir = c.pendingPhDir == PhControl::UP ? "up"
                      : c.pendingPhDir == PhControl::DOWN ? "down" : "";
    // ph_before is the reading the dose was planned from, ph_after the settled one.
    Sensors before = s;
    before.ph = c.pendingPhBefore;
    logEvent("ph_recheck", before, dir, c.pendingPhDoseMl, NAN, s.ph, NAN, "", recordedAtUtc);
    if (tripped != PhControl::NONE) {
      const bool up = tripped == PhControl::UP;
      savePhNoResponse();
      logEvent("ph_no_response", s, dir, up ? c.ph.noResponseMlUp : c.ph.noResponseMlDown,
               NAN, NAN, NAN, up ? "ph_up_no_response" : "ph_down_no_response", recordedAtUtc);
      Serial.print("[WARN] ");
      Serial.print(up ? "pH-up" : "pH-down");
      Serial.print(": ");
      Serial.print(up ? c.ph.noResponseUp : c.ph.noResponseDown);
      Serial.print(" doses (");
      Serial.print(up ? c.ph.noResponseMlUp : c.ph.noResponseMlDown, 2);
      Serial.println(" mL) did not move pH - that direction is stopped. Check the pump "
                     "runs and the line delivers (PRIME), or a heavily buffered tank; "
                     "then SESSION RESET PH.");
    }
    c.pendingPhDir = PhControl::NONE;
    c.pendingPhDoseMl = 0.0f;
    c.phRecheckPending = true;
  } else if (prev == WAIT_EC_SETTLE) {
    const float ecAfter = s.ecPpm;  // total TDS (salt correction removed in 1.9.11)
    const bool ecTripped = EcControl::updateGainAfterRecheck(c.ec, ecAfter);
    logEvent("ec_recheck", s, "", NAN, c.pendingEcDoseMl, NAN, ecAfter, "", recordedAtUtc);
    if (ecTripped) {
      saveEcNoResponse();
      logEvent("ec_no_response", s, "", NAN, c.ec.noResponseMl, NAN, NAN, "nutrient_no_response",
               recordedAtUtc);
      Serial.print("[WARN] Nutrient: ");
      Serial.print(c.ec.noResponseCount);
      Serial.print(" doses (");
      Serial.print(c.ec.noResponseMl, 1);
      Serial.println(" mL) did not raise TDS - nutrient dosing stopped. Check the pump runs "
                     "and the line delivers (PRIME NUTRIENT), then SESSION RESET EC.");
      // Nothing more to wait for: pH is checked on the next pass.
      c.pendingEcDoseMl = 0.0f;
      c.nutrientPhase = false;
      c.runPhAfterEcSettle = true;
      c.activity = IDLE;
      c.waitUntilMs = 0;
      return;
    }
    c.pendingEcDoseMl = 0.0f;
    c.ecRecheckPending = true;
    if (!isnan(ecAfter) && ecAfter < stageCfg().ecDeadLow) {
      // Still below the band: another nutrient dose on the next pass, no pH yet.
      c.nutrientPhase = true;
      c.runPhAfterEcSettle = false;
      c.activity = IDLE;
      c.waitUntilMs = 0;
      return;
    }
    c.nutrientPhase = false;
    c.runPhAfterEcSettle = true;
    // The TDS has mixed, but the pH needs phSettleMs from the dose to show the
    // nutrient's acidity. Always pass through WAIT_EC_PH_MIX so the pH check
    // also waits for the acid release to stop moving pH (settle detection).
    // pH is checked once the nutrient's acidity has settled: first look one
    // slope window after the dose, at the latest phSettleMs after it.
    const unsigned long phReadyMs = c.ecDoseEndMs + ControlDesign::SETTLE_MIN_MS;
    if (GrowStage::allowsPh(c.stage)) {
      const unsigned long nowMs = millis();
      c.activity = WAIT_EC_PH_MIX;
      c.waitUntilMs = static_cast<long>(phReadyMs - nowMs) > 0 ? phReadyMs : nowMs;
      c.phWaitNominalEndMs = c.ecDoseEndMs + stageCfg().phSettleMs;
      return;
    }
  } else if (prev == WAIT_WATER_SETTLE) {
    logEvent("level_after_topup", s, "", NAN, NAN, NAN, NAN, "", recordedAtUtc);
    c.chemistryAfterTopUpPending = true;
  }

  c.activity = IDLE;
  c.waitUntilMs = 0;
}

// TDS slope (ppm/min) over the last EC_SLOPE_WINDOW_MS of samples; NAN if the
// samples do not yet span most of the window.
inline float ecSlopePpmPerMin(
    unsigned long minSpanMs = ControlDesign::EC_SLOPE_WINDOW_MS * 4 / 5) {
  const Context& c = ctx();
  if (c.ecHistN < 2) {
    return NAN;
  }
  const int newest = (c.ecHistN - 1) % 6;
  const int count = c.ecHistN < 6 ? c.ecHistN : 6;
  const int oldest = (c.ecHistN - count) % 6;
  const unsigned long span = c.ecHistMs[newest] - c.ecHistMs[oldest];
  if (span < minSpanMs) {
    return NAN;
  }
  return (c.ecHist[newest] - c.ecHist[oldest]) / (span / 60000.0f);
}

// pH slope (pH/min) over the last PH_SLOPE_WINDOW_MS of samples; NAN if too few.
inline float phSlopePerMin() {
  const Context& c = ctx();
  if (c.phHistN < 2) {
    return NAN;
  }
  const int newest = (c.phHistN - 1) % 6;
  const int count = c.phHistN < 6 ? c.phHistN : 6;
  const int oldest = (c.phHistN - count) % 6;
  const unsigned long span = c.phHistMs[newest] - c.phHistMs[oldest];
  if (span < ControlDesign::PH_SLOPE_WINDOW_MS * 4 / 5) {
    return NAN;
  }
  return (c.phHist[newest] - c.phHist[oldest]) / (span / 60000.0f);
}

inline void samplePhForSettle(unsigned long nowMs, float ph) {
  Context& c = ctx();
  if (isnan(ph)) {
    return;
  }
  const int last = (c.phHistN + 5) % 6;
  if (c.phHistN > 0 && nowMs - c.phHistMs[last] < 60000UL) {
    return;
  }
  const int slot = c.phHistN % 6;
  c.phHist[slot] = ph;
  c.phHistMs[slot] = nowMs;
  c.phHistN++;
}

inline void sampleEcForSettle(unsigned long nowMs, float ecPpm) {
  Context& c = ctx();
  if (isnan(ecPpm)) {
    return;
  }
  const int last = (c.ecHistN + 5) % 6;
  if (c.ecHistN > 0 && nowMs - c.ecHistMs[last] < 60000UL) {
    return;
  }
  const int slot = c.ecHistN % 6;
  c.ecHist[slot] = ecPpm;
  c.ecHistMs[slot] = nowMs;
  c.ecHistN++;
}

inline void serviceWaits(unsigned long nowMs, const Sensors& s, const char* recordedAtUtc) {
  Context& c = ctx();
  if (c.refillDoneLogPending) {
    c.refillDoneLogPending = false;
    static char note[64];
    snprintf(note, sizeof(note), "delivered=%.0f planned=%.0f min=%lu", c.refillDeliveredMl,
             c.refillPlannedMl, c.refillDoneMin);
    logEvent("refill_done", s, "", NAN, NAN, NAN, NAN, note, recordedAtUtc);
    Serial.print("[STAGE-CTRL] Refill done: ");
    Serial.println(note);
  }
  if (c.activity == IDLE) {
    return;
  }
  if (c.activity == WAIT_EC_SETTLE || c.activity == WAIT_WATER_SETTLE) {
    sampleEcForSettle(nowMs, s.ecPpm);
  }
  if (c.activity == WAIT_EC_SETTLE || c.activity == WAIT_EC_PH_MIX ||
      c.activity == WAIT_PH_SETTLE) {
    samplePhForSettle(nowMs, s.ph);
  }
  if (static_cast<long>(nowMs - c.waitUntilMs) < 0) {
    return;
  }
  if (c.activity == WAIT_TOPUP_REST) {
    c.activity = IDLE;  // the next pass checks the level and plans the next chunk
    c.waitUntilMs = 0;
    return;
  }
  if (c.activity == WAIT_WATER_SETTLE) {
    // Settled when TDS stops moving either way (a top-up dilutes): |slope| within
    // the larger of the ppm floor and a fraction of the reading. No TDS reading
    // (warming up, probe out) has nothing to wait for.
    const float slope = ecSlopePpmPerMin(ControlDesign::WATER_MIX_SLOPE_SPAN_MS);
    const float limit =
        isnan(s.ecPpm) ? ControlDesign::EC_SETTLED_PPM_PER_MIN
                       : fmaxf(ControlDesign::EC_SETTLED_PPM_PER_MIN,
                               ControlDesign::WATER_MIX_SETTLED_FRACTION_PER_MIN * s.ecPpm);
    const bool settled = isnan(s.ecPpm) || (!isnan(slope) && fabsf(slope) <= limit);
    const bool atLimit = nowMs - c.waterMixStartedMs >= ControlDesign::WATER_SETTLE_MS;
    if (!settled && !atLimit) {
      c.waitUntilMs = nowMs + ControlDesign::SETTLE_POLL_MS;
      return;
    }
    Serial.print(settled ? "[STAGE-CTRL] TDS settled after the top-up ("
                         : "[STAGE-CTRL] TDS still moving at the wait limit (");
    Serial.print(slope, 1);
    Serial.print(" ppm/min, ");
    Serial.print((nowMs - c.waterMixStartedMs) / 60000UL);
    Serial.println(" min) - nutrient check next");
  }
  if (c.activity == WAIT_EC_PH_MIX || c.activity == WAIT_PH_SETTLE) {
    const float slope = phSlopePerMin();
    const bool drifting = isnan(slope) || fabsf(slope) > ControlDesign::PH_SETTLED_PER_MIN;
    // Signed: negative while still inside the nominal wait (an unsigned
    // difference wrapped there and ended the wait early).
    const long pastNominal = static_cast<long>(nowMs - c.phWaitNominalEndMs);
    if (drifting && pastNominal < 0) {
      c.waitUntilMs = nowMs + ControlDesign::SETTLE_POLL_MS;  // look again in a minute
      return;
    }
    if (drifting) {
      Serial.print("[STAGE-CTRL] pH still drifting (");
      Serial.print(slope, 3);
      Serial.println(" pH/min) at the wait limit - checking anyway");
    } else if (pastNominal < 0) {
      Serial.print("[STAGE-CTRL] pH settled ");
      Serial.print((nowMs - (c.phWaitNominalEndMs - stageCfg().phSettleMs)) / 60000UL);
      Serial.println(" min after the dose - checking now");
    }
  }
  if (c.activity == WAIT_EC_SETTLE) {
    const float slope = ecSlopePpmPerMin();
    const bool rising = isnan(slope) || slope > ControlDesign::EC_SETTLED_PPM_PER_MIN;
    const long pastNominal = static_cast<long>(nowMs - (c.ecDoseEndMs + stageCfg().ecSettleMs));
    if (rising && pastNominal < 0) {
      c.waitUntilMs = nowMs + ControlDesign::SETTLE_POLL_MS;  // still mixing: look again in a minute
      return;
    }
    if (!rising && pastNominal < 0) {
      Serial.print("[STAGE-CTRL] TDS settled ");
      Serial.print((nowMs - c.ecDoseEndMs) / 60000UL);
      Serial.println(" min after the dose - rechecking now");
    }
    if (rising) {
      // Judged at the wait limit like a settled reading: below the dead limit
      // doses again, inside it hands over to pH.
      Serial.print("[STAGE-CTRL] TDS still rising (");
      Serial.print(slope, 1);
      Serial.println(" ppm/min) at the wait limit - rechecking anyway");
    }
  }
  onSettleComplete(s, recordedAtUtc);
}

}  // namespace StageController

#endif  // SMART_GARDEN_STAGE_CONTROLLER_H
