/* Water level, pH, and nutrient control loops. */
#include "garden.h"

/************************************************************
 * Water Refill Control
 ************************************************************/

bool waterAdditionAllowed() {
  #if ENABLE_WATER_LEVEL
  // Unknown level: do not add water. At/above net-pot touch: do not add water.
  if (waterLevelPercent < 0 || waterLevelPercent >= waterCeilingPercent()) {
    return false;
  }
  #endif
  return true;
}

bool chemistryBusy() {
  return phControlState != PH_CONTROL_IDLE ||
         nutrientControlState != NUTRIENT_CONTROL_IDLE;
}

void enforceWaterLevelCeiling() {
  #if ENABLE_PUMPS && ENABLE_WATER_LEVEL
  if (waterAdditionAllowed()) {
    return;
  }

  bool stoppedWater = false;

  // Stop only forward water adds. TEMPORARY reverse drain may still run.
  if (waterRefillPumpRunning) {
    pump1.write(SERVO_STOP);
    waterRefillLastRunMs = millis() - waterRefillStartedAt;
    PumpCal::consumeRunMs(PumpCal::WATER, waterRefillLastRunMs);
    waterRefillPumpRunning = false;
    waterSafetyRefillActive = false;
    waterLevelControlAllowedAt = millis() + WATER_LEVEL_SETTLE_MS;
    stoppedWater = true;
    onStagedWaterComplete(millis());  // a staged top-up ends here too (no-op otherwise)
  }

  if (nutrientControlState == NUTRIENT_DILUTING) {
    pump1.write(SERVO_STOP);
    PumpCal::consumeRunMs(PumpCal::WATER, millis() - nutrientControlStateStartedAt);
    nutrientControlState = NUTRIENT_MIXING;
    nutrientControlStateStartedAt = millis();
    nutrientControlAllowedAt = millis() + NUTRIENT_SETTLE_MS;
    stoppedWater = true;
  }

  if (stoppedWater) {
    Serial.print("[CONTROL] Fresh-water pump stopped - level ");
    Serial.print(waterLevelPercent);
    Serial.print("% is at/above net-pot touch ");
    Serial.print(waterCeilingPercent());
    Serial.println("%");
  }
  #endif
}

bool pump1ForwardBusy() {
  return waterRefillPumpRunning ||
         nutrientControlState == NUTRIENT_DILUTING;
}

bool waterPumpBusy() {
  return waterRefillPumpRunning || waterDrainPumpRunning;
}

// TEMPORARY reverse-drain window: actively reversing, or still above the
// ceiling between bursts. pH dosing must stay off — shared inlet would pull
// acid/base back toward the source reservoir.
bool reverseDrainModeActive() {
  #if ENABLE_WATER_LEVEL && ENABLE_TEMPORARY_REVERSE_DRAIN
  return waterDrainPumpRunning ||
         (waterLevelPercent > waterCeilingPercent());
  #else
  return false;
  #endif
}

void consumeActivePhBursts() {
  PumpCal::Id id = phControlState == PH_DOSING_DOWN ? PumpCal::PH_DOWN : PumpCal::PH_UP;
  if (phBurstOn) {
    const unsigned long ran = millis() - phBurstMark;
    PumpCal::consumeRunMs(id, ran);
  }
  phBurstOn = false;
  phBurstDone = 0;
  phBurstTarget = 0;
}

void pausePHDosingForReverseDrain(unsigned long now) {
  #if ENABLE_PUMPS
  if (phControlState != PH_DOSING_UP && phControlState != PH_DOSING_DOWN) {
    return;
  }

  consumeActivePhBursts();
  pump3.write(SERVO_STOP);
  pump4.write(SERVO_STOP);
  phControlState = PH_CONTROL_IDLE;
  phControlAllowedAt = now;

  Serial.print("[CONTROL][TEMPORARY] pH dosing paused - reverse drain active (level ");
  Serial.print(waterLevelPercent);
  Serial.print("%, touch ");
  Serial.print(waterCeilingPercent());
  Serial.println("%); will resume when level is in range");
  #endif
}

