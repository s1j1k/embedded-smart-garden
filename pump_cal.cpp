/* Pump calibration wizard (Serial / MQTT). */
#include "garden.h"

/************************************************************
 * Pump calibration (Serial + MQTT dashboard wizard)
 *
 * Open Serial Monitor at 115200, set line ending to Newline, type HELP.
 * Dashboard: {"cmd":"pump_cal","action":"enter|prime|run|stop|volume|…"}
 * A human must still confirm the cup is on the outlet before "run".
 ************************************************************/

bool pumpCalibrationActive() {
  return calPhase != CAL_IDLE;
}

bool softPrimeActive() {
  return manualPrimeActive;
}

void stopAllDosingPumps() {
  pump1.write(SERVO_STOP);
  pump2.write(SERVO_STOP);
  pump3.write(SERVO_STOP);
  pump4.write(SERVO_STOP);
  waterRefillPumpRunning = false;
  waterDrainPumpRunning = false;
  waterSafetyRefillActive = false;
  if (waterFillManualPumpRunning) {
    const unsigned long ran = millis() - waterFillManualStartedAt;
    PumpCal::consumeRunMs(PumpCal::WATER, ran);
    waterFillManualPumpRunning = false;
  }
  phControlState = PH_CONTROL_IDLE;
  nutrientControlState = NUTRIENT_CONTROL_IDLE;
  if (manualPrimeActive) {
    const unsigned long ran = millis() - manualPrimeStartedAt;
    if (ran > 0) {
      PumpCal::consumeRunMs(manualPrimePump, ran);
    }
    manualPrimeActive = false;
  }
}

void startCalPump(PumpCal::Id id) {
  switch (id) {
    case PumpCal::WATER:
      pump1.write(WATER_REFILL_RUN_SPEED);
      break;
    case PumpCal::NUTRIENT:
      pump2.write(NUTRIENT_RUN_SPEED);
      break;
    case PumpCal::PH_UP:
      pump3.write(PH_UP_RUN_SPEED);
      break;
    case PumpCal::PH_DOWN:
      pump4.write(PH_DOWN_RUN_SPEED);
      break;
    default:
      break;
  }
}

void stopCalPump(PumpCal::Id id) {
  switch (id) {
    case PumpCal::WATER:
      pump1.write(SERVO_STOP);
      break;
    case PumpCal::NUTRIENT:
      pump2.write(SERVO_STOP);
      break;
    case PumpCal::PH_UP:
      pump3.write(SERVO_STOP);
      break;
    case PumpCal::PH_DOWN:
      pump4.write(SERVO_STOP);
      break;
    default:
      break;
  }
}

