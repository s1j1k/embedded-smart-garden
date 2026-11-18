/* StageController action bridge (water / EC / pH bursts). */
#include "garden.h"
#include "shared/sensor_ema.h"
#include "esp_timer.h"

/************************************************************
 * Hardware-timed dose stop
 *
 * A chemistry dose is stopped by an esp_timer one-shot at exactly its planned
 * run time, so nothing the loop is doing (a sensor step, a Serial printout, a
 * LittleFS write) can lengthen it. The timer callback runs in the esp_timer
 * task and only writes the servo stop; the loop sees doseStopTimerFired() and
 * does the bookkeeping. Only one chemistry pump runs at a time.
 ************************************************************/

static esp_timer_handle_t doseStopTimer = nullptr;
static volatile int doseTimerPump = 0;      // 2 nutrient, 3 pH up, 4 pH down
static volatile bool doseTimerFiredFlag = false;

static void doseStopCallback(void*) {
  switch (doseTimerPump) {
    case 2: pump2.write(SERVO_STOP); break;
    case 3: pump3.write(SERVO_STOP); break;
    case 4: pump4.write(SERVO_STOP); break;
    default: break;
  }
  doseTimerFiredFlag = true;
}

void armDoseStopTimer(int pumpNumber, unsigned long runMs) {
  if (doseStopTimer == nullptr) {
    esp_timer_create_args_t args = {};
    args.callback = &doseStopCallback;
    args.name = "dose_stop";
    if (esp_timer_create(&args, &doseStopTimer) != ESP_OK) {
      doseStopTimer = nullptr;
      Serial.println("[ERROR] dose_stop timer create failed — loop timing only");
      return;
    }
  }
  esp_timer_stop(doseStopTimer);  // harmless if not running
  doseTimerFiredFlag = false;
  doseTimerPump = pumpNumber;
  esp_timer_start_once(doseStopTimer, static_cast<uint64_t>(runMs) * 1000ULL);
}

bool doseStopTimerFired() {
  return doseTimerFiredFlag;
}

/************************************************************
 * Staged control runtime — executes StageController actions
 ************************************************************/

bool stagedWaterTopUp = false;
bool stagedEcDose = false;
bool stagedPhDose = false;

void abortStagedDosingForOperator() {
  stagedWaterTopUp = false;
  stagedEcDose = false;
  stagedPhDose = false;
  StageController::ctx().activity = StageController::IDLE;
  StageController::ctx().waitUntilMs = 0;
  StageController::ctx().pendingPhDir = PhControl::NONE;
  StageController::ctx().pendingPhDoseMl = 0.0f;
  StageController::ctx().pendingEcDoseMl = 0.0f;
  StageController::ctx().runPhAfterEcSettle = false;
}

// Soft prime from MODE PRIMING (or any stage): pause auto dosing only.
// Sensors / Influx / drain keep running. Works even when ENABLE_PUMPS=0.
void startManualPrime(PumpCal::Id id) {
  if (id >= PumpCal::COUNT) {
    return;
  }
  if (pumpCalibrationActive()) {
    Serial.println("[PRIME] CAL session owns the pumps — use PRIME / STOP there, or ABORT.");
    return;
  }
  if (manualPrimeActive) {
    Serial.println("[PRIME] Already priming. Type STOP first.");
    return;
  }
  if (isWaterFillMode()) {
    Serial.println("[PRIME] Leave MODE FILL first (CONFIRM or MODE PRIMING).");
    return;
  }

  abortStagedDosingForOperator();
  stopAllDosingPumps();
#if ENABLE_INFLUXDB
  InfluxCloud::preemptForControl();
#endif
  manualPrimePump = id;
  manualPrimeStartedAt = millis();
  manualPrimeActive = true;
  startCalPump(manualPrimePump);

  Serial.print("[PRIME] Soft-priming ");
  Serial.print(PumpCal::name(manualPrimePump));
  Serial.print(" (");
  Serial.print(PumpCal::gpioHint(manualPrimePump));
  Serial.println(") — auto pH/EC/level dosing paused.");
  Serial.println("[PRIME] Put a waste cup on the outlet. Type STOP when liquid (not air) comes out.");
  Serial.print("[PRIME] Auto-stops after ");
  Serial.print(CAL_PRIME_MAX_MS / 1000UL);
  Serial.println(" s. Sensors and Influx keep running.");
}

