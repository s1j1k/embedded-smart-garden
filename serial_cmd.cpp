/* Serial command handling and dose helpers. */
#include "garden.h"
#include "shared/sensor_ema.h"

void printSystemModeBanner() {
  Serial.println();
  Serial.print("========== STAGE: ");
  Serial.print(systemModeName());
  Serial.println(" ==========");
  switch (systemMode) {
    case GrowStage::FILLING:
      Serial.println("Fill tank by hand until water touches net pots, then CONFIRM.");
      Serial.print("fill_0 file: ");
      Serial.print(StageController::ctx().fill0Percent);
      Serial.println("%");
      break;
    case GrowStage::PH_BASELINE:
      Serial.println("pH baseline on plain water — no EC dosing.");
      Serial.println("If tubing is dry: PRIME PH_UP / PRIME PH_DOWN, then STOP.");
      if (StageController::ctx().readyToSeed) {
        Serial.println("Status: READY TO SEED — dip rockwool ~30 s, then MODE SEEDLING.");
      } else {
        Serial.println("Waiting for stable pH in band with re-check interval.");
      }
      break;
    case GrowStage::SEEDLING:
      Serial.println("Seedling: hold drawdown 0 mm; pH checks only (no nutrients).");
      break;
    case GrowStage::TRANSITIONAL:
      Serial.print("Transitional: same control as vegetative; EC × ");
      Serial.print(GrowStage::TRANSITIONAL_STRENGTH_FRACTION, 2);
      Serial.println(" of live vegetative target (pH unchanged).");
      break;
    case GrowStage::VEGETATIVE:
      Serial.println("Vegetative: 50 mm drawdown; EC dose then pH correction.");
      break;
    case GrowStage::HARVEST:
      Serial.println("Harvest: 100 mm drawdown; gentler EC/pH intervals.");
      break;
    case GrowStage::SENSOR_BASELINE:
      Serial.println("Sensor baseline: all probes logged every 10 s; NO pump control.");
      Serial.println("  CONFIRM — record fill_0 from current level (already filled? use this)");
      Serial.println("  BASELINE START / BASELINE END — mark capture window in Influx");
      Serial.println("  MODE SEEDLING — when transplanting / ready for hold");
      break;
    case GrowStage::IDLE:
      Serial.println("Idle: sensors + MQTT only. No pH / nutrient / level auto control.");
      Serial.println("  Set a stage: MODE SEEDLING | VEGETATIVE | HARVEST | …");
      Serial.println("  Or dashboard Overview → Grow stage → Apply (persists across reboot).");
      break;
    default:
      break;
  }
}

void stopWaterFillManualPump(const char* reason) {
  if (!waterFillManualPumpRunning) {
    return;
  }
  pump1.write(SERVO_STOP);
  const unsigned long ran = millis() - waterFillManualStartedAt;
  PumpCal::consumeRunMs(PumpCal::WATER, ran);
  waterFillManualPumpRunning = false;
  Serial.print("[FILL] Jug pump STOP (");
  Serial.print(reason);
  Serial.print(") after ");
  Serial.print(ran);
  Serial.println(" ms");
}

bool setSystemMode(GrowStage::Id next, const char* via) {
  if (next == systemMode) {
    Serial.print("[MODE] Already ");
    Serial.println(systemModeName());
    return true;
  }
  if (next != GrowStage::FILLING && waterFillManualPumpRunning) {
    stopWaterFillManualPump("mode change");
  }
  if (next == GrowStage::FILLING) {
    stopAllDosingPumps();
  }
  if (next >= GrowStage::SEEDLING && systemMode < GrowStage::PH_BASELINE) {
    Serial.println("[MODE] Tip: CONFIRM a water-fill touch first if you have not yet.");
  }
  StageController::transitionTo(next);
  Serial.print("[MODE] → ");
  Serial.print(systemModeName());
  if (via != nullptr && via[0] != '\0') {
    Serial.print(" (");
    Serial.print(via);
    Serial.print(")");
  }
  Serial.println();
  printSystemModeBanner();
  return true;
}

bool parseSystemModeToken(const String& token, GrowStage::Id& out) {
  return GrowStage::parseName(token.c_str(), out);
}

void recordBaselineFill0() {
  stopWaterFillManualPump("baseline record");
  updateWaterLevelReading(true);
  if (waterLevelPercent < 0) {
    Serial.println("[BASELINE] No trusted water level — wait for sensor, then CONFIRM.");
    return;
  }
  if (!StageController::recordFill0Only(waterLevelPercent)) {
    Serial.println("[BASELINE] Failed to record fill_0.");
    return;
  }
  SensorDataStore::saveNetPotTouchPercent(waterLevelPercent);
  SensorDataStore::recordWaterLevel(waterLevelPercent);
  netPotTouchPercent = waterLevelPercent;
  Serial.print("[BASELINE] fill_0 saved at ");
  Serial.print(waterLevelPercent);
  Serial.println("% — logging continues (no dosing)");
}

void logBaselineCaptureEvent(const char* eventName) {
  ControlEventLog::Row row;
  char recordedAtUtc[24] = "";
#if ENABLE_WIFI
  recordedAtUtcForUptime(
      millis(),
      recordedAtUtc,
      sizeof(recordedAtUtc));
#endif
  row.recordedAtUtc = recordedAtUtc;
  row.uptimeMs = millis();
  row.event = eventName;
  row.stage = systemModeName();
  row.currentLevelPct = waterLevelPercent;
  row.currentDrawdownMm =
      LevelControl::drawdownMm(fill0Percent(), waterLevelPercent);
  row.targetDepthMm = GrowStage::config(systemMode).targetDrawdownMm;
  row.phBefore = SensorEma::forControl(phValueEma, phValueFiltered);
  row.ecBefore = SensorEma::forControl(tdsPpmEma, static_cast<float>(tdsPPM));
  row.waterTempC = waterTempC;
  row.alertFlags = "baseline_capture";
  ControlEventLog::append(row);
  ControlEventLog::logToSerial(row);
  publishControlEvent(row);
}