void printSerialHelp(bool verbose) {
  Serial.println();
  Serial.println("========== SERIAL COMMANDS ==========");
  Serial.println("HELP              list commands");
  Serial.print("VERSION           firmware version (now: ");
  Serial.print(FIRMWARE_VERSION);
  Serial.println(")");
  Serial.print("MODE              show mode (now: ");
  Serial.print(systemModeName());
  Serial.println(")");
  Serial.println("MODE BASELINE     sensor-only logging (no pumps) — probe drift capture");
  Serial.println("MODE IDLE         no stage set — sensors only until you choose a stage");
  Serial.println("MODE FILL         manual fill → CONFIRM records fill_0 → PH_BASELINE");
  Serial.println("CONFIRM           FILL: → PH_BASELINE | BASELINE: record fill_0 only");
  Serial.println("BASELINE START    mark capture window start (Influx + flash)");
  Serial.println("BASELINE END      mark capture complete");
  Serial.println("INFLUX            print cloud upload status");
  Serial.println("INFLUX TEST       send one test point to InfluxDB now");
  Serial.println("POWER             brownout / uptime / rail sense / heap / RSSI");
  Serial.println("NET / MQTT        WiFi, NTP, MQTT and outbox status");
  Serial.println("MQTT CONNECT      try MQTT now (it also retries by itself with backoff)");
  Serial.println("DUMP CTRL         print /control_events.csv (pH/EC/level adjustments)");
  Serial.println("DUMP SENSORS      print /sensor_readings.csv (framed; scripts/serial_dump.py)");
  Serial.println("DUMP PREV|OUTBOX  previous journal / MQTT outbox; add RAW, FROM <line>");
  Serial.println("DUMP STOP         stop a running dump");
  Serial.println("MODE SEEDLING     0 mm drawdown; pH only");
  Serial.println("MODE TRANSITIONAL veg control; EC × 0.5 of live vegetative target");
  Serial.println("MODE VEGETATIVE   50 mm drawdown; EC then pH");
  Serial.println("MODE HARVEST      100 mm drawdown; EC then pH (gentler)");
  Serial.println("CYCLE             trigger one control cycle now");
  Serial.println("ENTERPH           start SEN0169 two-point probe calibration");
  Serial.println("CALPH ACID        capture buffer 4.00 (volts + DS18B20 buffer temp)");
  Serial.println("CALPH ALKALI      capture buffer 9.18 (pH from sachet table at temp)");
  Serial.println("CALPH ACID 21.5   same, with hand-measured buffer C (no DS18B20)");
  Serial.println("EXITPH            fit slope/offset, save to LittleFS, exit");
  Serial.println("PHABORT           cancel probe calibration (keep last save)");
  Serial.println("PHSHOW            live V, buffer temp + slope/offset");
  Serial.println("PHRESTORE         rewrite the default (provisional 11 Oct) cal to LittleFS");
  Serial.println("PHCLEAR           same as PHRESTORE (back to the default line)");
  Serial.println("ENTERTDS          start Grove TDS single-point calibration");
  Serial.println("CALTDS <ppm>      capture ref (salt water / pen / 707 standard)");
  Serial.println("EXITTDS           save kValue to LittleFS, exit");
  Serial.println("TDSABORT          cancel TDS calibration (keep last save)");
  Serial.println("TDSSHOW           print live V + kValue + ppm");
  Serial.println("TDSCLEAR          erase saved TDS cal (back to k=1.0)");
  Serial.println("TDSOBS            pH-salt TDS observer (raw vs nutrient ppm)");
  Serial.println("SETKAPPA UP|DOWN <v>  salt κ (ppm·L/mL); marks measured");
  Serial.println("TDSCORR ON|OFF    use TDS_nutrient in EC control loop");
  Serial.println("RESETPHSALT       clear pH-reagent accumulators (after flush)");
  Serial.println("CYCLE / TRIGGER   run one control cycle now");
  Serial.println("CYCLE LEVEL|NUTRIENT|PH  run one loop now");
  Serial.println("FLASH [ms] / FLASH OFF  white camera flash on the grow light");
  Serial.println("LIGHT MAKEUP [OFF|CLEAR|STATUS]  repay grow light missed while off");
  Serial.println("LIGHT ON [hours]  grow light on now (default: owed light), then schedule");
  Serial.println("LIGHT AUTO        end LIGHT ON, back to the schedule");
  Serial.println("AUTO PAUSE|RESUME hold/resume auto dosing (stage unchanged)");
  Serial.println("SOLUTION          water-change indicators (age, top-up, reagent, buffer, pH, TDS)");
  Serial.println("WATER CHANGED     record a water change (indicators start from zero)");
  Serial.println("SESSION RESET PH|EC|ALL  allow more dosing after a session cap or pH no-response (check why first)");
  Serial.println("DOSE PH_UP 0.15   metered operator dose (requires CAL; mL)");
  Serial.println("DOSE PH_DOWN|NUTRIENT|WATER <ml>");
  Serial.println("CONFIRM / FILLED  save hand-fill level as net-pot touch → PRIMING");
  Serial.println("CAL               calibrate all 4 pumps, one by one");
  Serial.println("CAL WATER         1 L top-up jug (continuous run)");
  Serial.println("CAL NUTRIENT      1 L bottle (500 ms bursts)");
  Serial.println("CAL PH_UP         600 mL bottle (500 ms bursts)");
  Serial.println("CAL PH_DOWN       600 mL bottle (500 ms bursts)");
  Serial.println("CAL SHOW          print stored calibration");
  Serial.println("CAL CLEAR         erase stored rates (keeps bottle levels)");
  Serial.println("STOCK             print remaining mL in each bottle");
  Serial.println("FILL              mark all bottles full (after you replace them)");
  Serial.println("FILL WATER        or NUTRIENT / PH_UP / PH_DOWN");
  Serial.println("SETCAP WATER 1000 change programmed bottle size (mL)");
  Serial.println("SETREM WATER 800  set remaining estimate (mL)");
  Serial.println("PRIME WATER       soft-prime: pause auto dosing, run until STOP");
  Serial.println("PRIME PH_UP       (or NUTRIENT / PH_DOWN) — sensors stay online");
  Serial.println("PRIME             during CAL only: prime the current cal pump");
  Serial.println("STOP              stop soft-prime / CAL prime / fill pump");
  Serial.println("YES               start the cup run (or a confirm replay)");
  Serial.println("RETRY / SKIP / ABORT");
  Serial.println("Then enter the measured mL after each run (e.g. 2.5)");
  if (verbose) {
    Serial.println();
    Serial.println("Use Newline as the Serial Monitor line ending.");
    Serial.println("Boot → MODE FILL: fill tank by hand until water touches net pots, CONFIRM.");
    Serial.println("Then MODE PRIMING adjusts pH only. MODE SEEDLING unlocks nutrients.");
    Serial.println("GPIO18 auto control (after seedling): safety top-up or EC dilute only.");
    Serial.println("pH / nutrient: 500 ms bursts until STOP, then 3 repeats → mL/burst.");
    Serial.println("Water jug: continuous until STOP (max 8 s), then 3 repeats → mL/s.");
    Serial.println("Empty bottles stop auto dosing. Type FILL after you swap a bottle.");
    Serial.println("CAL/PRIME stops the whole garden (sensors, MQTT, dosing).");
    Serial.println("See CONTROL_REPORT.md for P/PI/PID vs threshold control.");
  }
  Serial.println("=====================================");
}