void finishManualPrime(const char* reason, bool announceResume) {
  if (!manualPrimeActive) {
    return;
  }
  const unsigned long primeMs = millis() - manualPrimeStartedAt;
  stopCalPump(manualPrimePump);
  if (primeMs > 0) {
    PumpCal::consumeRunMs(manualPrimePump, primeMs);
  }
  manualPrimeActive = false;
  Serial.print("[PRIME] Stopped (");
  Serial.print(reason);
  Serial.print(") after ");
  Serial.print(primeMs / 1000UL);
  Serial.print(" s — ");
  Serial.print(PumpCal::name(manualPrimePump));
  Serial.println(".");
  if (announceResume) {
    Serial.println("[PRIME] Auto dosing resumes on the next control cycle.");
    #if !ENABLE_PUMPS
    Serial.println("[PRIME] Note: ENABLE_PUMPS is still 0 — auto doses stay off until you set it to 1.");
    #endif
  }
}

void updateManualPrime(unsigned long now) {
  if (!manualPrimeActive) {
    return;
  }
  startCalPump(manualPrimePump);
  if (now - manualPrimeStartedAt >= CAL_PRIME_MAX_MS) {
    finishManualPrime("safety timeout — type PRIME PH_UP/PH_DOWN again if still air", true);
    return;
  }
  static unsigned long lastPrimeNoteMs = 0;
  if (now - lastPrimeNoteMs >= 5000UL) {
    lastPrimeNoteMs = now;
    Serial.print("[PRIME] Still priming ");
    Serial.print(PumpCal::name(manualPrimePump));
    Serial.print(" (");
    Serial.print((now - manualPrimeStartedAt) / 1000UL);
    Serial.println(" s). Type STOP when liquid reaches the outlet.");
  }
}

// The pause is saved, so a power cut or reboot cannot quietly restart dosing.
static const char* AUTO_PAUSE_PATH = "/auto_paused.txt";

static void saveAutoControlPaused(bool paused) {
  File f = LittleFS.open(AUTO_PAUSE_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[AUTO] WARNING: could not save the pause state to LittleFS");
    return;
  }
  f.println(paused ? 1 : 0);
  f.close();
}

void loadAutoControlPaused() {
  File f = LittleFS.open(AUTO_PAUSE_PATH, FILE_READ);
  if (!f) {
    return;  // never saved: auto control runs
  }
  autoControlPaused = f.readStringUntil('\n').toInt() == 1;
  f.close();
  if (autoControlPaused) {
    Serial.println("[AUTO] Auto control PAUSED (saved before reboot) — AUTO RESUME to restart");
  }
}

void setAutoControlPaused(bool paused, const char* via) {
  if (autoControlPaused == paused) {
    Serial.print("[AUTO] Already ");
    Serial.println(paused ? "paused" : "running");
    saveAutoControlPaused(paused);
    return;
  }
  autoControlPaused = paused;
  saveAutoControlPaused(paused);
  if (paused) {
    abortStagedDosingForOperator();
    stopAllDosingPumps();
  } else {
    // The tank may have changed while paused (water change, hand doses):
    // clean start, sensor warm-up hold, then a full cycle on fresh readings.
    StageController::armFreshStart(millis());
    StageController::logEvent("auto_resume", buildSensorSnapshot(), "", NAN, NAN, NAN, NAN,
                              via != nullptr ? via : "", currentIsoTimestamp().c_str());
  }
  Serial.print("[AUTO] Control ");
  Serial.print(paused ? "PAUSED" : "RESUMED");
  if (via != nullptr && via[0] != '\0') {
    Serial.print(" (");
    Serial.print(via);
    Serial.print(")");
  }
  Serial.print(" — stage stays ");
  Serial.println(systemModeName());
  if (!paused) {
    Serial.print("[AUTO] Fresh start: first check in ");
    Serial.print(ControlDesign::POST_BOOT_HOLD_MS / 60000UL);
    Serial.println(" min on settled readings (level -> nutrient -> pH). CYCLE runs it now.");
  }
}