void startBaselineCapture() {
  if (!isSensorBaselineMode()) {
    Serial.println("[BASELINE] Type MODE BASELINE first.");
    return;
  }
  recordBaselineFill0();
  if (StageController::markBaselineCaptureStart()) {
    logBaselineCaptureEvent("baseline_capture_start");
    Serial.println("[BASELINE] Capture window START marked (Influx + /baseline_capture.txt)");
  }
}

void endBaselineCapture() {
  if (!isSensorBaselineMode()) {
    Serial.println("[BASELINE] Type MODE BASELINE first.");
    return;
  }
  if (StageController::markBaselineCaptureEnd()) {
    logBaselineCaptureEvent("baseline_capture_complete");
    Serial.println("[BASELINE] Capture COMPLETE — filter Influx on event=baseline_capture_*");
  } else {
    Serial.println("[BASELINE] Run BASELINE START first.");
  }
}

void confirmWaterFillTouch() {
  if (!isWaterFillMode()) {
    Serial.println("[FILL] CONFIRM only applies in MODE FILL. Type MODE FILL first.");
    return;
  }
  stopWaterFillManualPump("confirm");
  updateWaterLevelReading(true);
  if (waterLevelPercent < 0) {
    Serial.println("[FILL] No trusted water level yet — wait for a stable reading, then CONFIRM.");
    return;
  }
  if (!StageController::confirmFillComplete(waterLevelPercent)) {
    Serial.println("[FILL] Failed to record fill_0.");
    return;
  }
  SensorDataStore::saveNetPotTouchPercent(waterLevelPercent);
  netPotTouchPercent = waterLevelPercent;
  Serial.print("[FILL] fill_0 / net-pot touch saved: ");
  Serial.print(netPotTouchPercent);
  Serial.println("%");
  printSystemModeBanner();
}

void startWaterFillManualPump() {
  Serial.println("[FILL] Jug pump is not used in MODE FILL — pour by hand.");
  Serial.println("[FILL] Watch the level printout, then CONFIRM when it touches the net pots.");
}

void serviceWaterFillMode(unsigned long now) {
  #if ENABLE_WATER_LEVEL
  if (now - lastWaterLevelPriorityPollMs >= WATER_LEVEL_PRIORITY_POLL_MS) {
    lastWaterLevelPriorityPollMs = now;
    updateWaterLevelReading(false);
  }
  #endif

  // Hand-fill only — never run the jug pump in this mode.
  if (waterFillManualPumpRunning) {
    stopWaterFillManualPump("hand-fill mode");
  }

  if (now - lastWaterFillStatusMs >= WATER_FILL_STATUS_MS) {
    lastWaterFillStatusMs = now;
    Serial.print("[FILL] mode=");
    Serial.print(systemModeName());
    Serial.print("  level=");
    if (waterLevelPercent < 0) {
      Serial.print("unavailable");
    } else {
      Serial.print(waterLevelPercent);
      Serial.print("%");
    }
    Serial.print("  touch_file=");
    Serial.print(netPotTouchPercent);
    Serial.println("%  | fill by hand → CONFIRM  (MODE for help)");
  }
}