bool calIsContinuous() {
  return !PumpCal::usesBursts(calPump);
}

const char* calPhaseName() {
  switch (calPhase) {
    case CAL_WAIT_READY: return "wait_ready";
    case CAL_PRIMING: return "priming";
    case CAL_RUNNING: return "running";
    case CAL_WAIT_VOLUME: return "wait_volume";
    case CAL_IDLE:
    default: return "idle";
  }
}

void setCalMqttStatus(const char* msg, bool ok) {
  calMqttMessage = (msg != nullptr) ? msg : "";
  calMqttOk = ok;
}

void resetCalPumpStats() {
  calPulseMs = 0;
  calBurstsDone = 0;
  calTargetBursts = 0;
  calTargetMs = 0;
  calBurstOn = false;
  calStopRequested = false;
  calRateSum = 0.0f;
  calRateCount = 0;
  calLastSample = NAN;
}

void promptCalReady() {
  Serial.println();
  Serial.print("[CAL] Pump: ");
  Serial.print(PumpCal::name(calPump));
  Serial.print(" — ");
  Serial.println(PumpCal::gpioHint(calPump));
  if (calIsContinuous() && calTargetMs > 0) {
    Serial.print("[CAL] Confirm run ");
    Serial.print(calRateCount);
    Serial.print("/");
    Serial.print(CAL_CONFIRM_RUNS);
    Serial.print(" — empty the 10 mL cup, then YES (");
    Serial.print(calTargetMs);
    Serial.println(" ms continuous).");
    return;
  }
  if (!calIsContinuous() && calTargetBursts > 0) {
    Serial.print("[CAL] Confirm run ");
    Serial.print(calRateCount);
    Serial.print("/");
    Serial.print(CAL_CONFIRM_RUNS);
    Serial.print(" — empty the 10 mL cup, then YES (");
    Serial.print(calTargetBursts);
    Serial.print(" x ");
    Serial.print(CAL_BURST_MS);
    Serial.println(" ms).");
    return;
  }
  Serial.println("[CAL] 1) Put the correct liquid on the INLET.");
  Serial.println("[CAL] 2) If the line is dry, put a WASTE cup on the outlet and type PRIME.");
  Serial.println("[CAL]    Type STOP when liquid (not air) comes out. Empty the cup.");
  Serial.println("[CAL] 3) Put the empty 10 mL measuring cup on the OUTLET.");
  if (calIsContinuous()) {
    Serial.println("[CAL] Tank is filled by hand this iteration. This is the 1 L top-up jug.");
    Serial.println("[CAL] 4) Type YES. Pump runs continuously. STOP when the cup is readable.");
    Serial.println("[CAL]    Auto-stops at 8 s. That duration is replayed 3 times → mL/s.");
  } else {
    Serial.println("[CAL] 4) Type YES. The pump does 500 ms bursts with a short pause.");
    Serial.println("[CAL]    Type STOP when you can read a proper volume (about 2 mL+).");
    Serial.println("[CAL] That burst count is replayed 3 times → mL / 500 ms burst.");
  }
  Serial.println("[CAL] Type SKIP to leave this pump unchanged, or ABORT.");
}

