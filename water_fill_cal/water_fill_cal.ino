/************************************************************
 * Water fill + Grove volume correlation
 *
 * Flash this instead of embedded-smart-garden.ino for the priming fill:
 * mix nutrients, set pH, then soak rockwool in that solution
 * by hand. Seeding uses the pre-soaked cubes. No drain.
 *
 * Tank (approx inner mm): 280 W × 127 H × 432 L  ≈ 15.36 L full.
 * Fill until water just touches the net pots, then TOUCH (or auto
 * on DONE) to persist /net_pot_touch.txt for embedded-smart-garden.ino.
 * Grove rule of thumb: 5 mm ≈ 5 %.
 *
 * Each interval:
 *   1. Read Grove water level
 *   2. Run the calibrated GPIO 18 peristaltic forward
 *   3. Settle and read again
 *   4. Log Grove % vs pumped mL vs geometric mL
 *
 * Serial 115200, line ending Newline.
 *   HELP / START / STOP / FILL / EMPTY / TARGET 70 / TOUCH / BURST 4000
 *
 * Needs a prior Serial CAL WATER in embedded-smart-garden.ino so mL/s exists.
 * The 1 L jug will empty several times on a 12 L fill — type FILL
 * after you refill the jug, then START again.
 ************************************************************/

#include <Wire.h>
#include <ESP32Servo.h>
#include <LittleFS.h>
#include <cstring>
#include "sensor_data_store.h"
#include "pump_calibration.h"
#include "tank_geometry.h"

#define PUMP1_PIN 18
#define I2C_SDA 21
#define I2C_SCL 22

#define WATER_LOW_ADDR  0x77
#define WATER_HIGH_ADDR 0x78
#define NO_TOUCH              0xFE
#define WATER_TOUCH_THRESHOLD 100

const int SERVO_STOP = 90;
const int WATER_FILL_RUN_SPEED = 0;  // same forward as embedded-smart-garden.ino

// Default stop guess; refine visually, then TOUCH to save the real reading.
int targetLevelPercent = Tank::NET_POT_TOUCH_DEFAULT_PERCENT;
unsigned long fillBurstMs = 4000;
const unsigned long FILL_SETTLE_MS = 5000;
const unsigned long HOLD_POLL_MS = 2000;
const int WATER_LEVEL_SAMPLE_COUNT = 3;
const int MAX_POINTS = 80;
const char* LOG_PATH = "/water_fill_cal.csv";

Servo waterPump;
unsigned char low_data[8];
unsigned char high_data[12];

bool running = false;
bool holding = false;
unsigned long burstCount = 0;
float pumpedMl = 0.0f;
float sessionRateMlPerSec = 0.0f;
int lastLevelPercent = -1;
String serialLineAcc;

float pointPct[MAX_POINTS];
float pointPumpMl[MAX_POINTS];
int pointCount = 0;

void printHelp() {
  Serial.println();
  Serial.println("========== WATER FILL CAL ==========");
  Serial.println("HELP              this text");
  Serial.println("START / YES       fill toward TARGET (forward only)");
  Serial.println("STOP              pause the pump");
  Serial.println("FILL              jug refilled — reset 1 L stock, then START");
  Serial.println("EMPTY             tank emptied — zero the pumped-mL counter");
  Serial.println("TARGET 70         soft stop guess (watch the net pots)");
  Serial.println("TOUCH             save current Grove % as net-pot touch");
  Serial.println("TOUCH 72          save an explicit Grove % as net-pot touch");
  Serial.println("BURST 4000        fill pulse length in ms");
  Serial.println("RATE 1.25         override mL/s for this session (does not save)");
  Serial.println("SHOW              print points + fit + saved touch");
  Serial.println("====================================");
}

void saveTouchPercent(int pct) {
  if (!SensorDataStore::saveNetPotTouchPercent(pct)) {
    return;
  }
  SensorDataStore::recordWaterLevel(pct);
  Serial.print("[TOUCH] Net-pot touch locked at ");
  Serial.print(pct);
  Serial.println("% (5 mm ≈ 5 %). Reflash embedded-smart-garden.ino when priming is done.");
}