void handleSerialLine(String line) {
  line.trim();
  if (line.length() == 0) {
    return;
  }
  String upper = line;
  upper.toUpperCase();

  if (upper == "HELP" || upper == "?") {
    printSerialHelp(true);
    return;
  }
  if (upper == "VERSION" || upper == "FW" || upper == "FIRMWARE") {
    Serial.print("[FW] firmware_version=");
    Serial.println(FIRMWARE_VERSION);
    return;
  }
  if (upper == "POWER" || upper == "BROWNOUT" || upper == "POWER STATUS") {
    PowerMonitor::printStatus();
    return;
  }
  if (upper == "MODE") {
    printSystemModeBanner();
    return;
  }
  if (upper.startsWith("MODE ")) {
    GrowStage::Id next;
    if (!parseSystemModeToken(line.substring(5), next)) {
      Serial.println("[MODE] Use MODE FILL | PRIMING | SEEDLING | TRANSITIONAL | VEGETATIVE | HARVEST");
      return;
    }
    setSystemMode(next, "serial");
    return;
  }
  if (upper.startsWith("SETPOINTS")) {
    // Same write as MQTT {"cmd":"setpoints"} / the dashboard plant catalogue.
    // ppm500 EC band + pH band, vegetative only (transitional EC tracks it).
    // Optional: pH dead limits and setpoint (else setpoint +/- 0.5 pH).
    String rest = (line.length() > 9) ? line.substring(9) : "";
    rest.trim();
    float ecMin = NAN;
    float ecMax = NAN;
    float phMin = NAN;
    float phMax = NAN;
    float phDeadLow = NAN;
    float phDeadHigh = NAN;
    float phSetpoint = NAN;
    const int n = sscanf(rest.c_str(), "%f %f %f %f %f %f %f", &ecMin, &ecMax, &phMin,
                         &phMax, &phDeadLow, &phDeadHigh, &phSetpoint);
    if (n < 4 ||
        !GrowStage::applyChemistryBands(GrowStage::VEGETATIVE, ecMin, ecMax, phMin, phMax,
                                        NAN, NAN, phDeadLow, phDeadHigh, phSetpoint)) {
      Serial.println(
          "[SETPOINTS] Usage: SETPOINTS <ec_min_ppm> <ec_max_ppm> <ph_min> <ph_max> "
          "[ph_dead_low ph_dead_high [ph_setpoint]]");
      return;
    }
    StageController::saveChemistryBands(GrowStage::VEGETATIVE);
    const GrowStage::Config& cfg = GrowStage::config(GrowStage::VEGETATIVE);
    Serial.print("[SETPOINTS] vegetative EC ");
    Serial.print(cfg.ecTargetMin, 1);
    Serial.print("-");
    Serial.print(cfg.ecTargetMax, 1);
    Serial.print(" ppm (dead ");
    Serial.print(cfg.ecDeadLow, 1);
    Serial.print("-");
    Serial.print(cfg.ecDeadHigh, 1);
    Serial.print(") pH ");
    Serial.print(cfg.phTargetMin, 2);
    Serial.print("-");
    Serial.print(cfg.phTargetMax, 2);
    Serial.print(" (dead ");
    Serial.print(cfg.phDeadLow, 2);
    Serial.print("-");
    Serial.print(cfg.phDeadHigh, 2);
    Serial.println(")");
    return;
  }
  if (upper.startsWith("FILL0")) {
    // Persist net-pot touch without MODE FILL (that path resets chemistry
    // and leaves vegetative). FILL0 65
    String rest = (line.length() > 5) ? line.substring(5) : "";
    rest.trim();
    const int pct = rest.toInt();
    if (rest.length() == 0 || pct < 0 || pct > 100 ||
        !StageController::saveFill0(pct)) {
      Serial.println("[FILL0] Usage: FILL0 <percent 0-100>");
      return;
    }
    SensorDataStore::saveNetPotTouchPercent(pct);
    netPotTouchPercent = pct;
    Serial.print("[FILL0] net-pot touch / fill_0 saved at ");
    Serial.print(pct);
    Serial.println("%");
    return;
  }
  if (upper == "FLASH" || upper.startsWith("FLASH ")) {
    #if ENABLE_GROW_LIGHT
    if (upper == "FLASH OFF") {
      GrowLight::endFlash();
    } else {
      const long ms = upper.length() > 6 ? upper.substring(6).toInt() : 3000;
      GrowLight::startFlash(ms > 0 ? static_cast<unsigned long>(ms) : 3000UL);
    }
    #else
    Serial.println("[LIGHT] Grow light disabled in firmware");
    #endif
    return;
  }
  if (upper == "LIGHT MAKEUP" || upper.startsWith("LIGHT MAKEUP ")) {
    // LIGHT MAKEUP [ON|OFF|CLEAR|STATUS]: repay light missed while off.
    String action = upper.length() > 13 ? upper.substring(13) : String("ON");
    action.trim();
    action.toLowerCase();
    char msg[160];
    lightMakeupCommand(action.c_str(), "serial", msg, sizeof(msg));
    Serial.print("[LIGHT] ");
    Serial.println(msg);
    return;
  }
  if (upper == "LIGHT ON" || upper.startsWith("LIGHT ON ") || upper == "LIGHT AUTO") {
    // LIGHT ON [hours]: on now for that long (default: the owed make-up light).
    // LIGHT AUTO: end it, back to the schedule.
    const float hours = upper == "LIGHT AUTO" ? -1.0f
                        : upper.length() > 9   ? upper.substring(9).toFloat()
                                               : 0.0f;
    char msg[160];
    lightOnCommand(hours, "serial", msg, sizeof(msg));
    Serial.print("[LIGHT] ");
    Serial.println(msg);
    return;
  }
  if (upper == "CYCLE" || upper == "TRIGGER") {
    StageController::requestManualCycle();
    Serial.println("[STAGE] Manual control cycle requested.");
    return;
  }
  if (upper == "CYCLE LEVEL" || upper == "CYCLE NUTRIENT" || upper == "CYCLE PH") {
    String loop = upper.substring(6);
    loop.toLowerCase();
    StageController::requestLoopRun(loop.c_str());
    Serial.print("[STAGE] ");
    Serial.print(loop);
    Serial.println(" loop run requested.");
    return;
  }
  if (upper == "PAUSE LEVEL" || upper == "PAUSE NUTRIENT" || upper == "PAUSE PH" ||
      upper == "RESUME LEVEL" || upper == "RESUME NUTRIENT" || upper == "RESUME PH") {
    const bool paused = upper.startsWith("PAUSE");
    String loop = upper.substring(paused ? 6 : 7);
    loop.toLowerCase();
    setLoopPaused(loop.c_str(), paused, "serial");
    return;
  }
  if (upper == "LOOPS") {
    const StageController::Context& c = StageController::ctx();
    Serial.print("[LOOP] level ");
    Serial.print(c.levelPaused ? "PAUSED" : "running");
    Serial.print(", nutrient ");
    Serial.print(c.nutrientPaused ? "PAUSED" : "running");
    Serial.print(", pH ");
    Serial.print(c.phPaused ? "PAUSED" : "running");
    Serial.println(autoControlPaused ? " (all held: AUTO PAUSE)" : "");
    return;
  }
  if (upper == "REFILL PROCEED" || upper == "REFILL OK") {
    proceedRefill("serial");
    return;
  }
  if (upper == "REFILL" || upper == "REFILL STATUS") {
    const StageController::Context& c = StageController::ctx();
    Serial.print("[REFILL] ");
    Serial.print(c.refillHold ? "ON HOLD (" : c.refilling ? "running" : "idle");
    if (c.refillHold) {
      Serial.print(c.refillHoldReason);
      Serial.print(")");
    }
    Serial.print(" level ");
    Serial.print(waterLevelPercentForControl());
    Serial.print(" % (spread ");
    Serial.print(waterLevelSpreadPercent());
    Serial.print(" %), target ");
    Serial.print(StageController::refillTargetPercent());
    Serial.print(" %, delivered ");
    Serial.print(c.refillDeliveredMl, 0);
    Serial.print(" / ");
    Serial.print(c.refillPlannedMl, 0);
    Serial.print(" mL, tank gain ");
    Serial.print(c.mlPerMmEst, 0);
    Serial.println(" mL/mm");
    return;
  }
  if (upper == "AUTO" || upper == "AUTO STATUS") {
    Serial.print("[AUTO] ");
    Serial.print(autoControlPaused ? "PAUSED" : "running");
    Serial.print(" — stage ");
    Serial.println(systemModeName());
    return;
  }
  if (upper == "AUTO PAUSE" || upper == "PAUSE") {
    setAutoControlPaused(true, "serial");
    return;
  }
  if (upper == "AUTO RESUME" || upper == "RESUME") {
    setAutoControlPaused(false, "serial");
    return;
  }
  if (upper == "SOLUTION" || upper == "SOLUTION STATUS") {
    printSolutionStatus();
    return;
  }
  if (upper == "WATER CHANGED" || upper == "SOLUTION RESET") {
    resetSolutionMonitor("serial");
    return;
  }
  if (upper.startsWith("SESSION RESET")) {
    const bool all = upper.endsWith(" ALL");
    const bool ph = all || upper.endsWith(" PH");
    const bool ec = all || upper.endsWith(" EC") || upper.endsWith(" NUTRIENT");
    if (!ph && !ec) {
      Serial.println("[DOSE] Usage: SESSION RESET PH | EC | ALL");
      return;
    }
    resetDosingSession(ph, ec, "serial");
    return;
  }
  if (upper.startsWith("DOSE ")) {
    String rest = line.substring(5);
    rest.trim();
    const int sp = rest.indexOf(' ');
    if (sp <= 0) {
      Serial.println("[DOSE] Usage: DOSE PH_UP|PH_DOWN|NUTRIENT|WATER <ml>");
      return;
    }
    String pumpTok = rest.substring(0, sp);
    String mlTok = rest.substring(sp + 1);
    pumpTok.trim();
    mlTok.trim();
    const PumpCal::Id pid = PumpCal::idFromName(pumpTok.c_str());
    float ml = NAN;
    if (!parseVolumeMl(mlTok, ml)) {
      Serial.println("[DOSE] Could not parse mL. Example: DOSE PH_UP 0.15");
      return;
    }
    startOperatorDose(pid, ml, "serial");
    return;
  }
  if (upper == "ENTERPH" || upper == "PH ENTER" || upper == "PHCAL ENTER") {
    PhProbeCal::enter();
    return;
  }
  if (upper == "EXITPH" || upper == "PH EXIT" || upper == "PHCAL SAVE") {
    PhProbeCal::save();
    return;
  }
  if (upper == "PHABORT" || upper == "PH ABORT" || upper == "PHCAL ABORT") {
    PhProbeCal::abortSession();
    return;
  }
  if (upper == "PHCLEAR" || upper == "PH CLEAR") {
    PhProbeCal::clearStore();
    Serial.println("[PH-CAL] Cleared — restored the default (provisional 2026-10-11) cal.");
    return;
  }
  if (upper == "PHRESTORE" || upper == "PH RESTORE") {
    PhProbeCal::restoreBreadboardCalibration();
    return;
  }
  if (upper == "PHSHOW" || upper == "PH STATUS" || upper == "PHCAL") {
    #if ENABLE_DS18B20
    waterTempC = readDS18B20Average();  // watch the buffer temperature settle
    #endif
    readPhSensorValues();
    PhProbeCal::printStatus(phVoltage, waterTempC);
    return;
  }
  if (upper.startsWith("CALPH ")) {
    String point = line.substring(6);
    point.trim();
    // Optional hand-measured buffer °C: CALPH ACID 21.5 (overrides DS18B20).
    float bufferTempC = NAN;
    const int sp = point.indexOf(' ');
    if (sp > 0) {
      String tempArg = point.substring(sp + 1);
      tempArg.trim();
      point = point.substring(0, sp);
      bufferTempC = tempArg.toFloat();
      if (tempArg.length() == 0 || (bufferTempC == 0.0f && tempArg[0] != '0')) {
        Serial.println("[PH-CAL] Usage: CALPH ACID|ALKALI [buffer C], e.g. CALPH ACID 21.5");
        return;
      }
    }
    #if ENABLE_DS18B20
    // Fresh reading of the DS18B20 sitting in the buffer.
    waterTempC = readDS18B20Average();
    #endif
    if (isnan(bufferTempC)) {
      bufferTempC = waterTempC;
    }
    // Dense average for the capture itself (not the last telemetry sample).
    waitForTdsQuiet();
    const float boardV = AnalogFilter::readGpioVoltsAverage(
                             PH_PIN, AnalogFilter::CAL_SAMPLES, 5) *
                         PH_SIGNAL_LEVEL_SHIFT_GAIN;
    phVoltage = boardV;
    phValue = convertVoltageToPH(boardV);
    phValueFiltered = phValue;
    PhProbeCal::capturePoint(point.c_str(), boardV, NAN, bufferTempC);
    return;
  }
  if (upper == "CALPH") {
    Serial.println("[PH-CAL] Usage: CALPH ACID | CALPH ALKALI [buffer C]  (after ENTERPH)");
    return;
  }
  if (upper == "ENTERTDS" || upper == "TDS ENTER" || upper == "TDSCAL ENTER") {
    TdsProbeCal::enter();
    return;
  }
  if (upper == "EXITTDS" || upper == "TDS EXIT" || upper == "TDSCAL SAVE") {
    TdsProbeCal::save();
    return;
  }
  if (upper == "TDSABORT" || upper == "TDS ABORT" || upper == "TDSCAL ABORT") {
    TdsProbeCal::abortSession();
    return;
  }
  if (upper == "TDSCLEAR" || upper == "TDS CLEAR") {
    TdsProbeCal::clearStore();
    Serial.println("[TDS-CAL] Cleared — kValue=1.0 restored.");
    return;
  }
  if (upper == "TDSSHOW" || upper == "TDS STATUS" || upper == "TDSCAL") {
    #if ENABLE_DS18B20
    waterTempC = readDS18B20Average();
    #endif
    tdsADC = readTdsAdcPowered();
    const float tdsV = tdsADC * 3.3f / 4095.0f;
    tdsPPM = convertTDSToPPM(tdsADC);
    TdsProbeCal::printStatus(tdsV, waterTempC);
    return;
  }
  if (upper == "TDSOBS" || upper == "TDS OBS" || upper == "TDSNUTRIENT") {
    TdsNutrientObserver::printStatus(static_cast<float>(tdsPPM), waterLevelPercent);
    return;
  }
  if (upper == "RESETPHSALT" || upper == "TDS RESETSALT") {
    TdsNutrientObserver::resetAccumulators();
    Serial.println("[TDS-OBS] pH-reagent salt accumulators cleared.");
    return;
  }
  if (upper.startsWith("TDSCORR ")) {
    String arg = upper.substring(8);
    arg.trim();
    if (arg == "ON" || arg == "1" || arg == "TRUE") {
      TdsNutrientObserver::setLiveCorrection(false);
      Serial.println("[TDS-OBS] Salt correction was removed in 1.9.11: control uses the total TDS.");
    } else if (arg == "OFF" || arg == "0" || arg == "FALSE") {
      TdsNutrientObserver::setLiveCorrection(false);
      Serial.println("[TDS-OBS] Live EC correction OFF (raw TDS).");
    } else {
      Serial.println("[TDS-OBS] Usage: TDSCORR ON|OFF");
    }
    return;
  }
  if (upper.startsWith("SETKAPPA ")) {
    // SETKAPPA UP 80   or   SETKAPPA DOWN 60
    String rest = line.substring(9);
    rest.trim();
    String restU = rest;
    restU.toUpperCase();
    int sp = restU.indexOf(' ');
    if (sp <= 0) {
      Serial.println("[TDS-OBS] Usage: SETKAPPA UP|DOWN <ppm_L_per_mL>");
      return;
    }
    String dir = restU.substring(0, sp);
    dir.trim();
    const float val = rest.substring(sp + 1).toFloat();
    if (isnan(val) || val < 0.0f) {
      Serial.println("[TDS-OBS] κ must be >= 0");
      return;
    }
    if (dir == "UP") {
      TdsNutrientObserver::setKappa(val, NAN, true);
      Serial.print("[TDS-OBS] kappa_up set to ");
      Serial.println(val, 2);
    } else if (dir == "DOWN") {
      TdsNutrientObserver::setKappa(NAN, val, true);
      Serial.print("[TDS-OBS] kappa_down set to ");
      Serial.println(val, 2);
    } else {
      Serial.println("[TDS-OBS] Usage: SETKAPPA UP|DOWN <ppm_L_per_mL>");
    }
    return;
  }
  if (upper.startsWith("CALTDS ")) {
    String ppmTok = line.substring(7);
    ppmTok.trim();
    const float knownPpm = ppmTok.toFloat();
    #if ENABLE_DS18B20
    waterTempC = readDS18B20Average();
    #endif
    const float tdsV = readTdsGpioVoltsPowered(AnalogFilter::CAL_SAMPLES);
    tdsADC = static_cast<int>(tdsV * 4095.0f / 3.3f + 0.5f);
    tdsPPM = convertTDSToPPM(tdsADC);
    TdsProbeCal::capturePoint(tdsV, waterTempC, knownPpm);
    return;
  }
  if (upper == "CALTDS") {
    Serial.println("[TDS-CAL] Usage: CALTDS <ppm>  e.g. CALTDS 707  (after ENTERTDS)");
    return;
  }
  if (upper == "CONFIRM" || upper == "FILLED" || upper == "TOUCH") {
    if (isSensorBaselineMode()) {
      recordBaselineFill0();
    } else {
      confirmWaterFillTouch();
    }
    return;
  }
  if (upper == "BASELINE START" || upper == "BASELINE STARTED") {
    startBaselineCapture();
    return;
  }
  if (upper == "BASELINE END" || upper == "BASELINE DONE" || upper == "BASELINE STOP") {
    endBaselineCapture();
    return;
  }
  if (upper == "INFLUX" || upper == "INFLUX STATUS") {
    InfluxCloud::printSetupHint();
    Serial.print("[INFLUX] WiFi ");
    Serial.println(WiFi.status() == WL_CONNECTED ? "connected" : "NOT connected");
#if ENABLE_WIFI
    Serial.print("[INFLUX] NTP ");
    Serial.println(ntpTimeSynced ? "ok" : "not synced (points may land in 1970)");
#endif
    InfluxCloud::printStats();
    Serial.println("[INFLUX] Expect 'uploaded N queued line(s)' within a few seconds of each sensor cycle.");
    return;
  }
  if (upper == "INFLUX TEST") {
    InfluxCloud::printSetupHint();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[INFLUX] TEST aborted — WiFi not connected");
      return;
    }
    const NtpMap ntp = ntpSnapshot();
    InfluxCloud::uploadTestPoint(ntp.valid, ntp.epochAtSync, ntp.millisAtSync);
    return;
  }
