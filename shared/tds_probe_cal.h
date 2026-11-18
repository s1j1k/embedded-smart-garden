/************************************************************
 * Grove TDS Sensor v1.0 — single-point probe calibration
 *
 * Conversion (Seeed Grove TDS / DFRobot Gravity SEN0244 cubic):
 *   T_C = DS18B20 water temperature (°C); 25 °C if the probe is invalid
 *   compensationCoefficient = 1.0 + 0.02 * (T_C − 25)
 *   V_comp = V_gpio / compensationCoefficient
 *   tds_raw = (133.42 V³ − 255.86 V² + 857.39 V) * 0.5
 *   tds_ppm = tds_raw * kValue
 *
 * Temperature compensation uses the tank water DS18B20 (not ambient DHT11
 * and not the water-level sensor, which has no temperature output).
 *
 * Calibration stores a multiplicative kValue so a known reference
 * solution (standard 707 ppm @ 25 °C, or salt water measured with a
 * TDS pen) matches the reported ppm. Default kValue = 1.0 (uncalibrated).
 *
 * Sources:
 *   - Seeed Studio Wiki — Grove - TDS Sensor
 *     https://wiki.seeedstudio.com/Grove-TDS-Sensor/
 *   - DFRobot Wiki — Gravity Analog TDS SEN0244 calibration
 *     https://wiki.dfrobot.com/sen0244/docs/20304
 *     (707 ppm / 1413 µS·cm⁻¹ @ 25 °C; enter / cal:ppm / exit)
 *   - DFRobot GravityTDS library (kValue + temperature compensation)
 *
 * Serial: ENTERTDS / CALTDS <ppm> / EXITTDS / TDSABORT / TDSSHOW / TDSCLEAR
 * MQTT:   {"cmd":"tds_cal","action":"enter|capture|save|abort|clear|status",
 *          "ppm":707}
 ************************************************************/

#ifndef SMART_GARDEN_TDS_PROBE_CAL_H
#define SMART_GARDEN_TDS_PROBE_CAL_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>