void printTankBanner() {
  Serial.println();
  Serial.println("========== WATER FILL + VOLUME MAP ==========");
  Serial.println("Forward fill only. Stop when water just touches the net pots.");
  Serial.println("Then TOUCH to persist /net_pot_touch.txt for DWC air-gap math.");
  Serial.print("Tank ");
  Serial.print(Tank::WIDTH_MM, 0);
  Serial.print(" x ");
  Serial.print(Tank::HEIGHT_MM, 0);
  Serial.print(" x ");
  Serial.print(Tank::LENGTH_MM, 0);
  Serial.print(" mm  →  ");
  Serial.print(Tank::mlPerMm(), 1);
  Serial.print(" mL/mm  /  full ");
  Serial.print(Tank::fullMl() / 1000.0f, 2);
  Serial.println(" L");
  Serial.print("Soft TARGET Grove %: ");
  Serial.print(targetLevelPercent);
  Serial.print("  (geom ≈ ");
  Serial.print(Tank::geomMlFromGrovePct(static_cast<float>(targetLevelPercent)), 0);
  Serial.println(" mL)");
  Serial.print("Burst ");
  Serial.print(fillBurstMs);
  Serial.print(" ms + settle ");
  Serial.print(FILL_SETTLE_MS);
  Serial.println(" ms");
}

void persistMapHeader() {
  if (LittleFS.exists(LOG_PATH)) {
    return;
  }
  File file = LittleFS.open(LOG_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("[LOG] Could not create /water_fill_cal.csv");
    return;
  }
  file.println("burst,grove_pct,touched,pump_ml,geom_ml,height_mm,on_ms,ml_per_s");
  file.close();
}

void appendMapRow(int burst, int grovePct, int touched, float pumpMl,
                  float geomMl, float heightMm, unsigned long onMs, float rate) {
  persistMapHeader();
  File file = LittleFS.open(LOG_PATH, FILE_APPEND);
  if (!file) {
    return;
  }
  file.print(burst);
  file.print(',');
  file.print(grovePct);
  file.print(',');
  file.print(touched);
  file.print(',');
  file.print(pumpMl, 1);
  file.print(',');
  file.print(geomMl, 1);
  file.print(',');
  file.print(heightMm, 1);
  file.print(',');
  file.print(onMs);
  file.print(',');
  file.println(rate, 3);
  file.close();
}

void printFit() {
  if (pointCount < 2) {
    Serial.println("[FIT] Need at least 2 points (EMPTY then START from a known level).");
    return;
  }
  float sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
  for (int i = 0; i < pointCount; i++) {
    sumX += pointPct[i];
    sumY += pointPumpMl[i];
    sumXX += pointPct[i] * pointPct[i];
    sumXY += pointPct[i] * pointPumpMl[i];
  }
  const float n = static_cast<float>(pointCount);
  const float denom = n * sumXX - sumX * sumX;
  if (fabsf(denom) < 0.001f) {
    Serial.println("[FIT] Grove % did not change — longer BURST or keep filling.");
    return;
  }
  const float slope = (n * sumXY - sumX * sumY) / denom;
  const float intercept = (sumY - slope * sumX) / n;
  Serial.print("[FIT] pump_mL ≈ ");
  Serial.print(slope, 2);
  Serial.print(" * grove_pct + ");
  Serial.print(intercept, 1);
  Serial.print("   (geom ");
  Serial.print(Tank::fullMl() / 100.0f, 1);
  Serial.println(" mL per Grove % if the stick spans the full height)");
}

void recordPoint(int grovePct) {
  if (pointCount >= MAX_POINTS) {
    return;
  }
  pointPct[pointCount] = static_cast<float>(grovePct);
  pointPumpMl[pointCount] = pumpedMl;
  pointCount++;
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
  }
}

int lastTouchedPads = 0;

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
  lastTouchedPads = touched;

  int percent = map(touched, 0, 20, 0, 100);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  if (logPads) {
    Serial.print("[LEVEL] touched=");
    Serial.print(touched);
    Serial.print("/20 raw=");
    Serial.print(percent);
    Serial.println("%");
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
    delay(80);
  }
  if (validCount == 0) {
    return -1;
  }
  return total / validCount;
}