void beginCalSequence(bool allPumps, PumpCal::Id only) {
  if (softPrimeActive()) {
    finishManualPrime("starting CAL", false);
  }
  stopAllDosingPumps();
  resetCalPumpStats();
  calSequence = allPumps;
  calSequenceIndex = 0;
  calPump = allPumps ? PumpCal::WATER : only;
  calPhase = CAL_WAIT_READY;
  setCalMqttStatus("entered");
#if ENABLE_MQTT
  // Drop the broker for the session so telemetry and dashboard commands
  // cannot interrupt Serial CAL. The network task reconnects on finish.
  Comms::setCalPause(true);
#endif
  Serial.println("[CAL] Garden paused — sensors, MQTT, drain, and auto");
  Serial.println("      dosing are off until ABORT or the last pump is done.");
  promptCalReady();
}

void finishCalSession() {
  calPhase = CAL_IDLE;
  calSequence = false;
  stopAllDosingPumps();
  setCalMqttStatus("finished");
  Serial.println("[CAL] Session finished. Sensors, MQTT, and control resume.");
  PumpCal::printRates();
  PumpCal::printStock();
#if ENABLE_MQTT
  Comms::setCalPause(false);
#endif
}

void advanceCalSequence() {
  if (!calSequence) {
    finishCalSession();
    return;
  }
  calSequenceIndex++;
  if (calSequenceIndex >= PumpCal::COUNT) {
    finishCalSession();
    return;
  }
  calPump = static_cast<PumpCal::Id>(calSequenceIndex);
  resetCalPumpStats();
  calPhase = CAL_WAIT_READY;
  promptCalReady();
}

void startCalBurstOn(unsigned long now) {
  calBurstOn = true;
  calRunStartedAt = now;
  startCalPump(calPump);
}

void startCalBurstRun() {
  stopAllDosingPumps();
  calBurstsDone = 0;
  calBurstOn = false;
  calStopRequested = false;
  calPhase = CAL_RUNNING;
  setCalMqttStatus("running");
  Serial.print("[CAL] ");
  if (calIsContinuous()) {
    if (calTargetMs == 0) {
      Serial.print("Continuous discovery on ");
      Serial.print(PumpCal::name(calPump));
      Serial.println(". Type STOP when the cup is readable (max 8 s).");
    } else {
      Serial.print("Confirm ");
      Serial.print(calRateCount);
      Serial.print("/");
      Serial.print(CAL_CONFIRM_RUNS);
      Serial.print(" — ");
      Serial.print(calTargetMs);
      Serial.println(" ms continuous.");
    }
  } else if (calTargetBursts == 0) {
    Serial.print("Discovery bursts on ");
    Serial.print(PumpCal::name(calPump));
    Serial.println(" (500 ms each). Type STOP when the cup is readable.");
  } else {
    Serial.print("Confirm ");
    Serial.print(calRateCount);
    Serial.print("/");
    Serial.print(CAL_CONFIRM_RUNS);
    Serial.print(" — ");
    Serial.print(calTargetBursts);
    Serial.println(" x 500 ms.");
  }
  startCalBurstOn(millis());
}