// TEMPORARY: intermittent reverse bursts until level is at/below ceiling.
// Highest priority while over-max (root air gap). Starts immediately when the
// ceiling is crossed; short settle only applies between later bursts.
void controlWaterDrain(unsigned long now) {
  #if ENABLE_PUMPS && ENABLE_WATER_LEVEL && ENABLE_TEMPORARY_REVERSE_DRAIN
  if (pumpCalibrationActive() || isWaterLevelSensorFault()) {
    return;
  }
  if (waterLevelPercent < 0) {
    return;
  }

  // Stop once we have reached the ceiling target.
  if (waterLevelPercent <= waterCeilingPercent()) {
    waterLevelOverMaxLatched = false;
    if (waterDrainPumpRunning) {
      pump1.write(SERVO_STOP);
      waterDrainPumpRunning = false;
      waterDrainControlAllowedAt = now;
      Serial.print("[CONTROL][TEMPORARY] Drain stopped - level ");
      Serial.print(waterLevelPercent);
      Serial.print("% is at/below ");
      Serial.print(waterCeilingPercent());
      Serial.println("%");
    }
    return;
  }

  // Shared pump inlet: never dose pH while reversing water out of the reservoir.
  pausePHDosingForReverseDrain(now);

  if (waterDrainPumpRunning) {
    // Keep commanding reverse in case a forward path briefly clobbered pump1.
    pump1.write(WATER_DRAIN_RUN_SPEED);
    return;
  }

  // Crossing above the ceiling always starts drain now — root air gap first.
  // Settle pauses only apply to follow-up bursts while still over-max.
  const bool newlyOverMax = !waterLevelOverMaxLatched;
  waterLevelOverMaxLatched = true;
  if (!newlyOverMax &&
      static_cast<long>(now - waterDrainControlAllowedAt) < 0) {
    return;
  }
  if (newlyOverMax) {
    waterDrainControlAllowedAt = now;
  }

  // Over-full takes priority: release any forward owner of pump1, then reverse.
  if (pump1ForwardBusy()) {
    if (waterRefillPumpRunning) {
      waterRefillLastRunMs = now - waterRefillStartedAt;
      PumpCal::consumeRunMs(PumpCal::WATER, waterRefillLastRunMs);
      waterRefillPumpRunning = false;
      waterSafetyRefillActive = false;
      waterLevelControlAllowedAt = now + WATER_LEVEL_SETTLE_MS;
    }
    if (nutrientControlState == NUTRIENT_DILUTING) {
      PumpCal::consumeRunMs(PumpCal::WATER, now - nutrientControlStateStartedAt);
      nutrientControlState = NUTRIENT_MIXING;
      nutrientControlStateStartedAt = now;
      nutrientControlAllowedAt = now + NUTRIENT_SETTLE_MS;
    }
    Serial.println("[CONTROL][TEMPORARY] Preempting fresh-water add to drain over-full level");
  }

  pump1.write(WATER_DRAIN_RUN_SPEED);
  waterDrainPumpRunning = true;
  waterDrainStartedAt = now;

  Serial.print("[CONTROL][TEMPORARY] Water level ");
  Serial.print(waterLevelPercent);
  Serial.print("% is above ");
  Serial.print(waterCeilingPercent());
  Serial.print("%; draining backwards for ");
  Serial.print(WATER_DRAIN_INCREMENT_MS);
  Serial.println(" ms (root air gap priority; watch source reservoir for overflow)");
  #endif
}

