/************************************************************
 * Water drain test — water level + water pump only
 *
 * Diagnostic only. The prototype water pump is fill-only
 * (ENABLE_TEMPORARY_REVERSE_DRAIN 0). Prefer water_fill_cal.ino
 * for the priming fill. Flash this only if you must reverse-drain.
 *
 * Behavior:
 *   - Reads Grove water level (I2C 0x77 / 0x78) only
 *   - Persists each reading to the shared LittleFS store/CSV used
 *     by embedded-smart-garden.ino (so main trusts the new level on reflash)
 *   - Runs water pump on GPIO 18 in DRAIN (reverse) bursts
 *   - Holds the pump stopped at/below TARGET_LEVEL_PERCENT, but keeps
 *     polling in real time and drains again if the level rises
 *   - Short rest between bursts to protect the pump
 *   - Verbose Serial logging of every decision and reading
 *
 * If the level rises or never falls, swap WATER_DRAIN_RUN_SPEED
 * between 0 and 180 (wrong reverse direction).
 *
 * WARNING: reverse drain returns water to the source reservoir —
 * that container can overflow. Watch it while this runs.
 ************************************************************/

#include <Wire.h>
#include <ESP32Servo.h>
#include <LittleFS.h>
#include <cstring>
#include "sensor_data_store.h"

#define PUMP1_PIN 18  // Water pump (same as main sketch)
#define I2C_SDA 21
#define I2C_SCL 22

#define WATER_LOW_ADDR  0x77
#define WATER_HIGH_ADDR 0x78
#define NO_TOUCH              0xFE
#define WATER_TOUCH_THRESHOLD 100

const int SERVO_STOP = 90;
// Opposite of forward refill (0). Swap to 0 only if drain still runs the wrong way
// after confirming setPeriodHertz(50) (required for reverse / write(180)).
const int WATER_DRAIN_RUN_SPEED = 180;

const int TARGET_LEVEL_PERCENT = 30;
const unsigned long DRAIN_BURST_MS = 3000;   // pump on
const unsigned long DRAIN_REST_MS = 4000;    // pump off / settle + re-read
const unsigned long HOLD_POLL_MS = 2000;     // keep reading while at/below target
const int WATER_LEVEL_SAMPLE_COUNT = 3;

Servo waterPump;
unsigned char low_data[8];
unsigned char high_data[12];

unsigned long burstCount = 0;
bool sensorStoreReady = false;
int lastPersistedLevelPercent = -1;
bool wasHoldingAtTarget = false;

