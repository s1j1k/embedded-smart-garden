/************************************************************
 * Persistent peristaltic pump calibration + bottle stock
 *
 * Chemistry pumps (nutrient, pH-up, pH-down) are calibrated with
 * 500 ms bursts. Stored unit: mL / burst. A dose runs for
 *   ms = requested_mL / (mL/burst) * 500
 * so the on-time matches the requested volume. The 500 ms figure
 * is the calibration interval, not a minimum dose.
 *
 * Water refill is a 1 L jug (initial tank fill: water_fill_cal.ino).
 * Water is calibrated as a continuous run. Stored unit: mL / s.
 * In DWC the jug pump only runs for safety top-up or high-EC dilution.
 *
 * Bottle capacities and remaining mL live in /pump_stock.txt.
 * Remaining is estimated from calibration whenever a pump runs
 * (auto dose, prime, or the cup wizard).
 *
 * Dashboard / MQTT wizard (smart-garden/<id>/cmd):
 *   {"cmd":"pump_cal","action":"enter","pump":"water|nutrient|ph_up|ph_down"}
 *   then prime | run | stop | volume({ml}) | retry | skip | abort | status
 * Human must confirm the 10 mL cup is on the outlet before "run".
 * TODO(pi-fill): accept FILL / SETREM from the Pi after a bottle change.
 ************************************************************/

#ifndef SMART_GARDEN_PUMP_CALIBRATION_H
#define SMART_GARDEN_PUMP_CALIBRATION_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>
#include <string.h>

namespace PumpCal {

enum Id {
  WATER = 0,
  NUTRIENT,
  PH_UP,
  PH_DOWN,
  COUNT
};

constexpr const char* STORE_PATH = "/pump_cal.txt";
constexpr const char* STOCK_PATH = "/pump_stock.txt";

// First-iteration bottle sizes (mL). Change with Serial SETCAP.
constexpr float DEFAULT_CAPACITY_ML[COUNT] = {
  5000.0f,  // water top-up: 5 L jerry can (18 Oct)
  1000.0f,  // nutrient
  600.0f,   // pH-up
  600.0f    // pH-down
};

constexpr unsigned long BURST_MS = 500;
constexpr float LOW_FRACTION = 0.15f;
constexpr float WATER_EMPTY_RESERVE_ML = 25.0f;
constexpr float CHEM_EMPTY_RESERVE_ML = 5.0f;

inline const char* name(Id id) {
  switch (id) {
    case WATER: return "water";
    case NUTRIENT: return "nutrient";
    case PH_UP: return "ph_up";
    case PH_DOWN: return "ph_down";
    default: return "unknown";
  }
}

inline const char* gpioHint(Id id) {
  switch (id) {
    case WATER: return "GPIO18 forward only (safety top-up / EC dilute; no reverse)";
    case NUTRIENT: return "GPIO19 (inlet = 1 L nutrient bottle)";
    case PH_UP: return "GPIO23 (inlet = 600 mL pH-up)";
    case PH_DOWN: return "GPIO5 (inlet = 600 mL pH-down; PCB D5 - trace broken, offline)";
    default: return "";
  }
}

inline bool usesBursts(Id id) {
  return id == NUTRIENT || id == PH_UP || id == PH_DOWN;
}

inline Id idFromName(const char* label) {
  if (label == nullptr) {
    return COUNT;
  }
  String s(label);
  s.toLowerCase();
  if (s == "water" || s == "refill" || s == "pump1") {
    return WATER;
  }
  if (s == "nutrient" || s == "tds" || s == "pump2") {
    return NUTRIENT;
  }
  if (s == "ph_up" || s == "phup" || s == "up" || s == "pump3") {
    return PH_UP;
  }
  if (s == "ph_down" || s == "phdown" || s == "down" || s == "pump4") {
    return PH_DOWN;
  }
  return COUNT;
}

struct Store {
  float value[COUNT];
  bool valid[COUNT];
  bool burstUnit[COUNT];
  float capacityMl[COUNT];
  float remainingMl[COUNT];