// Legacy control paths — superseded by StageController (shared/stage_controller.h).
#if 0
// DWC water pump: OFF during normal consumption. ON only to climb from the
// absolute safety floor up to the stage air-gap refill line (hysteresis).
void controlWaterLevel(int levelPercent, unsigned long now) {
  #if ENABLE_PUMPS && ENABLE_WATER_LEVEL
  if (pumpCalibrationActive() || !waterAutoControlAllowedByMode()) {
    return;
  }
  const GrowPreset& preset = activePreset();

  // A negative value means the level sensor did not provide a usable sample.
  // Never act on missing sensor data.
  if (!preset.waterSafetyEnabled || levelPercent < 0) {
    if (levelPercent < 0) {
      Serial.println("[WARN] Water level control skipped - water level is unavailable");
    }
    return;
  }

  // Never run the water pump while pH / nutrient dosing or settling.
  if (waterPumpBusy() ||
      chemistryBusy() ||
      static_cast<long>(now - waterLevelControlAllowedAt) < 0) {
    return;
  }

  const int ceilingPct = waterCeilingPercent();
  int floorPct = waterSafetyFloorPercent();
  int refillToPct = waterRefillToPercent();
  if (refillToPct <= floorPct) {
    refillToPct = Tank::clampPercent(floorPct + 5);
  }

  if (levelPercent >= ceilingPct) {
    waterSafetyRefillActive = false;
    return;
  }

  if (!waterSafetyRefillActive) {
    if (levelPercent > floorPct) {
      // Natural plant consumption — leave the air gap alone.
      return;
    }
    waterSafetyRefillActive = true;
    Serial.print("[CONTROL] ");
    Serial.print(preset.name);
    Serial.print(" safety floor ");
    Serial.print(floorPct);
    Serial.print("% (");
    Serial.print(Tank::SAFETY_FLOOR_AIR_GAP_MM + Tank::SAFETY_HYSTERESIS_MM);
    Serial.print(" mm below pots) hit at ");
    Serial.print(levelPercent);
    Serial.print("%; refilling toward ");
    Serial.print(refillToPct);
    Serial.print("% (");
    Serial.print(preset.airGapTargetMm);
    Serial.println(" mm air gap)");
  }

  if (levelPercent >= refillToPct) {
    waterSafetyRefillActive = false;
    ControlLaw::reset(waterPidState);
    Serial.print("[CONTROL] Safety refill complete at ");
    Serial.print(levelPercent);
    Serial.print("% (target air gap ");
    Serial.print(preset.airGapTargetMm);
    Serial.println(" mm)");
    return;
  }

  const float errorPct = static_cast<float>(refillToPct - levelPercent);
  activeWaterRefillMs = plannedRunMs(
      PumpCal::WATER,
      waterDoseMl(errorPct),
      WATER_REFILL_INCREMENT_MS,
      PUMP_MAX_RUN_MS_WATER);
  if (!reservoirAllows(PumpCal::WATER,
                       PumpCal::volumeForRunMs(PumpCal::WATER, activeWaterRefillMs),
                       "safety top-up")) {
    return;
  }
  pump1.write(WATER_REFILL_RUN_SPEED);
  waterRefillPumpRunning = true;
  waterRefillStartedAt = now;

  Serial.print("[CONTROL] ");
  Serial.print(preset.name);
  Serial.print(" ");
  Serial.print(ControlLaw::name(ACTIVE_CONTROL_LAW));
  Serial.print(" safety top-up ");
  Serial.print(levelPercent);
  Serial.print("% → ");
  Serial.print(refillToPct);
  Serial.print("% for ");
  Serial.print(activeWaterRefillMs);
  Serial.println(" ms");
  #endif
}

// Returns false when pH-down auto dosing must not run (logs the reason once).
bool allowPHDownDose(float currentPH, float phMax, unsigned long now) {
  if (phDownAutoDisabled) {
    return false;
  }

  if (currentPH > phMax + PH_AUTO_DOWN_MAX_DELTA) {
    phDownAutoDisabled = true;
    Serial.print("[WARN] pH ");
    Serial.print(currentPH, 2);
    Serial.print(" is more than ");
    Serial.print(PH_AUTO_DOWN_MAX_DELTA, 1);
    Serial.print(" above target max ");
    Serial.print(phMax, 1);
    Serial.println(" — auto pH-down DISABLED until reboot. "
                   "Adjust manually with a test kit, mix well, then reflash/power-cycle.");
    return false;
  }

  if (phDownDosesThisBoot >= phMaxDownDosesPerBoot()) {
    phDownAutoDisabled = true;
    Serial.print("[WARN] Auto pH-down hit boot limit (");
    Serial.print(phMaxDownDosesPerBoot());
    Serial.print(" doses, mode=");
    Serial.print(hydroponicModeName());
    Serial.println(") — DISABLED until reboot. Check probe calibration and mix the reservoir.");
    return false;
  }

  if (phDownDoseWindowStartedAt == 0 ||
      now - phDownDoseWindowStartedAt >= phDownDoseWindowMs()) {
    phDownDoseWindowStartedAt = now;
    phDownDosesThisWindow = 0;
  }

  if (phDownDosesThisWindow >= phMaxDownDosesPerWindow()) {
    phControlPausedUntil = now + phStuckPauseMs();
    Serial.print("[WARN] Auto pH-down window limit (");
    Serial.print(phMaxDownDosesPerWindow());
    Serial.print(" doses / ");
    printDurationMs(phDownDoseWindowMs());
    Serial.print(", mode=");
    Serial.print(hydroponicModeName());
    Serial.print(") — paused for ");
    printDurationMs(phStuckPauseMs());
    Serial.println(".");
    return false;
  }

  return true;
}