void finishCalMeasure() {
  stopCalPump(calPump);
  calBurstOn = false;
  if (calIsContinuous()) {
    if (calPulseMs < 200) {
      calPhase = CAL_WAIT_READY;
      Serial.println("[CAL] Run too short. Empty the cup and type YES.");
      return;
    }
  } else {
    calPulseMs = static_cast<unsigned long>(calBurstsDone) * CAL_BURST_MS;
    if (calBurstsDone < 1 || calPulseMs == 0) {
      calPhase = CAL_WAIT_READY;
      Serial.println("[CAL] No complete 500 ms burst. Empty the cup and type YES.");
      return;
    }
  }
  calPhase = CAL_WAIT_VOLUME;
  setCalMqttStatus("wait_volume");
  Serial.print("[CAL] Run done: ");
  if (calIsContinuous()) {
    Serial.print(calPulseMs);
    Serial.print(" ms continuous (");
    Serial.print(calPulseMs / 1000.0f, 2);
    Serial.println(" s). Read the cup.");
  } else {
    Serial.print(calBurstsDone);
    Serial.print(" x 500 ms = ");
    Serial.print(calPulseMs);
    Serial.print(" ms (");
    Serial.print(calPulseMs / 1000.0f, 2);
    Serial.println(" s). Read the cup.");
  }
  Serial.println("[CAL] Type the volume in mL (0.1 to 10), or RETRY / SKIP / ABORT.");
}

void startPrime() {
  stopAllDosingPumps();
  calRunStartedAt = millis();
  calPhase = CAL_PRIMING;
  setCalMqttStatus("priming");
  startCalPump(calPump);
  Serial.print("[CAL] Priming ");
  Serial.print(PumpCal::name(calPump));
  Serial.print(" (");
  Serial.print(PumpCal::gpioHint(calPump));
  Serial.println(").");
  Serial.println("[CAL] Watch the outlet. Type STOP when liquid comes out (not air).");
  Serial.print("[CAL] Auto-stops after ");
  Serial.print(CAL_PRIME_MAX_MS / 1000UL);
  Serial.println(" s if you do not type STOP.");
}

void finishPrime(const char* reason) {
  const unsigned long primeMs = millis() - calRunStartedAt;
  stopCalPump(calPump);
  PumpCal::consumeRunMs(calPump, primeMs);
  calPhase = CAL_WAIT_READY;
  setCalMqttStatus("prime_done");
  Serial.print("[CAL] Prime stopped (");
  Serial.print(reason);
  Serial.println(").");
  Serial.println("[CAL] Empty / discard the waste cup. Dry the 10 mL cup.");
  Serial.println("[CAL] When the outlet is a solid liquid stream and the 10 mL");
  Serial.print("      cup is empty on the outlet, type YES for ");
  Serial.println(calIsContinuous() ? "the continuous cup run." : "500 ms bursts.");
}

void updatePumpCalibration(unsigned long now) {
  if (calPhase == CAL_PRIMING) {
    startCalPump(calPump);
    if (now - calRunStartedAt >= CAL_PRIME_MAX_MS) {
      finishPrime("safety timeout — type PRIME again if still air");
      return;
    }
    static unsigned long lastPrimeNoteMs = 0;
    if (now - lastPrimeNoteMs >= 5000UL) {
      lastPrimeNoteMs = now;
      Serial.print("[CAL] Still priming ");
      Serial.print(PumpCal::name(calPump));
      Serial.print(" (");
      Serial.print((now - calRunStartedAt) / 1000UL);
      Serial.println(" s). Type STOP when liquid reaches the outlet.");
    }
    return;
  }

  if (calPhase != CAL_RUNNING) {
    return;
  }

  if (calIsContinuous()) {
    startCalPump(calPump);
    const unsigned long elapsed = now - calRunStartedAt;
    const unsigned long limit = calTargetMs > 0 ? calTargetMs : CAL_WATER_MAX_MS;
    if (!calStopRequested && elapsed < limit) {
      return;
    }
    calPulseMs = elapsed;
    if (calTargetMs > 0 && calPulseMs > calTargetMs) {
      calPulseMs = calTargetMs;
    }
    if (calTargetMs == 0 && elapsed >= CAL_WATER_MAX_MS) {
      Serial.println("[CAL] Hit 8 s cap — read the cup (or RETRY and STOP sooner).");
    }
    finishCalMeasure();
    return;
  }

  if (calBurstOn) {
    startCalPump(calPump);
    if (now - calRunStartedAt < CAL_BURST_MS) {
      return;
    }
    stopCalPump(calPump);
    calBurstOn = false;
    calBurstsDone++;
    calRunStartedAt = now;
    Serial.print("[CAL] Burst ");
    Serial.print(calBurstsDone);
    if (calTargetBursts > 0) {
      Serial.print("/");
      Serial.print(calTargetBursts);
    }
    Serial.println(" (500 ms) done.");

    const bool discoveryDone =
        calTargetBursts == 0 &&
        (calStopRequested || calBurstsDone >= CAL_MAX_BURSTS);
    const bool confirmDone =
        calTargetBursts > 0 && calBurstsDone >= calTargetBursts;
    if (discoveryDone || confirmDone) {
      if (calTargetBursts == 0 && calBurstsDone >= CAL_MAX_BURSTS) {
        Serial.println("[CAL] Hit max bursts — read the cup (or RETRY with STOP sooner).");
      }
      finishCalMeasure();
    }
    return;
  }

  if (now - calRunStartedAt < CAL_BURST_PAUSE_MS) {
    return;
  }
  if (calTargetBursts == 0 && calStopRequested) {
    finishCalMeasure();
    return;
  }
  startCalBurstOn(now);
}