bool startOperatorDose(PumpCal::Id id, float doseMl, const char* via) {
  if (id >= PumpCal::COUNT) {
    Serial.println("[DOSE] Unknown pump. Use PH_UP | PH_DOWN | NUTRIENT | WATER.");
    return false;
  }
  if (isnan(doseMl) || doseMl <= 0.0f) {
    Serial.println("[DOSE] Dose mL must be > 0.");
    return false;
  }
  if (pumpCalibrationActive()) {
    Serial.println("[DOSE] CAL session owns the pumps — finish or ABORT first.");
    return false;
  }
  if (softPrimeActive()) {
    Serial.println("[DOSE] Soft-prime running — type STOP first.");
    return false;
  }
  if (!PumpCal::isCalibrated(id)) {
    Serial.print("[DOSE] REFUSED — ");
    Serial.print(PumpCal::name(id));
    Serial.println(" is not calibrated. Run CAL for that pump first.");
    return false;
  }
  if (stagedPumpBusy() || StageController::isBusy(millis())) {
    Serial.println("[DOSE] Busy — wait for current dose/settle, or AUTO PAUSE first.");
    return false;
  }

  StageController::Action action;
  action.volumeMl = doseMl;
  int maxBursts = -1;
  switch (id) {
    case PumpCal::WATER:
      action.type = StageController::ACTION_TOP_UP_WATER;
      break;
    case PumpCal::NUTRIENT:
      action.type = StageController::ACTION_DOSE_EC;
      maxBursts = 40;  // characterization may need larger than auto-cap
      break;
    case PumpCal::PH_UP:
      action.type = StageController::ACTION_DOSE_PH_UP;
      maxBursts = 20;
      break;
    case PumpCal::PH_DOWN:
      action.type = StageController::ACTION_DOSE_PH_DOWN;
      maxBursts = 20;
      break;
    default:
      return false;
  }

  abortStagedDosingForOperator();
  const unsigned long now = millis();
  if (!startStagedAction(action, now, maxBursts)) {
    Serial.println("[DOSE] Actuation failed (stock, level fault, or busy).");
    return false;
  }

  const unsigned long onMs = id == PumpCal::NUTRIENT ? activeNutrientDoseMs
                             : id == PumpCal::WATER  ? activeWaterRefillMs
                                                     : activePhDoseMs;
  const float delivered = PumpCal::volumeForRunMs(id, onMs);

  ControlEventLog::Row row;
  char recordedAtUtc[24] = "";
#if ENABLE_WIFI
  recordedAtUtcForUptime(
      now,
      recordedAtUtc,
      sizeof(recordedAtUtc));
#endif
  row.recordedAtUtc = recordedAtUtc;
  row.uptimeMs = now;
  row.event = "operator_dose";
  row.stage = systemModeName();
  row.currentLevelPct = waterLevelPercent;
  row.currentDrawdownMm =
      LevelControl::drawdownMm(fill0Percent(), waterLevelPercent);
  row.targetDepthMm = GrowStage::config(systemMode).targetDrawdownMm;
  row.phBefore = SensorEma::forControl(phValueEma, phValueFiltered);
  row.ecBefore = SensorEma::forControl(tdsPpmEma, static_cast<float>(tdsPPM));
  row.waterTempC = waterTempC;
  row.gainUp = StageController::ctx().ph.gainUp;
  row.gainDown = StageController::ctx().ph.gainDown;
  row.gainEc = StageController::ctx().ec.gainEc;
  if (id == PumpCal::NUTRIENT) {
    row.ecDoseMl = delivered;
  } else if (id == PumpCal::PH_UP) {
    row.phDoseDir = "up";
    row.phDoseMl = delivered;
  } else if (id == PumpCal::PH_DOWN) {
    row.phDoseDir = "down";
    row.phDoseMl = delivered;
  }
  row.alertFlags = via != nullptr ? via : "operator";
  ControlEventLog::append(row);
  ControlEventLog::logToSerial(row);
  publishControlEvent(row);

  Serial.print("[DOSE] ");
  Serial.print(PumpCal::name(id));
  Serial.print(" requested ");
  Serial.print(doseMl, 3);
  Serial.print(" mL → ");
  Serial.print(onMs);
  Serial.print(" ms ~");
  Serial.print(delivered, 3);
  Serial.print(" mL");
  if (via != nullptr && via[0] != '\0') {
    Serial.print(" (");
    Serial.print(via);
    Serial.print(")");
  }
  Serial.println();
  return true;
}

bool stagedPumpBusy() {
  return waterRefillPumpRunning || waterDrainPumpRunning ||
         phControlState != PH_CONTROL_IDLE ||
         nutrientControlState != NUTRIENT_CONTROL_IDLE;
}

