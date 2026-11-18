/* Boot init: pumps, LittleFS, sensors. */
#include "garden.h"

void initPumps() {
  Serial.println("[DEBUG] Initialising pumps (all stopped)");

  // DFRobot DFR0523 PPM is ~50 Hz. 500 Hz makes the period 2000 µs, so
  // write(180) (~2500 µs) cannot fit and reverse never runs.
  pump1.setPeriodHertz(50);
  pump2.setPeriodHertz(50);
  pump3.setPeriodHertz(50);
  pump4.setPeriodHertz(50);

  pump1.attach(PUMP1_PIN, 500, 2500);
  pump2.attach(PUMP2_PIN, 500, 2500);
  pump3.attach(PUMP3_PIN, 500, 2500);
  pump4.attach(PUMP4_PIN, 500, 2500);

  pump1.write(SERVO_STOP);
  pump2.write(SERVO_STOP);
  pump3.write(SERVO_STOP);
  pump4.write(SERVO_STOP);

  Serial.printf("[DEBUG] Pumps attached on GPIO %d (water), %d (nutrient), %d (pH up), "
                "%d (pH down) — all at STOP (90°)\n",
                PUMP1_PIN, PUMP2_PIN, PUMP3_PIN, PUMP4_PIN);
  #if WATER_PUMP_OFFLINE
  Serial.println("[WARN] Water pump offline — no auto top-ups");
  #endif
  #if PH_DOWN_PUMP_OFFLINE
  Serial.println("[WARN] pH-down pump offline (PCB D5 trace broken) — no auto pH-down");
  #endif
}

bool initLittleFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("[ERROR] LittleFS mount failed — CSV logging disabled");
    return false;
  }
  Serial.print("[DEBUG] LittleFS mounted — CSV path - ");
  Serial.print(CSV_PATH);
  Serial.print("; total=");
  Serial.print(LittleFS.totalBytes());
  Serial.print(" B, used=");
  Serial.print(LittleFS.usedBytes());
  Serial.print(" B, free=");
  Serial.print(LittleFS.totalBytes() - LittleFS.usedBytes());
  Serial.println(" B");
  bootId = SensorDataStore::nextBootId();
  Serial.print("[DEBUG] boot_id ");
  Serial.print(bootId);
  Serial.println(" (with seq: reading identity for CSV, MQTT and Influx)");
  ensureCsvSchema();
  SensorDataStore::loadWaterLevelTrust(waterLevelTrust);
  if (!SensorDataStore::loadNetPotTouchPercent(netPotTouchPercent)) {
    netPotTouchPercent = Tank::NET_POT_TOUCH_DEFAULT_PERCENT;
    Serial.print("[DEBUG] Using default net-pot touch ");
    Serial.print(netPotTouchPercent);
    Serial.println("% until water_fill_cal saves one");
  }
  Serial.print("[DEBUG] Water ceiling (touch) ");
  Serial.print(waterCeilingPercent());
  Serial.print("%; safety floor ");
  Serial.print(waterSafetyFloorPercent());
  Serial.print("% (");
  Serial.print(Tank::SAFETY_FLOOR_AIR_GAP_MM + Tank::SAFETY_HYSTERESIS_MM);
  Serial.print(" mm air gap); stage refill-to ");
  Serial.print(waterRefillToPercent());
  Serial.print("% (");
  Serial.print(GrowStage::config(systemMode).targetDrawdownMm);
  Serial.println(" mm drawdown target)");
  if (waterRefillToPercent() <= waterSafetyFloorPercent()) {
    Serial.println("[WARN] Stage refill-to <= safety floor after clamping — "
                   "re-measure net-pot touch with water_fill_cal (TOUCH)");
  }
  loadAutoControlPaused();
  PumpCal::load();
  PumpCal::printRates();
  PumpCal::printStock();
  if (PhProbeCal::loadStore()) {
    Serial.println("[PH-CAL] Probe calibration ready (LittleFS or the baked default line)");
  } else {
    Serial.println("[PH-CAL] Probe calibration unavailable");
  }
  PhProbeCal::printStatus(NAN);
  if (TdsProbeCal::loadStore()) {
    Serial.println("[TDS-CAL] Loaded Grove TDS kValue from LittleFS");
  } else {
    Serial.println("[TDS-CAL] Uncalibrated — kValue=1.0 (run ENTERTDS with salt water / 707 ppm)");
  }
  TdsProbeCal::printStatus(NAN, NAN);
  StageController::loadPersisted();
  loadSolutionMonitor();
  loadLightMakeup();
  if (StageController::ctx().fill0Percent >= 0) {
    netPotTouchPercent = StageController::ctx().fill0Percent;
  }
  {
    char calAlert[96];
    PumpCal::fillCalAlert(calAlert, sizeof(calAlert));
    if (calAlert[0] != '\0') {
      Serial.print("[ALERT] Pumps need calibration (will publish to Pi): ");
      Serial.println(calAlert);
      Serial.println("[ALERT] Type CAL on Serial (cup on the outlet) before ENABLE_PUMPS=1.");
    }
  }
  printSerialHelp(false);
  printSystemModeBanner();
  return true;
}

void armControlHolds() {
  StageController::armPostBootHold(millis());
  // Legacy pump paths also wait one settle before acting.
  phControlAllowedAt = millis() + GrowStage::config(systemMode).phSettleMs;
  waterLevelControlAllowedAt = millis() + StageController::WATER_SETTLE_MS;
  nutrientControlAllowedAt = millis() + GrowStage::config(systemMode).ecSettleMs;
}

void initSensors() {
  // GPIO 32 is ADC1 after the 10 kΩ / 20 kΩ divider (SEN0169 0–5 V → ≤ ~3.33 V).
  // ADC_11db covers the full ESP32 ADC range for that pin voltage.
  pinMode(PH_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PH_PIN, ADC_11db);
  initTdsPower();
  Serial.print("[DEBUG] pH ADC GPIO ");
  Serial.print(PH_PIN);
  Serial.print(" @ ADC_11db; board_V = gpio_V × ");
  Serial.println(PH_SIGNAL_LEVEL_SHIFT_GAIN, 3);

  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println("[DEBUG] I2C started on SDA=" + String(I2C_SDA) + " SCL=" + String(I2C_SCL));
  scanI2CDevices();

  ds18b20.begin();
  Serial.println("[DEBUG] DS18B20 on GPIO " + String(DS18B20_PIN));

  dht.begin();
  // External 10k pull-up is connected on the DHT11 DATA line; use INPUT (external pull-up)
  pinMode(DHT11_PIN, INPUT);
  Serial.println("[DEBUG] DHT11 ambient sensor on GPIO " + String(DHT11_PIN) + " (external 10k pull-up)");

  Serial.println("[DEBUG] Starting CO2 sensor...");
  if (co2Init()) {
    Serial.println("[DEBUG] CO2 sensor ready (single-shot on each sensor cycle)");
  } else {
    Serial.println("[ERROR] CO2 sensor init failed");
  }
}