#if ENABLE_WIFI
  if (upper == "NET" || upper == "NET STATUS" || upper == "MQTT" || upper == "MQTT STATUS") {
    Comms::printStatus();
    return;
  }
#endif
#if ENABLE_MQTT
  if (upper == "MQTT CONNECT" || upper == "MQTT RECONNECT" ||
      upper == "MQTT RETRY" || upper == "MQTTCONNECT") {
    Comms::requestConnect("serial");
    return;
  }
#endif
  if (upper == "DUMP STOP") {
    stopSerialDump();
    return;
  }
  if (upper.startsWith("DUMP")) {
    // DUMP <SENSORS|CSV|CTRL|EVENTS|CONTROL|PREV|OUTBOX> [RAW] [FROM <line>]
    const char* path = nullptr;
    if (upper.startsWith("DUMP SENSORS") || upper.startsWith("DUMPSENSORS") ||
        upper.startsWith("DUMP CSV")) {
      path = CSV_PATH;
    } else if (upper.startsWith("DUMP CTRL") || upper.startsWith("DUMPCTRL") ||
               upper.startsWith("DUMP EVENTS") || upper.startsWith("DUMP CONTROL")) {
      path = ControlEventLog::PATH;
    } else if (upper.startsWith("DUMP PREV")) {
      path = CSV_PREV_PATH;
    } else if (upper.startsWith("DUMP OUTBOX")) {
      path = SensorDataStore::MQTT_OUTBOX_PATH;
    }
    if (path == nullptr) {
      Serial.println("[DUMP] Use DUMP SENSORS | CTRL | PREV | OUTBOX [RAW] [FROM <line>], or DUMP STOP");
      return;
    }
    uint32_t fromLine = 1;
    const int fromAt = upper.indexOf(" FROM ");
    if (fromAt >= 0) {
      fromLine = static_cast<uint32_t>(upper.substring(fromAt + 6).toInt());
    }
    startSerialDump(path, upper.indexOf(" RAW") >= 0, fromLine);
    return;
  }
  if (upper == "START" && isWaterFillMode() && !pumpCalibrationActive()) {
    startWaterFillManualPump();
    return;
  }
  if (upper == "STOCK" || upper == "LEVELS") {
    PumpCal::printStock();
    return;
  }
  if (upper == "FILL") {
    PumpCal::refillAll();
    Serial.println("[STOCK] All bottles marked full.");
    PumpCal::printStock();
    return;
  }
  if (upper.startsWith("FILL ")) {
    PumpCal::Id id = PumpCal::idFromName(line.substring(5).c_str());
    if (id == PumpCal::COUNT) {
      Serial.println("[STOCK] Unknown bottle. Use FILL WATER / NUTRIENT / PH_UP / PH_DOWN.");
      return;
    }
    PumpCal::refill(id);
    Serial.print("[STOCK] ");
    Serial.print(PumpCal::name(id));
    Serial.println(" marked full.");
    PumpCal::printStock();
    return;
  }
  if (upper.startsWith("SETCAP ") || upper.startsWith("SETREM ")) {
    const bool isCap = upper.startsWith("SETCAP ");
    String rest = line.substring(isCap ? 7 : 7);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp <= 0) {
      Serial.println(isCap ? "[STOCK] Usage: SETCAP WATER 1000"
                           : "[STOCK] Usage: SETREM WATER 800");
      return;
    }
    PumpCal::Id id = PumpCal::idFromName(rest.substring(0, sp).c_str());
    float ml = rest.substring(sp + 1).toFloat();
    if (id == PumpCal::COUNT || ml <= 0.0f) {
      Serial.println("[STOCK] Unknown bottle or bad mL value.");
      return;
    }
    if (isCap) {
      PumpCal::setCapacity(id, ml);
      Serial.print("[STOCK] ");
      Serial.print(PumpCal::name(id));
      Serial.print(" capacity set to ");
      Serial.print(PumpCal::capacityMl(id), 0);
      Serial.println(" mL");
    } else {
      PumpCal::setRemaining(id, ml);
      Serial.print("[STOCK] ");
      Serial.print(PumpCal::name(id));
      Serial.print(" remaining set to ");
      Serial.print(PumpCal::remainingMl(id), 1);
      Serial.println(" mL");
    }
    PumpCal::printStock();
    return;
  }
  if (upper == "CAL SHOW" || upper == "CALIBRATE SHOW") {
    PumpCal::printRates();
    return;
  }
  if (upper == "CAL CLEAR") {
    PumpCal::clear();
    return;
  }
  if (upper.startsWith("CAL PULSE ")) {
    Serial.println("[CAL] Timed pulses are gone. YES runs 500 ms bursts; STOP when readable.");
    return;
  }
  if (upper == "PRIME" || upper == "PRIME WATER" || upper == "PRIME NUTRIENT" ||
      upper == "PRIME PH_UP" || upper == "PRIME PH_DOWN" ||
      upper.startsWith("PRIME ")) {
    if (upper == "PRIME") {
      if (softPrimeActive()) {
        Serial.println("[PRIME] Already soft-priming. Type STOP when liquid comes out.");
        return;
      }
      if (calPhase != CAL_WAIT_READY && calPhase != CAL_IDLE) {
        Serial.println("[CAL] Type STOP first, or ABORT.");
        return;
      }
      if (calPhase == CAL_IDLE) {
        Serial.println("[PRIME] Soft-prime (pauses auto dosing only):");
        Serial.println("        PRIME PH_UP | PRIME PH_DOWN | PRIME WATER | PRIME NUTRIENT");
        Serial.println("[PRIME] During CAL, type PRIME alone to prime the current pump.");
        return;
      }
      startPrime();
      return;
    }

    PumpCal::Id id = PumpCal::idFromName(line.substring(6).c_str());
    if (id == PumpCal::COUNT) {
      Serial.println("[PRIME] Unknown pump. Use PRIME WATER / NUTRIENT / PH_UP / PH_DOWN.");
      return;
    }
    if (calPhase == CAL_IDLE) {
      startManualPrime(id);
      return;
    }
    if (calPhase != CAL_WAIT_READY) {
      Serial.println("[CAL] Type STOP first, or ABORT.");
      return;
    }
    calPump = id;
    startPrime();
    return;
  }
  if (upper == "STOP") {
    if (softPrimeActive()) {
      finishManualPrime("liquid at outlet", true);
    } else if (calPhase == CAL_PRIMING) {
      finishPrime("liquid at outlet");
    } else if (calPhase == CAL_RUNNING && calIsContinuous() && calTargetMs == 0) {
      calStopRequested = true;
      Serial.println("[CAL] STOP noted — finishing this continuous run.");
    } else if (calPhase == CAL_RUNNING && !calIsContinuous() && calTargetBursts == 0) {
      calStopRequested = true;
      Serial.println("[CAL] STOP noted — finishing after this burst.");
    } else if (calPhase == CAL_RUNNING) {
      Serial.println("[CAL] Confirm run stopped early. Empty the cup and type RETRY.");
      if (calIsContinuous()) {
        calPulseMs = millis() - calRunStartedAt;
      }
      finishCalMeasure();
    } else if (waterFillManualPumpRunning) {
      stopWaterFillManualPump("operator");
    } else {
      Serial.println("[STOP] Nothing is running. (PRIME PH_UP/PH_DOWN; FILL: START; CAL: PRIME/YES)");
    }
    return;
  }
  if (upper == "CAL" || upper == "CALIBRATE") {
    beginCalSequence(true, PumpCal::WATER);
    return;
  }
  if (upper.startsWith("CAL ")) {
    PumpCal::Id id = PumpCal::idFromName(line.substring(4).c_str());
    if (id == PumpCal::COUNT) {
      Serial.println("[CAL] Unknown pump. Use WATER, NUTRIENT, PH_UP, PH_DOWN.");
      return;
    }
    beginCalSequence(false, id);
    return;
  }
  if (upper == "ABORT" || upper == "CANCEL") {
    if (softPrimeActive()) {
      finishManualPrime("aborted", true);
      return;
    }
    if (calPhase == CAL_IDLE) {
      Serial.println("[CAL] Nothing to abort.");
      return;
    }
    stopCalPump(calPump);
    finishCalSession();
    return;
  }
  if (upper == "SKIP") {
    if (calPhase == CAL_IDLE) {
      return;
    }
    stopCalPump(calPump);
    Serial.print("[CAL] Skipped ");
    Serial.println(PumpCal::name(calPump));
    advanceCalSequence();
    return;
  }
  if (upper == "RETRY") {
    if (calPhase == CAL_WAIT_VOLUME || calPhase == CAL_WAIT_READY) {
      startCalBurstRun();
    }
    return;
  }
  if (upper == "YES" || upper == "Y") {
    if (calPhase == CAL_WAIT_READY) {
      startCalBurstRun();
    } else if (calPhase == CAL_PRIMING) {
      Serial.println("[CAL] Still priming. Type STOP when liquid comes out, then YES.");
    } else if (calPhase == CAL_IDLE) {
      Serial.println("[CAL] Type CAL first. Prime with PRIME if the line is dry, then YES.");
    }
    return;
  }

  if (calPhase == CAL_WAIT_VOLUME) {
    float ml = 0.0f;
    if (parseVolumeMl(line, ml)) {
      acceptCalVolume(ml);
      return;
    }
  }

  Serial.print("[CAL] Unknown command: ");
  Serial.println(line);
  Serial.println("[CAL] Type HELP");
}