bool sharedRailPumpDrawing() {
  // Pumps and the analog probes share the board 5 V rail. A sample taken
  // while a motor is drawing sags the ADC. Settle/mixing states are not
  // included: the motor is already off.
  return waterRefillPumpRunning || waterDrainPumpRunning || calBurstOn ||
         phControlState == PH_DOSING_UP || phControlState == PH_DOSING_DOWN ||
         nutrientControlState == NUTRIENT_DOSING ||
         nutrientControlState == NUTRIENT_DILUTING;
}

static SensorEma::State phEmaState;

void readPhSensorValues() {
  #if ENABLE_PH_TDS
  if (sharedRailPumpDrawing()) {
    Serial.println("[DEBUG] pH read held — pump on shared 5V rail");
    return;
  }
  // The TDS probe is on (or only just off): its excitation is in the water.
  // Keep the last pH until the sensor cycle's quiet window has passed.
  if (tdsExcitationActive()) {
    return;
  }

  float phSamples[AnalogFilter::DEFAULT_SAMPLES];
  int sampleCount = 0;
  float voltSamples[AnalogFilter::DEFAULT_SAMPLES];

  for (int i = 0; i < AnalogFilter::DEFAULT_SAMPLES; i++) {
    const float gpioVoltage = analogReadMilliVolts(PH_PIN) / 1000.0f;
    const float boardVoltage = gpioVoltage * PH_SIGNAL_LEVEL_SHIFT_GAIN;
    voltSamples[i] = boardVoltage;
    const float phSample = convertVoltageToPH(boardVoltage);
    if (!isnan(phSample) && phSample > 0.0f && phSample < 14.0f) {
      phSamples[sampleCount++] = phSample;
    }
    delay(5);
  }

  phVoltage = AnalogFilter::trimmedMean(voltSamples, AnalogFilter::DEFAULT_SAMPLES, 0.040f);
  phValue = convertVoltageToPH(phVoltage);

  // Most samples outside 0..14 pH (e.g. negative with the 5 V rail off):
  // probe fault. Drop the reading and restart the EMA warm-up.
  if (sampleCount < AnalogFilter::DEFAULT_SAMPLES / 2) {
    if (!phSensorFault) {
      Serial.print("[ALERT] pH sensor fault — ");
      Serial.print(sampleCount);
      Serial.print("/");
      Serial.print(AnalogFilter::DEFAULT_SAMPLES);
      Serial.print(" samples in 0-14 pH (raw ");
      Serial.print(phValue, 2);
      Serial.print(", ");
      Serial.print(phVoltage, 3);
      Serial.println(" V). pH ignored; auto pH dosing held. Check 5 V rail / probe.");
    }
    phSensorFault = true;
    phValueFiltered = NAN;
    phValueEma = NAN;
    SensorEma::reset(phEmaState);
    return;
  }
  if (phSensorFault) {
    phSensorFault = false;
    Serial.println("[ALERT] pH sensor recovered — EMA restarts 60 s warm-up.");
  }

  phValueFiltered = PhControl::filteredReading(phSamples, sampleCount);
  phValueEma = SensorEma::update(phEmaState, phValueFiltered, millis());

  Serial.print("[DEBUG] pH board voltage - ");
  Serial.print(phVoltage, 3);
  Serial.print(" V; pH raw - ");
  Serial.print(phValue, 2);
  Serial.print(", filtered - ");
  Serial.print(phValueFiltered, 2);
  Serial.print(", ema - ");
  Serial.print(phValueEma, 2);
  Serial.print(" @ water ");
  Serial.print(PhProbeCal::effectiveTempC(waterTempC), 1);
  Serial.print(" C (Nernst ");
  Serial.print(PhProbeCal::compensationFactor(waterTempC), 4);
  Serial.println(")");
  #endif
}

unsigned long phWarmupElapsedS() {
  return SensorEma::warmupElapsedS(phEmaState, millis());
}

// NAN while faulted or warming up: the pH loop treats it as ph_invalid.
float readPhFilteredAverage() {
  #if ENABLE_PH_TDS
  readPhSensorValues();
  return phSensorFault ? NAN : phValueEma;
  #else
  return phValue;
  #endif
}

