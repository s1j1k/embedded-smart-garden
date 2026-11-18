/* Solution monitor runtime: when is the nutrient solution due for a change?
 * Logic and rationale in shared/solution_monitor.h; this file feeds it,
 * saves it to LittleFS and reports it (telemetry, alerts, control events). */
#include "garden.h"
#include "shared/solution_monitor.h"
#include <time.h>

static const char* SOLUTION_PATH = "/solution.txt";
static SolutionMonitor::Saved solutionSaved;
static SolutionMonitor::Live solutionLive;
static SolutionMonitor::Result solutionResult;
static uint32_t seenResponseSeq = 0;
static uint32_t seenFillSeq = 0;
static unsigned long lastEvalMs = 0;
static bool solutionEvaluated = false;

static uint32_t clockEpoch() {
  const time_t now = time(nullptr);
  return now > 1700000000 ? static_cast<uint32_t>(now) : 0;  // 0 until NTP has set it
}

static void saveSolution() {
  File f = LittleFS.open(SOLUTION_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[SOLUTION] WARNING: could not save to LittleFS");
    return;
  }
  f.printf("%lu %.1f %.3f %.4f %d\n", static_cast<unsigned long>(solutionSaved.startEpoch),
           solutionSaved.topUpMl, solutionSaved.reagentMl,
           isnan(solutionSaved.bufferRatio) ? -1.0f : solutionSaved.bufferRatio,
           solutionSaved.bufferN);
  f.close();
}

void loadSolutionMonitor() {
  File f = LittleFS.open(SOLUTION_PATH, FILE_READ);
  if (!f) {
    return;  // never saved: the age starts once the clock is set
  }
  solutionSaved.startEpoch = static_cast<uint32_t>(f.parseInt());
  solutionSaved.topUpMl = f.parseFloat();
  solutionSaved.reagentMl = f.parseFloat();
  const float ratio = f.parseFloat();
  solutionSaved.bufferRatio = ratio < 0.0f ? NAN : ratio;
  solutionSaved.bufferN = f.parseInt();
  f.close();
}

void resetSolutionMonitor(const char* via) {
  solutionSaved = SolutionMonitor::Saved();
  solutionSaved.startEpoch = clockEpoch();
  solutionLive = SolutionMonitor::Live();
  solutionResult = SolutionMonitor::Result();
  saveSolution();
  StageController::logEvent("water_changed", buildSensorSnapshot(), "", NAN, NAN, NAN, NAN,
                            via != nullptr ? via : "", currentIsoTimestamp().c_str());
  Serial.print("[SOLUTION] Water change recorded (");
  Serial.print(via != nullptr ? via : "");
  Serial.println(") — solution indicators start from zero.");
}

static SolutionMonitor::Inputs solutionInputs(unsigned long now) {
  SolutionMonitor::Inputs in;
  const uint32_t epoch = clockEpoch();
  if (solutionSaved.startEpoch > 0 && epoch >= solutionSaved.startEpoch) {
    in.ageDays = (epoch - solutionSaved.startEpoch) / 86400.0f;
  }
  const float volumeMl =
      TdsNutrientObserver::volumeLFromLevelPct(waterLevelPercentForControl()) * 1000.0f;
  in.turnover = solutionSaved.topUpMl / volumeMl;
  in.reagentMlPerL = solutionSaved.reagentMl / (volumeMl / 1000.0f);
  in.bufferRatio = solutionSaved.bufferRatio;
  in.bufferN = solutionSaved.bufferN;
  in.phOutHours = SolutionMonitor::phOutHours(solutionLive);
  in.tdsHighHours = SolutionMonitor::tdsHighHours(solutionLive, now);
  return in;
}

void solutionOnTopUp(float deliveredMl) {
  if (isnan(deliveredMl) || deliveredMl <= 0.0f) {
    return;
  }
  solutionSaved.topUpMl += deliveredMl;
  saveSolution();
}