bool serviceSerial(bool systemOn) {
  bool ignored = false;
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    const bool linkLine = serialLineAcc.length() > 0 && serialLineAcc[0] == '@';
    if (c == '\n') {
      if (linkLine) {
#if ENABLE_WIFI
        Comms::handleSerialLinkLine(serialLineAcc.c_str());
#endif
      } else if (systemOn) {
        handleSerialLine(serialLineAcc);
      } else if (serialLineAcc.length() > 0) {
        ignored = true;
      }
      serialLineAcc = "";
    } else if (serialLineAcc.length() < (linkLine ? SERIAL_LINK_LINE_MAX : 80)) {
      serialLineAcc += c;
    }
  }
  return ignored;
}

/************************************************************
 * Non-blocking DUMP (on request only)
 *
 * The old DUMP streamed the whole file in one call, so a 1.4 MB journal
 * held the loop for ~2 min while pump timers went unserviced. Now each
 * loop pass writes a few whole lines, and only while the TX ring buffer has
 * room. Log lines from either task may land between dump lines, so each
 * line is framed for the host (scripts/serial_dump.py):
 *
 *   [DUMP] BEGIN <path> (<bytes> bytes) from=<n> format=framed
 *   ~<line no>:<crc16 hex>:<file line>
 *   [DUMP] END <path> lines=<emitted> last=<line no> crc32=<hex>
 *
 * A missing number or bad CRC means a lost or garbled line; ask again with
 * DUMP <file> FROM <n>. RAW prints the file lines unframed (old format,
 * still non-blocking).
 ************************************************************/