void resetDosingSession(bool ph, bool ec, const char* via) {
  const char* recordedAt = "";
  #if ENABLE_WIFI
  static String ts;
  ts = currentIsoTimestamp();
  recordedAt = ts.c_str();
  #endif
  StageController::resetDosingSession(ph, ec, buildSensorSnapshot(), recordedAt);
  Serial.print("[DOSE] Session cap reset for");
  Serial.print(ph ? " pH" : "");
  Serial.print(ec ? " nutrient" : "");
  Serial.print(" (");
  Serial.print(via);
  Serial.println(") — the cap applies again from zero; any pH no-response fault is cleared.");
}

StageController::Sensors buildSensorSnapshot() {
  StageController::Sensors s;
  s.ph = readPhFilteredAverage();
  s.ecPpm = tdsPpmEma;  // NAN during the 60 s warm-up: no EC dose
  s.waterTempC = waterTempC;
  s.levelPercent = waterLevelPercentForControl();
  s.levelSpreadPct = waterLevelSpreadPercent();
  return s;
}

bool startStagedAction(const StageController::Action& action, unsigned long now) {
  return startStagedAction(action, now, -1);
}

bool startStagedAction(const StageController::Action& action, unsigned long now,
                       int maxBurstsOverride) {
  #if !ENABLE_PUMPS
  (void)action;
  (void)now;
  (void)maxBurstsOverride;
  return false;
  #else
  if (action.type == StageController::ACTION_NONE) {
    return false;
  }
  if (stagedPumpBusy() || StageController::isBusy(now)) {
    return false;
  }
#if ENABLE_INFLUXDB
  // Drop any in-flight TLS write before the servo leaves stop. The line
  // stays queued and goes out after this pump turns off.
  InfluxCloud::preemptForControl();
#endif

  switch (action.type) {
    case StageController::ACTION_TOP_UP_WATER: {
      if (isWaterLevelSensorFault()) {
        Serial.println("[STAGE-CTRL] Top-up blocked — water level sensor fault (0% after FILL)");
        return false;
      }
      #if WATER_PUMP_OFFLINE
      Serial.println("[STAGE-CTRL] Top-up blocked — water pump offline (top up by hand)");
      return false;
      #endif
      // Sized from the water pump's calibration (the remaining deficit), one
      // chunk of at most TOPUP_CHUNK_MAX_MS; the pump also stops early when the
      // live level reaches the refill target (updateWaterRefillPump).
      if (PumpCal::isCalibrated(PumpCal::WATER)) {
        activeWaterRefillMs = PumpCal::runMsForVolume(
            PumpCal::WATER, action.volumeMl, WATER_REFILL_INCREMENT_MS, PUMP_MIN_RUN_MS,
            ControlDesign::TOPUP_CHUNK_MAX_MS);
      } else {
        Serial.println("[WARN] water pump not calibrated - 1.5 s top-up pulse. Run CAL WATER.");
        activeWaterRefillMs = WATER_REFILL_INCREMENT_MS;
      }
      if (!reservoirAllows(PumpCal::WATER,
                           PumpCal::volumeForRunMs(PumpCal::WATER, activeWaterRefillMs),
                           "drawdown top-up")) {
        return false;
      }
      pump1.write(WATER_REFILL_RUN_SPEED);
      waterRefillPumpRunning = true;
      waterRefillStartedAt = now;
      stagedWaterTopUp = true;
      Serial.print("[STAGE-CTRL] Top-up chunk ");
      Serial.print(PumpCal::volumeForRunMs(PumpCal::WATER, activeWaterRefillMs), 0);
      Serial.print(" mL (");
      Serial.print(activeWaterRefillMs);
      Serial.print(" ms) of ");
      Serial.print(action.volumeMl, 0);
      Serial.print(" mL to the target; stops at level ");
      Serial.print(StageController::refillTargetPercent());
      Serial.println(" %");
      return true;
    }
    case StageController::ACTION_DOSE_EC: {
      const int maxBursts = maxBurstsOverride > 0
                                ? maxBurstsOverride
                                : CAL_MAX_DOSE_BURSTS_NUTRIENT;
      activeNutrientDoseMs = plannedDoseMs(PumpCal::NUTRIENT, action.volumeMl, maxBursts);
      if (activeNutrientDoseMs == 0) {
        return false;
      }
      if (!reservoirAllows(PumpCal::NUTRIENT,
                           PumpCal::volumeForRunMs(PumpCal::NUTRIENT, activeNutrientDoseMs),
                           "EC dose")) {
        return false;
      }
      nutrientBurstDone = 0;
      nutrientBurstTarget = 0;
      nutrientBurstOn = true;
      nutrientBurstMark = now;
      pump2.write(NUTRIENT_RUN_SPEED);
      armDoseStopTimer(2, activeNutrientDoseMs);
      nutrientControlState = NUTRIENT_DOSING;
      nutrientControlStateStartedAt = now;
      stagedEcDose = true;
      Serial.print("[STAGE-CTRL] EC dose ");
      Serial.print(action.volumeMl, 2);
      Serial.print(" mL (");
      Serial.print(activeNutrientDoseMs);
      Serial.println(" ms)");
      return true;
    }
    case StageController::ACTION_DOSE_PH_UP:
    case StageController::ACTION_DOSE_PH_DOWN: {
      const bool down = action.type == StageController::ACTION_DOSE_PH_DOWN;
      #if PH_DOWN_PUMP_OFFLINE
      if (down) {
        Serial.println("[STAGE-CTRL] pH-down blocked — pump offline (PCB D5); lower pH by hand");
        return false;
      }
      #endif
      const PumpCal::Id pid = down ? PumpCal::PH_DOWN : PumpCal::PH_UP;
      const int maxBursts = maxBurstsOverride > 0
                                ? maxBurstsOverride
                                : CAL_MAX_DOSE_BURSTS_PH;
      activePhDoseMs = plannedDoseMs(pid, action.volumeMl, maxBursts);
      if (activePhDoseMs == 0) {
        return false;
      }
      if (!reservoirAllows(pid,
                           PumpCal::volumeForRunMs(pid, activePhDoseMs),
                           down ? "pH-down" : "pH-up")) {
        return false;
      }
      phBurstDone = 0;
      phBurstTarget = 0;
      phBurstOn = true;
      phBurstMark = now;
      if (down) {
        pump4.write(PH_DOWN_RUN_SPEED);
        phControlState = PH_DOSING_DOWN;
      } else {
        pump3.write(PH_UP_RUN_SPEED);
        phControlState = PH_DOSING_UP;
      }
      armDoseStopTimer(down ? 4 : 3, activePhDoseMs);
      phControlStateStartedAt = now;
      stagedPhDose = true;
      Serial.print("[STAGE-CTRL] pH ");
      Serial.print(down ? "down" : "up");
      Serial.print(" dose ");
      Serial.print(action.volumeMl, 2);
      Serial.print(" mL (");
      Serial.print(activePhDoseMs);
      Serial.println(" ms)");
      return true;
    }
    default:
      return false;
  }
  #endif
}