void controlPH(float currentPH, unsigned long now) {
  #if ENABLE_PUMPS && ENABLE_PH_TDS
  if (pumpCalibrationActive() || !phControlAllowedByMode()) {
    return;
  }
  const GrowPreset& preset = activePreset();

  if (currentPH >= preset.phMin && currentPH <= preset.phMax) {
    phReadingBeforeLastDownDose = NAN;
    phDownDosesWithoutImprovement = 0;
    ControlLaw::reset(phPidState);
  }

  if (static_cast<long>(now - phControlPausedUntil) < 0) {
    return;
  }

  // Interlock: never dose pH while the water pump is running (or reverse drain).
  if (!preset.phControlEnabled ||
      phControlState != PH_CONTROL_IDLE ||
      waterPumpBusy() ||
      waterSafetyRefillActive ||
      reverseDrainModeActive() ||
      nutrientControlState != NUTRIENT_CONTROL_IDLE ||
      static_cast<long>(now - phControlAllowedAt) < 0) {
    return;
  }

  if (currentPH < preset.phMin) {
    // Explicitly stop both pumps first so an incomplete previous operation can
    // never leave pH-up and pH-down running at the same time.
    pump3.write(SERVO_STOP);
    pump4.write(SERVO_STOP);
    const float phMid = 0.5f * (preset.phMin + preset.phMax);
    phBurstTarget = plannedBursts(
        PumpCal::PH_UP,
        phDoseMl(phMid - currentPH),
        CAL_FALLBACK_BURSTS,
        CAL_MAX_DOSE_BURSTS_PH);
    if (!reservoirAllows(PumpCal::PH_UP,
                         PumpCal::volumeForBursts(PumpCal::PH_UP, phBurstTarget),
                         "dosing pH-up")) {
      return;
    }
    phBurstDone = 0;
    phBurstOn = true;
    phBurstMark = now;
    activePhDoseMs = static_cast<unsigned long>(phBurstTarget) * CAL_BURST_MS;
    pump3.write(PH_UP_RUN_SPEED);
    phControlState = PH_DOSING_UP;
    phControlStateStartedAt = now;

    Serial.print("[CONTROL] ");
    Serial.print(preset.name);
    Serial.print(" ");
    Serial.print(ControlLaw::name(ACTIVE_CONTROL_LAW));
    Serial.print(" pH ");
    Serial.print(currentPH, 2);
    Serial.print(" is below target ");
    Serial.print(preset.phMin, 1);
    Serial.print("-");
    Serial.print(preset.phMax, 1);
    Serial.print("; dosing pH-up ");
    Serial.print(phBurstTarget);
    Serial.print(" x 500 ms (");
    Serial.print(hydroponicModeName());
    Serial.println(")");
  } else if (currentPH > preset.phMax) {
    if (!allowPHDownDose(currentPH, preset.phMax, now)) {
      return;
    }

    if (!isnan(phReadingBeforeLastDownDose) &&
        currentPH >= phReadingBeforeLastDownDose - PH_STUCK_MIN_IMPROVEMENT) {
      phDownDosesWithoutImprovement++;
    } else if (!isnan(phReadingBeforeLastDownDose)) {
      phDownDosesWithoutImprovement = 0;
    }

    if (phDownDosesWithoutImprovement >= PH_STUCK_DOSE_LIMIT) {
      phControlPausedUntil = now + phStuckPauseMs();
      phDownDosesWithoutImprovement = 0;
      phReadingBeforeLastDownDose = NAN;
      Serial.print("[WARN] pH still ");
      Serial.print(currentPH, 2);
      Serial.print(" after ");
      Serial.print(PH_STUCK_DOSE_LIMIT);
      Serial.print(" pH-down cycles — auto dosing paused for ");
      printDurationMs(phStuckPauseMs());
      Serial.print(" (");
      Serial.print(hydroponicModeName());
      Serial.println("). Mix the reservoir, check with a liquid test kit, "
                     "and calibrate the probe (ENTERPH / MQTT ph_cal).");
      return;
    }

    phReadingBeforeLastDownDose = currentPH;
    pump3.write(SERVO_STOP);
    pump4.write(SERVO_STOP);
    const float phMidDown = 0.5f * (preset.phMin + preset.phMax);
    phBurstTarget = plannedBursts(
        PumpCal::PH_DOWN,
        phDoseMl(currentPH - phMidDown),
        CAL_FALLBACK_BURSTS,
        CAL_MAX_DOSE_BURSTS_PH);
    if (!reservoirAllows(PumpCal::PH_DOWN,
                         PumpCal::volumeForBursts(PumpCal::PH_DOWN, phBurstTarget),
                         "dosing pH-down")) {
      return;
    }
    phDownDosesThisBoot++;
    phDownDosesThisWindow++;
    phBurstDone = 0;
    phBurstOn = true;
    phBurstMark = now;
    activePhDoseMs = static_cast<unsigned long>(phBurstTarget) * CAL_BURST_MS;
    pump4.write(PH_DOWN_RUN_SPEED);
    phControlState = PH_DOSING_DOWN;
    phControlStateStartedAt = now;

    Serial.print("[CONTROL] ");
    Serial.print(preset.name);
    Serial.print(" ");
    Serial.print(hydroponicModeName());
    Serial.print(" pH ");
    Serial.print(currentPH, 2);
    Serial.print(" is above target ");
    Serial.print(preset.phMin, 1);
    Serial.print("-");
    Serial.print(preset.phMax, 1);
    Serial.print("; dosing pH-down (boot ");
    Serial.print(phDownDosesThisBoot);
    Serial.print("/");
    Serial.print(phMaxDownDosesPerBoot());
    Serial.print(", window ");
    Serial.print(phDownDosesThisWindow);
    Serial.print("/");
    Serial.print(phMaxDownDosesPerWindow());
    Serial.println(")");
  }
  #endif
}