namespace TdsProbeCal {

constexpr const char* STORE_PATH = "/tds_probe_cal.txt";

// Uncalibrated factory factor from Grove / Gravity sample code.
constexpr float DEFAULT_K_VALUE = 1.0f;

// DFRobot recommended reference when a standard buffer is available.
constexpr float DEFAULT_REF_PPM = 707.0f;

// Same sentinel as DS18B20 / SensorDataStore when the water probe fails.
constexpr float INVALID_WATER_TEMP_C = -127.0f;

enum State {
  IDLE = 0,
  ACTIVE,
};

struct Store {
  float kValue = DEFAULT_K_VALUE;
  float refPpm = NAN;
  float refVoltage = NAN;
  float refTempC = NAN;
  bool calibrated = false;
};

struct Session {
  State state = IDLE;
  float pendingK = NAN;
  float pendingRefPpm = NAN;
  float pendingVoltage = NAN;
  float pendingTempC = NAN;
  float pendingRawPpm = NAN;
  const char* lastMessage = "";
  bool lastOk = true;
};

inline Store& store() {
  static Store s;
  return s;
}

inline Session& session() {
  static Session s;
  return s;
}

inline float kValue() {
  return store().kValue;
}

inline bool isCalibrated() {
  return store().calibrated;
}

inline bool isActive() {
  return session().state == ACTIVE;
}

inline const char* stateName() {
  return isActive() ? "active" : "idle";
}

/** Water temp used for TDS: DS18B20 °C, or 25 °C if the probe is invalid. */
inline float effectiveTempC(float waterTempC) {
  if (!isfinite(waterTempC) || waterTempC <= INVALID_WATER_TEMP_C ||
      waterTempC < -20.0f || waterTempC > 70.0f) {
    return 25.0f;
  }
  return waterTempC;
}

/** Temperature compensation coefficient (DFRobot / GravityTDS). */
inline float compensationCoefficient(float tempC) {
  return 1.0f + 0.02f * (effectiveTempC(tempC) - 25.0f);
}

/** Grove / DFRobot cubic on temperature-compensated volts → ppm before k. */
inline float voltageToRawPpm(float voltage, float tempC) {
  const float t = effectiveTempC(tempC);
  const float coeff = compensationCoefficient(t);
  if (coeff < 0.1f) {
    return 0.0f;
  }
  const float v = voltage / coeff;
  float tds = (133.42f * v * v * v - 255.86f * v * v + 857.39f * v) * 0.5f;
  if (tds < 0.0f) {
    tds = 0.0f;
  }
  return tds;
}

inline float voltageToPpm(float voltage, float tempC) {
  return voltageToRawPpm(voltage, tempC) * store().kValue;
}

inline int adcToPpm(int adc, float tempC, float aref = 3.3f, int adcMax = 4095) {
  if (adcMax <= 0) {
    return 0;
  }
  const float voltage = static_cast<float>(adc) * aref / static_cast<float>(adcMax);
  const float ppm = voltageToPpm(voltage, tempC);
  if (ppm < 0.0f) {
    return 0;
  }
  return static_cast<int>(ppm + 0.5f);
}

inline bool saveStore() {
  File f = LittleFS.open(STORE_PATH, FILE_WRITE);
  if (!f) {
    return false;
  }
  const Store& s = store();
  f.println(s.kValue, 6);
  f.println(s.refPpm, 3);
  f.println(s.refVoltage, 6);
  f.println(s.refTempC, 3);
  f.println(s.calibrated ? 1 : 0);
  f.close();
  return true;
}

inline bool loadStore() {
  Store& s = store();
  if (!LittleFS.exists(STORE_PATH)) {
    s.kValue = DEFAULT_K_VALUE;
    s.calibrated = false;
    return false;
  }
  File f = LittleFS.open(STORE_PATH, FILE_READ);
  if (!f) {
    return false;
  }
  s.kValue = f.parseFloat();
  s.refPpm = f.parseFloat();
  s.refVoltage = f.parseFloat();
  s.refTempC = f.parseFloat();
  s.calibrated = f.parseInt() != 0;
  f.close();
  if (!isfinite(s.kValue) || s.kValue < 0.05f || s.kValue > 20.0f) {
    s.kValue = DEFAULT_K_VALUE;
    s.calibrated = false;
    return false;
  }
  return s.calibrated;
}

inline bool clearStore() {
  Store& s = store();
  s.kValue = DEFAULT_K_VALUE;
  s.refPpm = NAN;
  s.refVoltage = NAN;
  s.refTempC = NAN;
  s.calibrated = false;
  if (LittleFS.exists(STORE_PATH)) {
    LittleFS.remove(STORE_PATH);
  }
  session().lastMessage = "cleared_to_defaults";
  session().lastOk = true;
  return true;
}

inline void enter() {
  Session& sess = session();
  sess.state = ACTIVE;
  sess.pendingK = NAN;
  sess.pendingRefPpm = NAN;
  sess.pendingVoltage = NAN;
  sess.pendingTempC = NAN;
  sess.pendingRawPpm = NAN;
  sess.lastMessage = "entered";
  sess.lastOk = true;
  Serial.println("[TDS-CAL] Entered Grove TDS probe calibration.");
  Serial.println("         Prep salt water (or 707 ppm standard), rinse probe, stir, wait stable.");
  Serial.println("         Serial: CALTDS <ppm>   e.g. CALTDS 707  (or value from a TDS pen)");
  Serial.println("         MQTT:   {\"cmd\":\"tds_cal\",\"action\":\"capture\",\"ppm\":707}");
  Serial.println("         Then EXITTDS / action save. Type TDSABORT to cancel.");
}

inline void abortSession() {
  Session& sess = session();
  sess.state = IDLE;
  sess.pendingK = NAN;
  sess.pendingRefPpm = NAN;
  sess.pendingVoltage = NAN;
  sess.pendingTempC = NAN;
  sess.pendingRawPpm = NAN;
  sess.lastMessage = "aborted";
  sess.lastOk = true;
  Serial.println("[TDS-CAL] Aborted — previous saved calibration unchanged.");
}

/**
 * Capture a single reference point.
 * @param gpioVoltage  averaged ADC pin volts (0–~2.3 V for Grove TDS)
 * @param tempC        DS18B20 water temperature (°C); 25 used if invalid
 * @param knownPpm     reference TDS at ~25 °C (standard buffer or pen reading)
 */
inline bool capturePoint(float gpioVoltage, float tempC, float knownPpm) {
  Session& sess = session();
  if (sess.state != ACTIVE) {
    sess.lastMessage = "not_active";
    sess.lastOk = false;
    Serial.println("[TDS-CAL] Not in calibration — send ENTERTDS / action enter first.");
    return false;
  }
  if (isnan(knownPpm) || knownPpm < 10.0f || knownPpm > 2000.0f) {
    sess.lastMessage = "bad_ppm";
    sess.lastOk = false;
    Serial.println("[TDS-CAL] Capture failed — known ppm must be 10–2000 (e.g. 707).");
    return false;
  }
  if (isnan(gpioVoltage) || gpioVoltage < 0.01f || gpioVoltage > 3.0f) {
    sess.lastMessage = "bad_voltage";
    sess.lastOk = false;
    Serial.println("[TDS-CAL] Capture failed — invalid probe voltage.");
    return false;
  }

  const float rawPpm = voltageToRawPpm(gpioVoltage, tempC);
  if (rawPpm < 1.0f) {
    sess.lastMessage = "raw_too_low";
    sess.lastOk = false;
    Serial.println("[TDS-CAL] Capture failed — raw TDS too low (probe dry or open?).");
    return false;
  }

  const float k = knownPpm / rawPpm;
  if (!isfinite(k) || k < 0.05f || k > 20.0f) {
    sess.lastMessage = "k_out_of_range";
    sess.lastOk = false;
    Serial.println("[TDS-CAL] Capture failed — computed kValue out of range.");
    return false;
  }

  sess.pendingK = k;
  sess.pendingRefPpm = knownPpm;
  sess.pendingVoltage = gpioVoltage;
  sess.pendingTempC = isfinite(tempC) ? tempC : 25.0f;
  sess.pendingRawPpm = rawPpm;
  sess.lastMessage = "captured";
  sess.lastOk = true;
  Serial.print("[TDS-CAL] Captured: V=");
  Serial.print(gpioVoltage, 4);
  Serial.print("  T=");
  Serial.print(sess.pendingTempC, 1);
  Serial.print(" C  raw=");
  Serial.print(rawPpm, 1);
  Serial.print(" ppm  → k=");
  Serial.print(k, 4);
  Serial.print(" for ref ");
  Serial.print(knownPpm, 1);
  Serial.println(" ppm");
  return true;
}

inline bool save() {
  Session& sess = session();
  if (sess.state != ACTIVE) {
    sess.lastMessage = "not_active";
    sess.lastOk = false;
    return false;
  }
  if (isnan(sess.pendingK)) {
    sess.lastMessage = "need_capture";
    sess.lastOk = false;
    Serial.println("[TDS-CAL] Need CALTDS <ppm> capture before save.");
    return false;
  }

  Store& s = store();
  s.kValue = sess.pendingK;
  s.refPpm = sess.pendingRefPpm;
  s.refVoltage = sess.pendingVoltage;
  s.refTempC = sess.pendingTempC;
  s.calibrated = true;

  if (!saveStore()) {
    sess.lastMessage = "persist_failed";
    sess.lastOk = false;
    Serial.println("[TDS-CAL] Fit OK but LittleFS write failed.");
    return false;
  }

  sess.state = IDLE;
  sess.lastMessage = "saved";
  sess.lastOk = true;
  Serial.print("[TDS-CAL] Saved  kValue=");
  Serial.print(s.kValue, 4);
  Serial.print("  (ref ");
  Serial.print(s.refPpm, 1);
  Serial.println(" ppm)");
  return true;
}

inline void printStatus(float liveVoltage, float liveTempC) {
  const Store& s = store();
  const Session& sess = session();
  Serial.println("[TDS-CAL] --- Grove TDS probe calibration ---");
  Serial.print("  state=");
  Serial.print(stateName());
  Serial.print("  calibrated=");
  Serial.println(s.calibrated ? "yes" : "no (k=1.0 defaults)");
  Serial.print("  kValue=");
  Serial.println(s.kValue, 4);
  if (isfinite(liveVoltage)) {
    Serial.print("  live V=");
    Serial.print(liveVoltage, 4);
    Serial.print("  T=");
    Serial.print(isfinite(liveTempC) ? liveTempC : 25.0f, 1);
    Serial.print(" C  → ");
    Serial.print(voltageToPpm(liveVoltage, liveTempC), 1);
    Serial.println(" ppm");
  }
  if (!isnan(sess.pendingK)) {
    Serial.print("  pending k=");
    Serial.print(sess.pendingK, 4);
    Serial.print("  @ ref ");
    Serial.print(sess.pendingRefPpm, 1);
    Serial.println(" ppm");
  }
  if (s.calibrated) {
    Serial.print("  stored ref ");
    Serial.print(s.refPpm, 1);
    Serial.print(" ppm @ V=");
    Serial.print(s.refVoltage, 4);
    Serial.print("  T=");
    Serial.print(s.refTempC, 1);
    Serial.println(" C");
  }
}

}  // namespace TdsProbeCal

#endif  // SMART_GARDEN_TDS_PROBE_CAL_H