void onStagedWaterComplete(unsigned long now) {
  if (!stagedWaterTopUp) {
    return;
  }
  stagedWaterTopUp = false;
  const float deliveredMl = PumpCal::volumeForRunMs(PumpCal::WATER, waterRefillLastRunMs);
  solutionOnTopUp(deliveredMl);
  StageController::addRefillDelivered(deliveredMl);
  const int target = StageController::refillTargetPercent();
  const int level = waterLevelPercentForControl();
  const bool reached = target >= 0 && level >= target;
  StageController::onWaterTopUpFinished(reached);
  Serial.println(reached ? "[STAGE-CTRL] Top-up complete - level at target"
                         : "[STAGE-CTRL] Top-up chunk complete - pump rests, then the level is checked");
}

// loop: "level", "nutrient" or "ph". Pausing level ends a running top-up now
// (the controller then goes on to nutrient and pH); a running nutrient or pH
// dose finishes its burst. Saved, so it survives a reboot.
bool setLoopPaused(const char* loop, bool paused, const char* via) {
  StageController::Context& c = StageController::ctx();
  bool* flag = nullptr;
  if (strcmp(loop, "level") == 0 || strcmp(loop, "water") == 0) {
    flag = &c.levelPaused;
  } else if (strcmp(loop, "nutrient") == 0 || strcmp(loop, "ec") == 0) {
    flag = &c.nutrientPaused;
  } else if (strcmp(loop, "ph") == 0) {
    flag = &c.phPaused;
  }
  if (flag == nullptr) {
    Serial.println("[LOOP] Unknown loop - use level, nutrient or ph");
    return false;
  }
  *flag = paused;
  StageController::saveLoopsPaused();
  if (paused && flag == &c.levelPaused && waterRefillPumpRunning && stagedWaterTopUp) {
    activeWaterRefillMs = 0;  // updateWaterRefillPump stops it on the next pass
    c.refilling = false;
  }
  if (paused && flag == &c.levelPaused) {
    c.refillHold = false;
    c.refillHoldReason = "";
  }
  StageController::logEvent(paused ? "loop_paused" : "loop_resumed", buildSensorSnapshot(), "",
                            NAN, NAN, NAN, NAN, loop, currentIsoTimestamp().c_str());
  Serial.print("[LOOP] ");
  Serial.print(loop);
  Serial.print(paused ? " PAUSED" : " RESUMED");
  Serial.print(" (");
  Serial.print(via != nullptr ? via : "");
  Serial.println(paused ? ") - the other loops keep running in order" : ")");
  return true;
}