#endif  // legacy controlWaterLevel / controlPH

void updatePHControl(unsigned long now) {
  #if ENABLE_PUMPS
  if (phControlState == PH_CONTROL_IDLE) {
    return;
  }

  // Abort an in-progress acid/base dose if reverse drain starts mid-cycle.
  if (reverseDrainModeActive() &&
      (phControlState == PH_DOSING_UP || phControlState == PH_DOSING_DOWN)) {
    pausePHDosingForReverseDrain(now);
    return;
  }

  if (phControlState == PH_DOSING_UP || phControlState == PH_DOSING_DOWN) {
    PumpCal::Id doseId =
        phControlState == PH_DOSING_DOWN ? PumpCal::PH_DOWN : PumpCal::PH_UP;
    // The motor is stopped by the dose timer at exactly activePhDoseMs; the
    // loop only books the dose. It does not rewrite the run speed, which could
    // restart a pump the timer has just stopped. If the timer never fires, the
    // loop stops the pump itself after a short grace period.
    const unsigned long phElapsed = now - phBurstMark;
    if (phBurstOn && !doseStopTimerFired() &&
        phElapsed < activePhDoseMs + DOSE_TIMER_GRACE_MS) {
      return;
    }

    const unsigned long ran = doseStopTimerFired() ? activePhDoseMs : phElapsed;
    pump3.write(SERVO_STOP);
    pump4.write(SERVO_STOP);
    const float phDelivered = phBurstOn ? PumpCal::volumeForRunMs(doseId, ran) : 0.0f;
    if (phBurstOn) {
      PumpCal::consumeRunMs(doseId, ran);
    }
    phBurstOn = false;
    phBurstDone = 0;
    phBurstTarget = 0;
    pump3.write(SERVO_STOP);
    pump4.write(SERVO_STOP);

    // Capture the completed dose in the journal/plotter before leaving the
    // dosing state. Journal only: the dose itself reaches the Pi and Influx
    // as a control event, and this row repeats the last reading's values.
    saveSensorDataToCSV();
    printPlotterData();

    if (stagedPhDose) {
      onStagedPhComplete(now, phDelivered);
      phControlState = PH_CONTROL_IDLE;
      return;
    }

    // No fresh-water chase — GPIO18 is reserved for drawdown top-up only.
    phControlState = PH_MIXING;
    phControlStateStartedAt = now;
    phControlAllowedAt = now + phSettleMs();
    Serial.print("[CONTROL] pH dose complete; waiting ");
    printDurationMs(phSettleMs());
    Serial.print(" for DWC mixing (air stone) before next pH check");
    Serial.println();
    return;
  }

  if (phControlState == PH_MIXING &&
      static_cast<long>(now - phControlAllowedAt) >= 0) {
    phControlState = PH_CONTROL_IDLE;
    Serial.println("[CONTROL] pH mixing delay complete; next sensor cycle may dose");
  }
  #endif
}