namespace {

struct SerialDump {
  File file;
  bool active = false;
  bool raw = false;
  uint32_t fromLine = 1;
  uint32_t lineNo = 0;
  uint32_t emitted = 0;
  uint32_t crc32 = 0xFFFFFFFFUL;
  char path[40] = "";
  char line[1024];
};
SerialDump dumpState;

uint16_t crc16Ccitt(const char* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(static_cast<uint8_t>(data[i])) << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

uint32_t crc32Update(uint32_t crc, const char* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint8_t>(data[i]);
    for (int b = 0; b < 8; b++) {
      crc = (crc & 1UL) ? (crc >> 1) ^ 0xEDB88320UL : crc >> 1;
    }
  }
  return crc;
}

// Next file line into dumpState.line (no line ending). false at EOF.
bool readDumpLine(size_t& len) {
  len = 0;
  int c = -1;
  bool any = false;
  while ((c = dumpState.file.read()) >= 0) {
    any = true;
    if (c == '\n') {
      break;
    }
    if (c != '\r' && len < sizeof(dumpState.line) - 1) {
      dumpState.line[len++] = static_cast<char>(c);
    }
  }
  dumpState.line[len] = '\0';
  return any;
}

void finishSerialDump(const char* why) {
  if (dumpState.file) {
    dumpState.file.close();
  }
  dumpState.active = false;
  Serial.print("[DUMP] END ");
  Serial.print(dumpState.path);
  Serial.print(" lines=");
  Serial.print(dumpState.emitted);
  Serial.print(" last=");
  Serial.print(dumpState.lineNo);
  Serial.print(" crc32=");
  char hex[9];
  snprintf(hex, sizeof(hex), "%08lx",
           static_cast<unsigned long>(dumpState.crc32 ^ 0xFFFFFFFFUL));
  Serial.print(hex);
  if (why != nullptr) {
    Serial.print(" ");
    Serial.print(why);
  }
  Serial.println();
}

}  // namespace