bool proceedRefill(const char* via) {
  StageController::Context& c = StageController::ctx();
  if (!c.refillHold) {
    Serial.println("[STAGE-CTRL] No refill on hold.");
    return false;
  }
  const char* reason = c.refillHoldReason;
  StageController::confirmRefill();
  StageController::logEvent("refill_proceed", buildSensorSnapshot(), "", NAN, NAN, NAN, NAN,
                            via != nullptr ? via : "", currentIsoTimestamp().c_str());
  Serial.print("[STAGE-CTRL] Refill confirmed (");
  Serial.print(via != nullptr ? via : "");
  Serial.print(", was on hold: ");
  Serial.print(reason);
  Serial.println(") - continuing to the target");
  return true;
}

int stagedTopUpStopPercent() {
  return stagedWaterTopUp ? StageController::refillTargetPercent() : -1;
}

void onStagedEcComplete(unsigned long now, float deliveredMl) {
  if (!stagedEcDose) {
    return;
  }
  stagedEcDose = false;
  StageController::onEcDoseFinished(deliveredMl);
  nutrientControlAllowedAt = now + GrowStage::config(systemMode).ecSettleMs;
  Serial.print("[STAGE-CTRL] EC dose complete (");
  Serial.print(deliveredMl, 2);
  Serial.println(" mL delivered) — waiting for mixing before pH check");
}

void onStagedPhComplete(unsigned long now, float deliveredMl) {
  if (!stagedPhDose) {
    return;
  }
  stagedPhDose = false;
  StageController::onPhDoseFinished(deliveredMl);
  phControlAllowedAt = now + GrowStage::config(systemMode).phSettleMs;
  Serial.print("[STAGE-CTRL] pH dose complete (");
  Serial.print(deliveredMl, 3);
  Serial.println(" mL delivered) — waiting for mixing before re-check");
}

void serviceStagedControl(unsigned long now) {
  const char* recordedAt = "";
  #if ENABLE_WIFI
  static String ts;
  ts = currentIsoTimestamp();
  recordedAt = ts.c_str();
  #endif

  StageController::serviceWaits(now, buildSensorSnapshot(), recordedAt);
  serviceSolutionMonitor(now);

  if (isWaterFillMode() || pumpCalibrationActive() || softPrimeActive() ||
      autoControlPaused) {
    return;
  }
  if (isControlIdleMode()) {
    return;
  }

  if (stagedPumpBusy() || StageController::isBusy(now)) {
    return;
  }

  if (!phControlAllowedByMode() && !waterAutoControlAllowedByMode() &&
      !nutrientsAllowedByMode()) {
    return;
  }

  const StageController::Action action =
      StageController::planControlCycle(buildSensorSnapshot(), now, recordedAt);
  const bool started = startStagedAction(action, now);
  if (!started && action.type == StageController::ACTION_TOP_UP_WATER) {
    StageController::onTopUpRefused(now);
  } else if (!started && (action.type == StageController::ACTION_DOSE_PH_UP ||
                          action.type == StageController::ACTION_DOSE_PH_DOWN)) {
    StageController::onPhDoseRefused(now);
  } else if (!started && action.type == StageController::ACTION_DOSE_EC) {
    // Nutrient dose refused (stock, empty bottle, ENABLE_PUMPS=0): the nutrient
    // loop cannot run, so pH is checked on its own on the next pass.
    StageController::ctx().runPhAfterEcSettle = true;
    StageController::ctx().pendingEcDoseMl = 0.0f;
    StageController::ctx().nutrientDoseRefused = true;
    Serial.println("[STAGE-CTRL] EC dose not started — checking pH next");
  }
}