#if 0  // legacy controlNutrients
void controlNutrients(int currentTdsPpm, unsigned long now) {
  #if ENABLE_PUMPS && ENABLE_PH_TDS
  if (pumpCalibrationActive() || !nutrientsAllowedByMode()) {
    return;
  }
  const GrowPreset& preset = activePreset();

  if (nutrientControlState != NUTRIENT_CONTROL_IDLE ||
      waterPumpBusy() ||
      waterSafetyRefillActive ||
      phControlState != PH_CONTROL_IDLE ||
      static_cast<long>(now - nutrientControlAllowedAt) < 0) {
    return;
  }

  if (currentTdsPpm >= preset.nutrientTdsMinPpm &&
      currentTdsPpm <= preset.nutrientTdsMaxPpm) {
    ControlLaw::reset(nutrientPidState);
  }

  if (preset.nutrientDosingEnabled && currentTdsPpm < preset.nutrientTdsMinPpm) {
    // Nutrient pump is pump2 — never co-run with the water pump (interlock above).
    const float tdsMid =
        0.5f * (preset.nutrientTdsMinPpm + preset.nutrientTdsMaxPpm);
    nutrientBurstTarget = plannedBursts(
        PumpCal::NUTRIENT,
        nutrientDoseMl(tdsMid - static_cast<float>(currentTdsPpm)),
        CAL_FALLBACK_BURSTS,
        CAL_MAX_DOSE_BURSTS_NUTRIENT);
    if (!reservoirAllows(PumpCal::NUTRIENT,
                         PumpCal::volumeForBursts(PumpCal::NUTRIENT, nutrientBurstTarget),
                         "dosing nutrient")) {
      return;
    }
    nutrientBurstDone = 0;
    nutrientBurstOn = true;
    nutrientBurstMark = now;
    activeNutrientDoseMs =
        static_cast<unsigned long>(nutrientBurstTarget) * CAL_BURST_MS;
    pump2.write(NUTRIENT_RUN_SPEED);
    nutrientControlState = NUTRIENT_DOSING;
    nutrientControlStateStartedAt = now;

    Serial.print("[CONTROL] ");
    Serial.print(preset.name);
    Serial.print(" TDS ");
    Serial.print(currentTdsPpm);
    Serial.print(" ppm is below target ");
    Serial.print(preset.nutrientTdsMinPpm);
    Serial.print("-");
    Serial.print(preset.nutrientTdsMaxPpm);
    Serial.println(" ppm; dosing nutrients");
  } else if (preset.nutrientDilutionEnabled) {
    const float diluteTriggerPpm =
        static_cast<float>(preset.nutrientTdsMaxPpm) * EC_DILUTE_OVERSHOOT_RATIO;
    if (static_cast<float>(currentTdsPpm) <= diluteTriggerPpm) {
      return;
    }
    if (!waterAdditionAllowed() || waterDrainPumpRunning) {
      Serial.print("[CONTROL] Nutrient dilution blocked - level ");
      Serial.print(waterLevelPercent);
      Serial.print("% / drain running; touch ceiling ");
      Serial.print(waterCeilingPercent());
      Serial.println("%");
      return;
    }

    const float tdsMidHi =
        0.5f * (preset.nutrientTdsMinPpm + preset.nutrientTdsMaxPpm);
    activeNutrientDiluteMs = plannedRunMs(
        PumpCal::WATER,
        waterDoseMl((static_cast<float>(currentTdsPpm) - tdsMidHi) * 0.05f),
        NUTRIENT_DILUTE_INCREMENT_MS,
        PUMP_MAX_RUN_MS_WATER);
    if (!reservoirAllows(PumpCal::WATER,
                         PumpCal::volumeForRunMs(PumpCal::WATER, activeNutrientDiluteMs),
                         "diluting")) {
      return;
    }
    pump1.write(WATER_REFILL_RUN_SPEED);
    nutrientControlState = NUTRIENT_DILUTING;
    nutrientControlStateStartedAt = now;

    Serial.print("[CONTROL] ");
    Serial.print(preset.name);
    Serial.print(" TDS ");
    Serial.print(currentTdsPpm);
    Serial.print(" ppm is > ");
    Serial.print(static_cast<int>(diluteTriggerPpm + 0.5f));
    Serial.print(" ppm (");
    Serial.print(static_cast<int>((EC_DILUTE_OVERSHOOT_RATIO - 1.0f) * 100.0f + 0.5f));
    Serial.print("% over max ");
    Serial.print(preset.nutrientTdsMaxPpm);
    Serial.println("); diluting with fresh water");
  }
  #endif
}
#endif  // legacy controlNutrients