void logRow(const char* phase, int level, unsigned long onMs) {
  const float geom = level < 0 ? NAN : Tank::geomMlFromGrovePct(static_cast<float>(level));
  const float height = level < 0 ? NAN : Tank::heightMmFromGrovePct(static_cast<float>(level));
  Serial.print("[MAP] phase=");
  Serial.print(phase);
  Serial.print(" grove=");
  if (level < 0) {
    Serial.print("NA");
  } else {
    Serial.print(level);
    Serial.print("%");
  }
  Serial.print(" pads=");
  Serial.print(lastTouchedPads);
  Serial.print(" pump_ml=");
  Serial.print(pumpedMl, 1);
  Serial.print(" geom_ml=");
  if (level < 0) {
    Serial.print("NA");
  } else {
    Serial.print(geom, 1);
    Serial.print(" h_mm=");
    Serial.print(height, 1);
  }
  Serial.print(" rate=");
  Serial.print(sessionRateMlPerSec, 3);
  Serial.println(" mL/s");

  Serial.print("GrovePct:");
  Serial.print(level < 0 ? 0 : level);
  Serial.print(" PumpMl:");
  Serial.print(pumpedMl, 1);
  Serial.print(" GeomMl:");
  Serial.println(level < 0 ? 0 : geom, 1);

  if (level >= 0 && strcmp(phase, "POST") == 0) {
    appendMapRow(static_cast<int>(burstCount), level, lastTouchedPads, pumpedMl,
                 geom, height, onMs, sessionRateMlPerSec);
    recordPoint(level);
    printFit();
  }
}

void stopFill(const char* reason) {
  waterPump.write(SERVO_STOP);
  running = false;
  Serial.print("[FILL] Stopped (");
  Serial.print(reason);
  Serial.println("). Type START to continue.");
}

void serviceSerial();

bool waitMsOrStop(unsigned long ms) {
  const unsigned long started = millis();
  while (millis() - started < ms) {
    serviceSerial();
    if (!running) {
      waterPump.write(SERVO_STOP);
      return false;
    }
    delay(20);
  }
  return true;
}

void startFill() {
  if (sessionRateMlPerSec < 0.01f) {
    Serial.println("[WARN] Water pump not calibrated. Type RATE <mL/s> or flash");
    Serial.println("       embedded-smart-garden.ino, run CAL WATER, then return here.");
  }
  if (PumpCal::isEmpty(PumpCal::WATER)) {
    Serial.println("[STOCK] Jug empty. Refill the 1 L bottle, type FILL, then START.");
    return;
  }
  running = true;
  holding = false;
  Serial.println("[FILL] Starting forward fill. Type STOP to pause.");
}

void handleLine(String line) {
  line.trim();
  if (line.length() == 0) {
    return;
  }
  String upper = line;
  upper.toUpperCase();

  if (upper == "HELP" || upper == "?") {
    printHelp();
    return;
  }
  if (upper == "STOP") {
    stopFill("operator");
    return;
  }
  if (upper == "START" || upper == "YES" || upper == "Y") {
    startFill();
    return;
  }
  if (upper == "FILL") {
    PumpCal::refill(PumpCal::WATER);
    Serial.println("[STOCK] Water jug marked full (1 L). Type START.");
    PumpCal::printStock();
    return;
  }
  if (upper == "EMPTY") {
    pumpedMl = 0.0f;
    pointCount = 0;
    burstCount = 0;
    Serial.println("[FILL] Pumped-mL counter and fit points cleared.");
    return;
  }
  if (upper == "SHOW") {
    Serial.print("[SHOW] points=");
    Serial.print(pointCount);
    Serial.print(" pumped_ml=");
    Serial.println(pumpedMl, 1);
    for (int i = 0; i < pointCount; i++) {
      Serial.print("  ");
      Serial.print(pointPct[i], 0);
      Serial.print("% → ");
      Serial.print(pointPumpMl[i], 1);
      Serial.println(" mL pumped");
    }
    printFit();
    int touch = Tank::NET_POT_TOUCH_DEFAULT_PERCENT;
    if (SensorDataStore::loadNetPotTouchPercent(touch)) {
      Serial.print("[SHOW] Saved net-pot touch ");
      Serial.print(touch);
      Serial.println("%");
    }
    PumpCal::printStock();
    return;
  }
  if (upper == "TOUCH") {
    int level = lastLevelPercent;
    if (level < 0) {
      level = readWaterLevelAverage(true);
      lastLevelPercent = level;
    }
    if (level < 0) {
      Serial.println("[TOUCH] No usable Grove reading — wait for a stable level.");
      return;
    }
    saveTouchPercent(level);
    return;
  }
  if (upper.startsWith("TOUCH ")) {
    int v = line.substring(6).toInt();
    saveTouchPercent(v);
    return;
  }
  if (upper.startsWith("TARGET ")) {
    int v = line.substring(7).toInt();
    if (v < 5 || v > 95) {
      Serial.println("[FILL] TARGET must be 5–95.");
      return;
    }
    targetLevelPercent = v;
    Serial.print("[FILL] Soft TARGET Grove % = ");
    Serial.println(targetLevelPercent);
    return;
  }
  if (upper.startsWith("BURST ")) {
    long v = line.substring(6).toInt();
    if (v < 500 || v > 20000) {
      Serial.println("[FILL] BURST must be 500–20000 ms.");
      return;
    }
    fillBurstMs = static_cast<unsigned long>(v);
    Serial.print("[FILL] Burst ms = ");
    Serial.println(fillBurstMs);
    return;
  }
  if (upper.startsWith("RATE ")) {
    sessionRateMlPerSec = line.substring(5).toFloat();
    Serial.print("[FILL] Session rate = ");
    Serial.print(sessionRateMlPerSec, 3);
    Serial.println(" mL/s (not saved to LittleFS)");
    return;
  }
  Serial.print("[FILL] Unknown: ");
  Serial.println(line);
  Serial.println("[FILL] Type HELP");
}