// Always refresh the shared trust file; append CSV only when the level changes
// so idle polls do not flood LittleFS.
void persistWaterLevel(int levelPercent) {
  if (!sensorStoreReady || levelPercent < 0) {
    return;
  }
  SensorDataStore::recordWaterLevel(levelPercent);
  if (levelPercent == lastPersistedLevelPercent) {
    return;
  }
  lastPersistedLevelPercent = levelPercent;
  SensorDataStore::appendCsvRow(
      "drain_test", levelPercent, TARGET_LEVEL_PERCENT);
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========== WATER DRAIN TEST ==========");
  Serial.println("Sensors: water level only");
  Serial.println("Actuator: water pump GPIO 18 reverse drain");
  Serial.print("Target level: <= ");
  Serial.print(TARGET_LEVEL_PERCENT);
  Serial.println("%");
  Serial.print("Burst: ");
  Serial.print(DRAIN_BURST_MS);
  Serial.print(" ms on / ");
  Serial.print(DRAIN_REST_MS);
  Serial.println(" ms rest");
  Serial.print("Drain servo speed: ");
  Serial.println(WATER_DRAIN_RUN_SPEED);
  Serial.print("Hold poll while at target: ");
  Serial.print(HOLD_POLL_MS);
  Serial.println(" ms (keeps running; no reset needed)");
  Serial.println("Watch source reservoir for overflow.");
  Serial.println("======================================");

  sensorStoreReady = SensorDataStore::begin(true);
  if (sensorStoreReady) {
    SensorDataStore::WaterLevelTrust trust;
    SensorDataStore::loadWaterLevelTrust(trust);
    if (trust.lastTrustedPercent > 0) {
      Serial.print("[SETUP] Shared store last trusted level: ");
      Serial.print(trust.lastTrustedPercent);
      Serial.println("%");
    }
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.print("[SETUP] I2C on SDA=");
  Serial.print(I2C_SDA);
  Serial.print(" SCL=");
  Serial.println(I2C_SCL);
  scanI2CDevices();

  // DFRobot DFR0523 expects ~50 Hz PPM. At 500 Hz the period is only 2000 µs,
  // so write(180) (~2500 µs) cannot fit and reverse never runs; write(0) still works.
  waterPump.setPeriodHertz(50);
  waterPump.attach(PUMP1_PIN, 500, 2500);
  waterPump.write(SERVO_STOP);
  Serial.println("[SETUP] Water pump attached on GPIO 18 — STOP (90)");
  Serial.println("[SETUP] Starting drain loop...");
  Serial.println();
}

void loop() {
  int level = readWaterLevelAverage(true);
  persistWaterLevel(level);

  if (level < 0) {
    wasHoldingAtTarget = false;
    waterPump.write(SERVO_STOP);
    Serial.println("[WARN] Water level unavailable — pump stays stopped; retrying");
    delay(HOLD_POLL_MS);
    return;
  }

  if (level <= TARGET_LEVEL_PERCENT) {
    waterPump.write(SERVO_STOP);
    if (!wasHoldingAtTarget) {
      Serial.println();
      Serial.print("[HOLD] Level ");
      Serial.print(level);
      Serial.print("% is at/below target ");
      Serial.print(TARGET_LEVEL_PERCENT);
      Serial.println("% — pump STOPPED; still polling (will drain again if level rises)");
      Serial.println();
      wasHoldingAtTarget = true;
    }
    logStatus("HOLD", level, false);
    delay(HOLD_POLL_MS);
    return;
  }

  if (wasHoldingAtTarget) {
    Serial.print("[RESUME] Level rose to ");
    Serial.print(level);
    Serial.print("% (above ");
    Serial.print(TARGET_LEVEL_PERCENT);
    Serial.println("%); draining again");
    wasHoldingAtTarget = false;
  }

  logStatus("PRE-BURST", level, false);

  burstCount++;
  Serial.print("[DRAIN] Burst #");
  Serial.print(burstCount);
  Serial.print(" START — writing servo ");
  Serial.print(WATER_DRAIN_RUN_SPEED);
  Serial.print(" for ");
  Serial.print(DRAIN_BURST_MS);
  Serial.println(" ms");

  waterPump.write(WATER_DRAIN_RUN_SPEED);
  unsigned long burstStartedAt = millis();
  while (millis() - burstStartedAt < DRAIN_BURST_MS) {
    // Re-assert drain command in case PWM glitches.
    waterPump.write(WATER_DRAIN_RUN_SPEED);
    delay(200);
  }

  waterPump.write(SERVO_STOP);
  Serial.print("[DRAIN] Burst #");
  Serial.print(burstCount);
  Serial.println(" END — pump STOP; resting");

  delay(DRAIN_REST_MS);

  level = readWaterLevelAverage(true);
  persistWaterLevel(level);
  logStatus("POST-BURST", level, true);
}

void logStatus(const char* phase, int levelPercent, bool afterBurst) {
  Serial.print("[STATUS] phase=");
  Serial.print(phase);
  Serial.print(" millis=");
  Serial.print(millis());
  Serial.print(" level=");
  if (levelPercent < 0) {
    Serial.print("UNAVAILABLE");
  } else {
    Serial.print(levelPercent);
    Serial.print("%");
  }
  Serial.print(" target=<=");
  Serial.print(TARGET_LEVEL_PERCENT);
  Serial.print("% bursts=");
  Serial.print(burstCount);
  if (afterBurst && levelPercent >= 0) {
    Serial.print(" delta_to_target=");
    Serial.print(levelPercent - TARGET_LEVEL_PERCENT);
  }
  Serial.println();

  // Serial Plotter friendly line
  Serial.print("WaterLevelPct:");
  Serial.print(levelPercent < 0 ? 0 : levelPercent);
  Serial.print(" TargetPct:");
  Serial.print(TARGET_LEVEL_PERCENT);
  Serial.print(" DrainBurst:");
  Serial.print(afterBurst ? 0 : 1);
  Serial.print(" BurstCount:");
  Serial.println(burstCount);
}

void scanI2CDevices() {
  int deviceCount = 0;
  Serial.println("[SETUP] Scanning I2C...");
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      deviceCount++;
      Serial.print("[SETUP] I2C device at 0x");
      if (address < 16) {
        Serial.print("0");
      }
      Serial.println(address, HEX);
    }
  }
  if (deviceCount == 0) {
    Serial.println("[ERROR] No I2C devices — water level wiring/power/pull-ups?");
  } else {
    Serial.print("[SETUP] Found ");
    Serial.print(deviceCount);
    Serial.println(" I2C device(s)");
  }
}

int readWaterLevelPercent(bool logPads) {
  memset(low_data, 0, sizeof(low_data));
  memset(high_data, 0, sizeof(high_data));

  Wire.requestFrom(WATER_LOW_ADDR, 8);
  int i = 0;
  while (Wire.available() && i < 8) {
    low_data[i++] = Wire.read();
  }

  Wire.requestFrom(WATER_HIGH_ADDR, 12);
  i = 0;
  while (Wire.available() && i < 12) {
    high_data[i++] = Wire.read();
  }

  int touched = 0;
  for (int j = 0; j < 8; j++) {
    if (low_data[j] > WATER_TOUCH_THRESHOLD && low_data[j] < NO_TOUCH) {
      touched++;
    }
  }
  for (int j = 0; j < 12; j++) {
    if (high_data[j] > WATER_TOUCH_THRESHOLD && high_data[j] < NO_TOUCH) {
      touched++;
    }
  }

  int percent = map(touched, 0, 20, 0, 100);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  if (logPads) {
    Serial.print("[LEVEL] touched=");
    Serial.print(touched);
    Serial.print("/20 raw=");
    Serial.print(percent);
    Serial.print("% low=[");
    for (int j = 0; j < 8; j++) {
      if (j) Serial.print(",");
      Serial.print(low_data[j]);
    }
    Serial.print("] high=[");
    for (int j = 0; j < 12; j++) {
      if (j) Serial.print(",");
      Serial.print(high_data[j]);
    }
    Serial.println("]");
  }

  return percent;
}

int readWaterLevelAverage(bool logPads) {
  int total = 0;
  int validCount = 0;

  for (int sample = 0; sample < WATER_LEVEL_SAMPLE_COUNT; sample++) {
    int level = readWaterLevelPercent(logPads && sample == 0);
    if (level >= 0 && level <= 100) {
      total += level;
      validCount++;
    }
    delay(100);
  }

  if (validCount == 0) {
    Serial.println("[WARN] No valid water level samples");
    return -1;
  }

  int average = total / validCount;
  Serial.print("[LEVEL] average of ");
  Serial.print(validCount);
  Serial.print(" samples = ");
  Serial.print(average);
  Serial.println("%");
  return average;
}