void startSerialDump(const char* path, bool raw, uint32_t fromLine) {
  if (dumpState.active) {
    finishSerialDump("(replaced by a new DUMP)");
  }
  if (path == nullptr || !LittleFS.exists(path)) {
    Serial.print("[DUMP] no file yet: ");
    Serial.println(path != nullptr ? path : "");
    return;
  }
  dumpState.file = LittleFS.open(path, FILE_READ);
  if (!dumpState.file) {
    Serial.print("[DUMP] open failed: ");
    Serial.println(path);
    return;
  }
  strncpy(dumpState.path, path, sizeof(dumpState.path) - 1);
  dumpState.path[sizeof(dumpState.path) - 1] = '\0';
  dumpState.raw = raw;
  dumpState.fromLine = fromLine < 1 ? 1 : fromLine;
  dumpState.lineNo = 0;
  dumpState.emitted = 0;
  dumpState.crc32 = 0xFFFFFFFFUL;
  dumpState.active = true;
  Serial.print("[DUMP] BEGIN ");
  Serial.print(path);
  Serial.print(" (");
  Serial.print(dumpState.file.size());
  Serial.print(" bytes) from=");
  Serial.print(dumpState.fromLine);
  Serial.println(raw ? " format=raw" : " format=framed");
}

void stopSerialDump() {
  if (dumpState.active) {
    finishSerialDump("(stopped)");
  }
}