// Every loop pass; samples once a minute, re-evaluates every 5 min.
void serviceSolutionMonitor(unsigned long now) {
  StageController::Context& c = StageController::ctx();
  const GrowStage::Config& cfg = GrowStage::config(c.stage);

  if (c.fillSeq != seenFillSeq) {
    seenFillSeq = c.fillSeq;
    resetSolutionMonitor("refill");
  }
  // Plain water in a tank that has history: the water was changed by hand.
  if (!tdsSensorFault && !isnan(tdsPpmEma) &&
      tdsPpmEma < TdsNutrientObserver::PLAIN_WATER_PPM &&
      (solutionSaved.reagentMl > 0.0f || solutionSaved.topUpMl > 0.0f ||
       solutionSaved.bufferN > 0)) {
    resetSolutionMonitor("plain_water");
  }
  if (solutionSaved.startEpoch == 0 && clockEpoch() > 0) {
    solutionSaved.startEpoch = clockEpoch();  // first boot with this feature
    saveSolution();
  }
  if (c.ph.responseSeq != seenResponseSeq) {
    seenResponseSeq = c.ph.responseSeq;
    if (!isnan(c.ph.lastResponseRatio)) {
      SolutionMonitor::recordPhResponse(solutionSaved, c.ph.lastResponseRatio,
                                        c.ph.lastResponseMl);
      saveSolution();
    }
  }
  if (GrowStage::allowsPh(c.stage) && !phSensorFault) {
    SolutionMonitor::samplePh(solutionLive, now, phValueEma, cfg.phDeadLow, cfg.phDeadHigh);
  }
  SolutionMonitor::sampleTds(solutionLive, now,
                             GrowStage::allowsEc(c.stage) && !tdsSensorFault ? tdsPpmEma : NAN,
                             cfg.ecDeadHigh);

  if (solutionEvaluated && now - lastEvalMs < 300000UL) {
    return;
  }
  const SolutionMonitor::Level before = solutionResult.level;
  solutionResult = SolutionMonitor::evaluate(solutionInputs(now));
  lastEvalMs = now;
  if (solutionEvaluated && solutionResult.level > before &&
      solutionResult.level == SolutionMonitor::DUE) {
    char reasons[64];
    SolutionMonitor::formatReasons(solutionResult.due, reasons, sizeof(reasons));
    StageController::logEvent("water_change_due", buildSensorSnapshot(), "", NAN, NAN, NAN,
                              NAN, reasons, currentIsoTimestamp().c_str());
    Serial.print("[SOLUTION] Water change due: ");
    Serial.println(reasons);
  }
  solutionEvaluated = true;
}

void addSolutionJson(JsonDocument& doc, JsonArray alerts) {
  const SolutionMonitor::Inputs in = solutionInputs(millis());
  char due[64], watch[64];
  SolutionMonitor::formatReasons(solutionResult.due, due, sizeof(due));
  SolutionMonitor::formatReasons(solutionResult.watch, watch, sizeof(watch));
  JsonObject o = doc["solution"].to<JsonObject>();
  o["status"] = SolutionMonitor::levelName(solutionResult.level);
  o["due"] = due;
  o["watch"] = watch;
  if (!isnan(in.ageDays)) o["age_d"] = in.ageDays;
  o["turnover"] = in.turnover;
  o["reagent_ml_per_l"] = in.reagentMlPerL;
  if (!isnan(in.bufferRatio)) o["buffer_ratio"] = in.bufferRatio;
  o["buffer_n"] = in.bufferN;
  o["ph_out_h"] = in.phOutHours;
  o["tds_high_h"] = in.tdsHighHours;
  if (solutionResult.level == SolutionMonitor::DUE) {
    alerts.add("water_change_due");
  } else if (solutionResult.level == SolutionMonitor::WATCH) {
    alerts.add("water_change_soon");
  }
}

void printSolutionStatus() {
  const SolutionMonitor::Inputs in = solutionInputs(millis());
  char due[64], watch[64];
  SolutionMonitor::formatReasons(solutionResult.due, due, sizeof(due));
  SolutionMonitor::formatReasons(solutionResult.watch, watch, sizeof(watch));
  Serial.printf("[SOLUTION] %s  due[%s] watch[%s]\n",
                SolutionMonitor::levelName(solutionResult.level), due, watch);
  Serial.printf("  age %.1f d, top-up %.2f tank volumes, reagent %.2f mL/L, buffer ratio %.2f "
                "(n=%d), pH out of band %d h/24 h, TDS high %.1f h\n",
                in.ageDays, in.turnover, in.reagentMlPerL, in.bufferRatio, in.bufferN,
                in.phOutHours, in.tdsHighHours);
}