bool parseVolumeMl(const String& line, float& outMl) {
  bool seenDigit = false;
  for (unsigned int i = 0; i < line.length(); i++) {
    char c = line[i];
    if (c >= '0' && c <= '9') {
      seenDigit = true;
    } else if (c != '.' && c != '-' && c != '+') {
      return false;
    }
  }
  if (!seenDigit) {
    return false;
  }
  outMl = line.toFloat();
  return true;
}

void acceptCalVolume(float volumeMl) {
  if (volumeMl < 0.1f || volumeMl > 10.0f) {
    setCalMqttStatus("bad_volume", false);
    Serial.println("[CAL] Volume must be between 0.1 and 10.0 mL (10 mL cup).");
    return;
  }
  if (calPulseMs < 200) {
    setCalMqttStatus("no_runtime", false);
    Serial.println("[CAL] No on-time recorded. Type YES to run again.");
    return;
  }
  PumpCal::consumeMl(calPump, volumeMl);

  float sample = 0.0f;
  if (calIsContinuous()) {
    const float seconds = static_cast<float>(calPulseMs) / 1000.0f;
    sample = volumeMl / seconds;
    if (calTargetMs == 0) {
      calTargetMs = calPulseMs;
    }
  } else {
    if (calBurstsDone < 1) {
      setCalMqttStatus("no_burst", false);
      Serial.println("[CAL] No complete burst. Type YES to run again.");
      return;
    }
    sample = volumeMl / static_cast<float>(calBurstsDone);
    if (calTargetBursts == 0) {
      calTargetBursts = calBurstsDone;
    }
  }
  calRateSum += sample;
  calRateCount++;
  calLastSample = sample;
  setCalMqttStatus("volume_accepted");

  Serial.print("[CAL] Run ");
  Serial.print(calRateCount);
  Serial.print("/");
  Serial.print(1 + CAL_CONFIRM_RUNS);
  Serial.print(": ");
  Serial.print(volumeMl, 2);
  if (calIsContinuous()) {
    Serial.print(" mL / ");
    Serial.print(calPulseMs / 1000.0f, 2);
    Serial.print(" s = ");
    Serial.print(sample, 3);
    Serial.println(" mL/s");
  } else {
    Serial.print(" mL / ");
    Serial.print(calBurstsDone);
    Serial.print(" bursts = ");
    Serial.print(sample, 3);
    Serial.println(" mL/burst");
  }

  if (calRateCount < 1 + CAL_CONFIRM_RUNS) {
    calPhase = CAL_WAIT_READY;
    setCalMqttStatus("need_confirm_run");
    promptCalReady();
    return;
  }

  const float average = calRateSum / static_cast<float>(calRateCount);
  PumpCal::setRate(calPump, average);
  setCalMqttStatus("saved");
  Serial.print("[CAL] ");
  Serial.print(PumpCal::name(calPump));
  Serial.print(" average of ");
  Serial.print(calRateCount);
  Serial.print(" runs = ");
  Serial.print(average, 3);
  Serial.println(calIsContinuous() ? " mL/s (saved to LittleFS)"
                                  : " mL/burst (saved to LittleFS)");
  resetCalPumpStats();
  advanceCalSequence();
}