void serviceSerialDump() {
  if (!dumpState.active) {
    return;
  }
  // A few lines per pass; never block on a full TX buffer.
  for (int i = 0; i < SERIAL_DUMP_LINES_PER_PASS; i++) {
    if (Serial.availableForWrite() < static_cast<int>(sizeof(dumpState.line) / 2)) {
      return;
    }
    size_t len = 0;
    if (!readDumpLine(len)) {
      finishSerialDump(nullptr);
      return;
    }
    dumpState.lineNo++;
    if (dumpState.lineNo < dumpState.fromLine) {
      continue;
    }
    dumpState.crc32 = crc32Update(dumpState.crc32, dumpState.line, len);
    dumpState.crc32 = crc32Update(dumpState.crc32, "\n", 1);
    dumpState.emitted++;
    if (dumpState.raw) {
      Serial.write(reinterpret_cast<const uint8_t*>(dumpState.line), len);
      Serial.write('\n');
      continue;
    }
    // One write call per framed line so another task cannot split it.
    char frame[sizeof(dumpState.line) + 24];
    const int head = snprintf(frame, sizeof(frame), "~%lu:%04x:",
                              static_cast<unsigned long>(dumpState.lineNo),
                              crc16Ccitt(dumpState.line, len));
    if (head <= 0) {
      continue;
    }
    size_t n = static_cast<size_t>(head);
    const size_t room = sizeof(frame) - n - 1;
    const size_t copy = len < room ? len : room;
    memcpy(frame + n, dumpState.line, copy);
    n += copy;
    frame[n++] = '\n';
    Serial.write(reinterpret_cast<const uint8_t*>(frame), n);
  }
}

unsigned long plannedRunMs(PumpCal::Id id, float doseMl, unsigned long fallbackMs,
                           unsigned long maxMs) {
  if (ACTIVE_CONTROL_LAW == ControlLaw::THRESHOLD || !PumpCal::isCalibrated(id)) {
    if (ACTIVE_CONTROL_LAW != ControlLaw::THRESHOLD && !PumpCal::isCalibrated(id)) {
      Serial.print("[WARN] ");
      Serial.print(PumpCal::name(id));
      Serial.println(" is not calibrated — using threshold pulse. Type CAL.");
    }
    return fallbackMs;
  }
  return PumpCal::runMsForVolume(id, doseMl, fallbackMs, PUMP_MIN_RUN_MS, maxMs);
}

int plannedBursts(PumpCal::Id id, float doseMl, int fallbackBursts, int maxBursts) {
  if (!PumpCal::isCalibrated(id)) {
    Serial.print("[WARN] ");
    Serial.print(PumpCal::name(id));
    Serial.println(" is not calibrated — using one 500 ms burst. Type CAL.");
    return fallbackBursts < 1 ? 1 : fallbackBursts;
  }
  return PumpCal::burstsForVolume(id, doseMl, fallbackBursts, maxBursts);
}

unsigned long plannedDoseMs(PumpCal::Id id, float doseMl, int maxBursts) {
  const int capBursts = maxBursts < 1 ? 1 : maxBursts;
  const unsigned long capMs =
      static_cast<unsigned long>(capBursts) * PumpCal::BURST_MS;
  if (!PumpCal::isCalibrated(id)) {
    Serial.print("[WARN] ");
    Serial.print(PumpCal::name(id));
    Serial.println(" is not calibrated — using one 500 ms pulse. Type CAL.");
    return PumpCal::BURST_MS < capMs ? PumpCal::BURST_MS : capMs;
  }
  // 1 ms is the clock resolution. Do not lift a short dose up to a full burst.
  return PumpCal::runMsForVolume(id, doseMl, PumpCal::BURST_MS, 1UL, capMs);
}

bool reservoirAllows(PumpCal::Id id, float doseMl, const char* action) {
  if (PumpCal::canDispense(id, doseMl)) {
    return true;
  }
  Serial.print("[STOCK] ");
  Serial.print(PumpCal::name(id));
  Serial.print(" ");
  Serial.print(PumpCal::remainingMl(id), 1);
  Serial.print("/");
  Serial.print(PumpCal::capacityMl(id), 0);
  Serial.print(" mL — not ");
  Serial.print(action);
  Serial.println(". Type FILL after you replace the bottle.");
  return false;
}

float waterDoseMl(float errorPercent) {
  if (ACTIVE_CONTROL_LAW == ControlLaw::THRESHOLD) {
    return 0.0f;
  }
  // Gains are in mL per percent-level error (RESERVOIR_WORKING_VOLUME_ML
  // documents the tank size used to choose those gains).
  (void)RESERVOIR_WORKING_VOLUME_ML;
  return ControlLaw::doseMl(
      ACTIVE_CONTROL_LAW,
      errorPercent,
      WATER_LEVEL_SETTLE_MS / 1000.0f,
      waterPidState,
      WATER_CONTROL_GAINS);
}

float phDoseMl(float errorPh) {
  if (ACTIVE_CONTROL_LAW == ControlLaw::THRESHOLD) {
    return 0.0f;
  }
  return ControlLaw::doseMl(
      ACTIVE_CONTROL_LAW,
      errorPh,
      phSettleMs() / 1000.0f,
      phPidState,
      PH_CONTROL_GAINS);
}

float nutrientDoseMl(float errorPpm) {
  if (ACTIVE_CONTROL_LAW == ControlLaw::THRESHOLD) {
    return 0.0f;
  }
  return ControlLaw::doseMl(
      ACTIVE_CONTROL_LAW,
      errorPpm,
      NUTRIENT_SETTLE_MS / 1000.0f,
      nutrientPidState,
      NUTRIENT_CONTROL_GAINS);
}