  Store() {
    for (int i = 0; i < COUNT; i++) {
      value[i] = 0.0f;
      valid[i] = false;
      burstUnit[i] = usesBursts(static_cast<Id>(i));
      capacityMl[i] = DEFAULT_CAPACITY_ML[i];
      remainingMl[i] = DEFAULT_CAPACITY_ML[i];
    }
  }
};

inline Store& store() {
  static Store cached;
  return cached;
}

inline bool isCalibrated(Id id) {
  return id < COUNT && store().valid[id] && store().value[id] > 0.001f;
}

inline float mlPerSec(Id id) {
  if (!isCalibrated(id)) {
    return 0.0f;
  }
  if (store().burstUnit[id]) {
    return store().value[id] / (static_cast<float>(BURST_MS) / 1000.0f);
  }
  return store().value[id];
}

inline float mlPerBurst(Id id) {
  if (!isCalibrated(id)) {
    return 0.0f;
  }
  if (store().burstUnit[id]) {
    return store().value[id];
  }
  return store().value[id] * (static_cast<float>(BURST_MS) / 1000.0f);
}

inline float capacityMl(Id id) {
  return id < COUNT ? store().capacityMl[id] : 0.0f;
}

inline float remainingMl(Id id) {
  return id < COUNT ? store().remainingMl[id] : 0.0f;
}

inline float remainingPct(Id id) {
  const float cap = capacityMl(id);
  if (cap <= 0.0f) {
    return 0.0f;
  }
  return 100.0f * remainingMl(id) / cap;
}

inline float emptyReserveMl(Id id) {
  if (usesBursts(id)) {
    const float one = isCalibrated(id) ? mlPerBurst(id) : CHEM_EMPTY_RESERVE_ML;
    return one > CHEM_EMPTY_RESERVE_ML ? one : CHEM_EMPTY_RESERVE_ML;
  }
  return WATER_EMPTY_RESERVE_ML;
}

inline bool isEmpty(Id id) {
  return remainingMl(id) <= emptyReserveMl(id);
}

inline bool isLow(Id id) {
  return remainingMl(id) <= capacityMl(id) * LOW_FRACTION;
}

inline bool saveStock();
inline bool saveRates();

inline float volumeForRunMs(Id id, unsigned long runMs) {
  if (!isCalibrated(id) || runMs == 0) {
    return 0.0f;
  }
  if (store().burstUnit[id]) {
    return mlPerBurst(id) *
           (static_cast<float>(runMs) / static_cast<float>(BURST_MS));
  }
  return mlPerSec(id) * (static_cast<float>(runMs) / 1000.0f);
}

inline float volumeForBursts(Id id, int bursts) {
  if (!isCalibrated(id) || bursts <= 0) {
    return 0.0f;
  }
  return mlPerBurst(id) * static_cast<float>(bursts);
}

inline unsigned long runMsForVolume(Id id, float doseMl, unsigned long fallbackMs,
                                    unsigned long minMs, unsigned long maxMs) {
  if (!isCalibrated(id) || doseMl <= 0.0f) {
    return fallbackMs;
  }
  // Burst calibration is a rate: mL per BURST_MS. Scale that interval.
  const float ms = store().burstUnit[id]
                       ? (doseMl / store().value[id]) * static_cast<float>(BURST_MS)
                       : (doseMl / store().value[id]) * 1000.0f;
  if (ms < static_cast<float>(minMs)) {
    return minMs;
  }
  if (ms > static_cast<float>(maxMs)) {
    return maxMs;
  }
  return static_cast<unsigned long>(ms + 0.5f);
}

inline int burstsForVolume(Id id, float doseMl, int fallbackBursts, int maxBursts) {
  if (!usesBursts(id)) {
    return 0;
  }
  if (!isCalibrated(id) || doseMl <= 0.0f) {
    return fallbackBursts < 1 ? 1 : fallbackBursts;
  }
  int n = static_cast<int>(doseMl / mlPerBurst(id) + 0.5f);
  if (n < 1) {
    n = 1;
  }
  if (n > maxBursts) {
    n = maxBursts;
  }
  return n;
}

inline bool canDispense(Id id, float doseMl) {
  if (isEmpty(id)) {
    return false;
  }
  if (doseMl <= 0.0f) {
    return !isEmpty(id);
  }
  return remainingMl(id) - doseMl >= emptyReserveMl(id);
}

inline void consumeMl(Id id, float ml) {
  if (id >= COUNT || ml <= 0.0f) {
    return;
  }
  Store& s = store();
  s.remainingMl[id] -= ml;
  if (s.remainingMl[id] < 0.0f) {
    s.remainingMl[id] = 0.0f;
  }
  saveStock();
}

inline void consumeBursts(Id id, int bursts) {
  consumeMl(id, volumeForBursts(id, bursts));
}

inline void consumeRunMs(Id id, unsigned long runMs) {
  consumeMl(id, volumeForRunMs(id, runMs));
}

inline void setRemaining(Id id, float ml) {
  if (id >= COUNT) {
    return;
  }
  if (ml < 0.0f) {
    ml = 0.0f;
  }
  if (ml > store().capacityMl[id]) {
    ml = store().capacityMl[id];
  }
  store().remainingMl[id] = ml;
  saveStock();
}

inline void setCapacity(Id id, float ml) {
  if (id >= COUNT || ml < 10.0f || ml > 20000.0f) {
    return;
  }
  Store& s = store();
  const bool wasFull =
      fabsf(s.remainingMl[id] - s.capacityMl[id]) < 0.5f;
  s.capacityMl[id] = ml;
  if (wasFull || s.remainingMl[id] > ml) {
    s.remainingMl[id] = ml;
  }
  saveStock();
}

inline void refill(Id id) {
  if (id >= COUNT) {
    return;
  }
  store().remainingMl[id] = store().capacityMl[id];
  saveStock();
}

inline void refillAll() {
  for (int i = 0; i < COUNT; i++) {
    store().remainingMl[i] = store().capacityMl[i];
  }
  saveStock();
}

inline bool saveRates() {
  File file = LittleFS.open(STORE_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("[CAL] Failed to write pump calibration file");
    return false;
  }
  for (int i = 0; i < COUNT; i++) {
    file.print(name(static_cast<Id>(i)));
    file.print("=");
    if (store().valid[i]) {
      file.print(store().burstUnit[i] ? "b:" : "s:");
      file.println(store().value[i], 4);
    } else {
      file.println();
    }
  }
  file.close();
  Serial.print("[CAL] Saved rates to ");
  Serial.println(STORE_PATH);
  return true;
}

inline bool saveStock() {
  File file = LittleFS.open(STOCK_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("[STOCK] Failed to write bottle file");
    return false;
  }
  for (int i = 0; i < COUNT; i++) {
    file.print(name(static_cast<Id>(i)));
    file.print("=");
    file.print(store().capacityMl[i], 1);
    file.print(",");
    file.println(store().remainingMl[i], 2);
  }
  file.close();
  return true;
}

inline bool save() {
  return saveRates();
}

inline void loadStock() {
  Store& s = store();
  for (int i = 0; i < COUNT; i++) {
    s.capacityMl[i] = DEFAULT_CAPACITY_ML[i];
    s.remainingMl[i] = DEFAULT_CAPACITY_ML[i];
  }

  if (!LittleFS.exists(STOCK_PATH)) {
    saveStock();
    Serial.println("[STOCK] No bottle file — assuming full defaults "
                   "(water 1 L, nutrient 1 L, pH 600 mL). Type FILL after a change.");
    return;
  }

  File file = LittleFS.open(STOCK_PATH, FILE_READ);
  if (!file) {
    Serial.println("[STOCK] Failed to read bottle file");
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }
    Id id = idFromName(line.substring(0, eq).c_str());
    if (id == COUNT) {
      continue;
    }
    String rest = line.substring(eq + 1);
    rest.trim();
    int comma = rest.indexOf(',');
    float cap = DEFAULT_CAPACITY_ML[id];
    float rem = cap;
    if (comma < 0) {
      rem = rest.toFloat();
    } else {
      cap = rest.substring(0, comma).toFloat();
      rem = rest.substring(comma + 1).toFloat();
    }
    if (cap >= 10.0f && cap <= 20000.0f) {
      s.capacityMl[id] = cap;
    }
    if (rem < 0.0f) {
      rem = 0.0f;
    }
    if (rem > s.capacityMl[id]) {
      rem = s.capacityMl[id];
    }
    s.remainingMl[id] = rem;
  }
  file.close();
}