void serviceSerial() {
  while (Serial.available() > 0) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      handleLine(serialLineAcc);
      serialLineAcc = "";
    } else if (serialLineAcc.length() < 80) {
      serialLineAcc += c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  printTankBanner();
  printHelp();

  if (!LittleFS.begin(true)) {
    Serial.println("[ERROR] LittleFS mount failed — no cal file / no CSV log");
  } else {
    PumpCal::load();
    PumpCal::printRates();
    PumpCal::printStock();
    persistMapHeader();
  }

  if (PumpCal::isCalibrated(PumpCal::WATER)) {
    sessionRateMlPerSec = PumpCal::mlPerSec(PumpCal::WATER);
    Serial.print("[SETUP] Using stored water rate ");
    Serial.print(sessionRateMlPerSec, 3);
    Serial.println(" mL/s");
  } else {
    Serial.println("[SETUP] No water mL/s yet — fill will still run; type RATE or CAL first.");
  }

  Wire.begin(I2C_SDA, I2C_SCL);
  scanI2CDevices();

  waterPump.setPeriodHertz(50);
  waterPump.attach(PUMP1_PIN, 500, 2500);
  waterPump.write(SERVO_STOP);
  Serial.println("[SETUP] GPIO 18 fill only (servo 0). Drain is disabled in this sketch.");
  Serial.println("[SETUP] Type START when the inlet is in the jug and the outlet is in the tank.");
}

void loop() {
  serviceSerial();

  if (!running) {
    delay(50);
    return;
  }

  int level = readWaterLevelAverage(true);
  lastLevelPercent = level;
  if (level < 0) {
    stopFill("level unavailable");
    delay(HOLD_POLL_MS);
    return;
  }

  if (level >= targetLevelPercent) {
    waterPump.write(SERVO_STOP);
    if (!holding) {
      Serial.print("[HOLD] Grove ");
      Serial.print(level);
      Serial.print("% reached soft TARGET ");
      Serial.print(targetLevelPercent);
      Serial.println("% — confirm water just touches the net pots, then TOUCH.");
      logRow("DONE", level, 0);
      saveTouchPercent(level);
      holding = true;
      running = false;
    }
    delay(HOLD_POLL_MS);
    return;
  }

  const float plannedMl =
      sessionRateMlPerSec * (static_cast<float>(fillBurstMs) / 1000.0f);
  if (sessionRateMlPerSec > 0.01f &&
      !PumpCal::canDispense(PumpCal::WATER, plannedMl)) {
    stopFill("jug low/empty — type FILL after you refill");
    PumpCal::printStock();
    return;
  }

  burstCount++;
  Serial.print("[FILL] Burst #");
  Serial.print(burstCount);
  Serial.print(" forward ");
  Serial.print(fillBurstMs);
  Serial.println(" ms");
  logRow("PRE", level, 0);

  waterPump.write(WATER_FILL_RUN_SPEED);
  if (!waitMsOrStop(fillBurstMs)) {
    return;
  }
  waterPump.write(SERVO_STOP);

  const float added = sessionRateMlPerSec * (static_cast<float>(fillBurstMs) / 1000.0f);
  pumpedMl += added;
  if (added > 0.0f) {
    PumpCal::consumeMl(PumpCal::WATER, added);
  }

  Serial.println("[FILL] Settling…");
  if (!waitMsOrStop(FILL_SETTLE_MS)) {
    return;
  }

  level = readWaterLevelAverage(true);
  lastLevelPercent = level;
  logRow("POST", level, fillBurstMs);
}