void updateNutrientControl(unsigned long now) {
  #if ENABLE_PUMPS
  if (nutrientControlState == NUTRIENT_CONTROL_IDLE) {
    return;
  }

  if (nutrientControlState == NUTRIENT_DOSING) {
    // Stopped by the dose timer (see updatePHControl); the loop only books it.
    const unsigned long nutrientElapsed = now - nutrientBurstMark;
    if (nutrientBurstOn && !doseStopTimerFired() &&
        nutrientElapsed < activeNutrientDoseMs + DOSE_TIMER_GRACE_MS) {
      return;
    }

    const unsigned long ran =
        doseStopTimerFired() ? activeNutrientDoseMs : nutrientElapsed;
    pump2.write(SERVO_STOP);
    const float nutrientDelivered =
        nutrientBurstOn ? PumpCal::volumeForRunMs(PumpCal::NUTRIENT, ran) : 0.0f;
    if (nutrientBurstOn) {
      PumpCal::consumeRunMs(PumpCal::NUTRIENT, ran);
    }
    nutrientBurstOn = false;
    nutrientBurstDone = 0;
    nutrientBurstTarget = 0;

    // Journal only (see the pH dose-complete row above).
    saveSensorDataToCSV();
    printPlotterData();

    pump2.write(SERVO_STOP);
    if (stagedEcDose) {
      onStagedEcComplete(now, nutrientDelivered);
      nutrientControlState = NUTRIENT_CONTROL_IDLE;
      return;
    }

    nutrientControlState = NUTRIENT_MIXING;
    nutrientControlStateStartedAt = now;
    nutrientControlAllowedAt = now + NUTRIENT_SETTLE_MS;

    Serial.print("[CONTROL] Nutrient dose complete; waiting ");
    Serial.print(NUTRIENT_SETTLE_MS);
    Serial.println(" ms for mixing before checking TDS");
    return;
  }

  if (nutrientControlState == NUTRIENT_DILUTING &&
      now - nutrientControlStateStartedAt >= activeNutrientDiluteMs) {
    // Journal only (see the pH dose-complete row above).
    saveSensorDataToCSV();
    printPlotterData();

    // Do not stop pump1 if TEMPORARY reverse drain owns it.
    if (!waterDrainPumpRunning) {
      pump1.write(SERVO_STOP);
    }
    PumpCal::consumeRunMs(PumpCal::WATER, activeNutrientDiluteMs);
    nutrientControlState = NUTRIENT_MIXING;
    nutrientControlStateStartedAt = now;
    nutrientControlAllowedAt = now + NUTRIENT_SETTLE_MS;

    Serial.print("[CONTROL] Nutrient dilution complete; waiting ");
    Serial.print(NUTRIENT_SETTLE_MS);
    Serial.println(" ms for mixing before checking TDS");
    return;
  }

  if (nutrientControlState == NUTRIENT_MIXING &&
      static_cast<long>(now - nutrientControlAllowedAt) >= 0) {
    nutrientControlState = NUTRIENT_CONTROL_IDLE;
    Serial.println("[CONTROL] Nutrient mixing delay complete; next sensor cycle may dose");
  }
  #endif
}