inline void loadRates() {
  Store& s = store();
  for (int i = 0; i < COUNT; i++) {
    s.value[i] = 0.0f;
    s.valid[i] = false;
    s.burstUnit[i] = usesBursts(static_cast<Id>(i));
  }

  if (!LittleFS.exists(STORE_PATH)) {
    Serial.println("[CAL] No pump calibration file yet — type HELP then CAL");
    return;
  }

  File file = LittleFS.open(STORE_PATH, FILE_READ);
  if (!file) {
    Serial.println("[CAL] Failed to read pump calibration file");
    return;
  }

  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq <= 0) {
      continue;
    }
    String key = line.substring(0, eq);
    String val = line.substring(eq + 1);
    key.trim();
    val.trim();
    Id id = idFromName(key.c_str());
    if (id == COUNT || val.length() == 0) {
      continue;
    }

    bool burst = usesBursts(id);
    if (val.startsWith("b:") || val.startsWith("B:")) {
      burst = true;
      val = val.substring(2);
    } else if (val.startsWith("s:") || val.startsWith("S:")) {
      burst = false;
      val = val.substring(2);
    }

    float parsed = val.toFloat();
    if (parsed <= 0.001f || parsed >= 50.0f) {
      continue;
    }

    // Legacy bare number was always mL/s. Convert chemistry pumps to mL/burst.
    if (usesBursts(id) && !burst) {
      parsed = parsed * (static_cast<float>(BURST_MS) / 1000.0f);
      burst = true;
    }
    if (!usesBursts(id)) {
      burst = false;
    }

    s.value[id] = parsed;
    s.valid[id] = true;
    s.burstUnit[id] = burst;
  }
  file.close();
}

inline void load() {
  loadRates();
  loadStock();
}