void updateWaterRefillPump(unsigned long now) {
  #if ENABLE_PUMPS
  if (!waterRefillPumpRunning) {
    return;
  }
  // A staged top-up also stops as soon as the live level reaches the refill
  // target (read every WATER_LEVEL_PRIORITY_POLL_MS while the pump runs).
  // The smoothed level (median of 30 s) must stay at the target for
  // TOPUP_CUTOFF_HOLD_MS, so one ripple reading cannot end the refill early.
  static unsigned long atTargetSince = 0;
  const int stopAt = stagedTopUpStopPercent();
  const int level = waterLevelSmoothedPercent();
  if (stopAt >= 0 && level >= stopAt) {
    if (atTargetSince == 0) {
      atTargetSince = now;
    }
  } else {
    atTargetSince = 0;
  }
  const bool atTarget =
      atTargetSince != 0 && now - atTargetSince >= ControlDesign::TOPUP_CUTOFF_HOLD_MS;
  if (!atTarget && now - waterRefillStartedAt < activeWaterRefillMs) {
    return;
  }
  atTargetSince = 0;
  if (atTarget) {
    Serial.print("[CONTROL] Water level ");
    Serial.print(level);
    Serial.println(" % reached the refill target - pump stopped");
  }

  pump1.write(SERVO_STOP);
  waterRefillLastRunMs = now - waterRefillStartedAt;
  PumpCal::consumeRunMs(PumpCal::WATER, waterRefillLastRunMs);
  waterRefillPumpRunning = false;

  if (stagedWaterTopUp) {
    onStagedWaterComplete(now);
  } else {
    waterLevelControlAllowedAt = now + WATER_LEVEL_SETTLE_MS;
    Serial.print("[CONTROL] Water top-up complete; waiting ");
    Serial.print(WATER_LEVEL_SETTLE_MS);
    Serial.println(" ms before another refill decision");
  }
  printPlotterData();
  #endif
}

void updateWaterDrainPump(unsigned long now) {
  #if ENABLE_PUMPS && ENABLE_TEMPORARY_REVERSE_DRAIN
  if (!waterDrainPumpRunning) {
    return;
  }

  // Hold reverse for the whole burst; re-assert in case another path wrote STOP.
  if (now - waterDrainStartedAt < WATER_DRAIN_INCREMENT_MS) {
    pump1.write(WATER_DRAIN_RUN_SPEED);
    return;
  }

  pump1.write(SERVO_STOP);
  waterDrainPumpRunning = false;
  waterDrainControlAllowedAt = now + WATER_DRAIN_SETTLE_MS;

  Serial.print("[CONTROL][TEMPORARY] Drain burst complete; pausing ");
  Serial.print(WATER_DRAIN_SETTLE_MS);
  Serial.print(" ms before next burst (target <= ");
  Serial.print(waterCeilingPercent());
  Serial.println(" %)");
  printPlotterData();
  #endif
}