inline void clear() {
  Store& s = store();
  for (int i = 0; i < COUNT; i++) {
    s.value[i] = 0.0f;
    s.valid[i] = false;
    s.burstUnit[i] = usesBursts(static_cast<Id>(i));
  }
  if (LittleFS.exists(STORE_PATH)) {
    LittleFS.remove(STORE_PATH);
  }
  Serial.println("[CAL] Cleared stored pump rates (bottle levels kept)");
}

inline void setContinuousRate(Id id, float rate) {
  if (id >= COUNT) {
    return;
  }
  store().value[id] = rate;
  store().valid[id] = rate > 0.001f;
  store().burstUnit[id] = false;
  saveRates();
}

inline void setBurstVolume(Id id, float mlBurst) {
  if (id >= COUNT) {
    return;
  }
  store().value[id] = mlBurst;
  store().valid[id] = mlBurst > 0.001f;
  store().burstUnit[id] = true;
  saveRates();
}

inline void setRate(Id id, float rate) {
  if (usesBursts(id)) {
    setBurstVolume(id, rate);
  } else {
    setContinuousRate(id, rate);
  }
}

inline void printRates() {
  Serial.println("[CAL] Stored pump calibration:");
  for (int i = 0; i < COUNT; i++) {
    Id id = static_cast<Id>(i);
    Serial.print("  ");
    Serial.print(name(id));
    Serial.print(": ");
    if (store().valid[i]) {
      if (store().burstUnit[i]) {
        Serial.print(store().value[i], 3);
        Serial.println(" mL / 500 ms burst");
      } else {
        Serial.print(store().value[i], 3);
        Serial.println(" mL/s (continuous)");
      }
    } else {
      Serial.println("not calibrated");
    }
  }
}

inline void printStock() {
  Serial.println("[STOCK] Dosing bottles (estimate from calibration):");
  for (int i = 0; i < COUNT; i++) {
    Id id = static_cast<Id>(i);
    Serial.print("  ");
    Serial.print(name(id));
    Serial.print(": ");
    Serial.print(remainingMl(id), 1);
    Serial.print(" / ");
    Serial.print(capacityMl(id), 0);
    Serial.print(" mL (");
    Serial.print(remainingPct(id), 0);
    Serial.print("%)");
    if (isEmpty(id)) {
      Serial.print(" EMPTY — dosing stopped");
    } else if (isLow(id)) {
      Serial.print(" LOW");
    }
    Serial.println();
  }
  Serial.println("[STOCK] Type FILL after you replace a bottle. SETCAP / SETREM to adjust.");
}

inline int calibratedCount() {
  int n = 0;
  for (int i = 0; i < COUNT; i++) {
    if (store().valid[i]) {
      n++;
    }
  }
  return n;
}

inline void fillStockAlert(char* buf, size_t buflen) {
  if (buf == nullptr || buflen == 0) {
    return;
  }
  buf[0] = '\0';
  bool first = true;
  for (int i = 0; i < COUNT; i++) {
    Id id = static_cast<Id>(i);
    const char* tag = nullptr;
    if (isEmpty(id)) {
      tag = "empty:";
    } else if (isLow(id)) {
      tag = "low:";
    }
    if (tag == nullptr) {
      continue;
    }
    if (!first) {
      strncat(buf, ",", buflen - strlen(buf) - 1);
    }
    first = false;
    strncat(buf, tag, buflen - strlen(buf) - 1);
    strncat(buf, name(id), buflen - strlen(buf) - 1);
  }
}

// MQTT / Pi banner: "uncal:water,uncal:ph_down" or "" when every pump has a rate.
inline void fillCalAlert(char* buf, size_t buflen) {
  if (buf == nullptr || buflen == 0) {
    return;
  }
  buf[0] = '\0';
  bool first = true;
  for (int i = 0; i < COUNT; i++) {
    Id id = static_cast<Id>(i);
    if (isCalibrated(id)) {
      continue;
    }
    if (!first) {
      strncat(buf, ",", buflen - strlen(buf) - 1);
    }
    first = false;
    strncat(buf, "uncal:", buflen - strlen(buf) - 1);
    strncat(buf, name(id), buflen - strlen(buf) - 1);
  }
}

// True when every pump that auto-control may use still needs a cup CAL.
// Nutrient can be skipped when dosing is disabled for the active stage.
inline bool needsCalibrationAlert(bool nutrientDosingEnabled) {
  if (!isCalibrated(WATER) || !isCalibrated(PH_UP) || !isCalibrated(PH_DOWN)) {
    return true;
  }
  if (nutrientDosingEnabled && !isCalibrated(NUTRIENT)) {
    return true;
  }
  return false;
}

}  // namespace PumpCal

#endif  // SMART_GARDEN_PUMP_CALIBRATION_H
