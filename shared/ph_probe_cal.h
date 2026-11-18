/************************************************************
 * SEN0169 software two-point pH probe calibration
 *
 * Datasheet Method 1 (software): fit a line through acid (4.00)
 * and alkali (9.18 / 10.00) buffer voltages on the adapter board
 * scale (0–5 V). Persist slope + intercept to LittleFS.
 *
 * Voltage path: probe → SEN0169 board (0–5 V) → discrete 10 kΩ / 20 kΩ
 * divider (tap across the 20 kΩ → ≤ ~3.33 V at GPIO) → ESP32 ADC @ ADC_11db.
 * Callers pass *board* volts (GPIO volts × PH_SIGNAL_LEVEL_SHIFT_GAIN,
 * currently 1.5 = (10k+20k)/20k).
 *
 * Buffer temperature at calibration:
 *   Each capture reads the DS18B20, which must sit in the same buffer as
 *   the pH probe. The buffer's true pH at that temperature comes from the
 *   sachet table (BUFFER_TABLE_* below; 4.00 barely moves, 9.18 reads 9.23
 *   at 20 °C). The electrode output at T is then mapped back to the 25 °C
 *   line the runtime compensation expects:
 *     pH25_eq = 7 + (pH_buffer(T) − 7) × (273.15 + T) / 298.15
 *   and the line is fit through (V, pH25_eq). With no valid DS18B20 the
 *   capture is refused unless a hand-measured buffer temperature is given
 *   (Serial CALPH ACID 21.5 / MQTT "temp_c").
 *
 * Temperature compensation (glass electrode / Nernst):
 *   At run time, convert board volts with the stored 25 °C line, then
 *   scale deviation from the isopotential point (pH 7) by the
 *   absolute-temperature ratio:
 *     T_C  = DS18B20 water temperature (°C); 25 °C if the probe is invalid
 *     pH25 = slope * V_board + offset
 *     pH   = 7 + (pH25 − 7) × (298.15 / (273.15 + T_C))
 *   DFRobot_PH advertises a temperature argument but does not apply it;
 *   this project implements the standard Nernst correction using the
 *   tank DS18B20 (same water-temp source as TDS compensation).
 *
 * Uncalibrated defaults match DFRobot_PH factory acid/neutral points
 * (2.03244 V @ pH 4.0, 1.500 V @ pH 7.0) until ENTERPH / MQTT ph_cal.
 * Store path is versioned so older 50/50-divider calibrations are ignored.
 *
 * Serial: ENTERPH / CALPH ACID|ALKALI [buffer °C] / EXITPH / PHSHOW / PHCLEAR
 * MQTT:   {"cmd":"ph_cal","action":"enter|capture|save|abort|clear",
 *          "point":"acid|alkali","nominal":4.00,"temp_c":21.5}
 *   nominal = the buffer's label (25 °C) value; temp_c overrides the DS18B20.
 ************************************************************/

#ifndef SMART_GARDEN_PH_PROBE_CAL_H
#define SMART_GARDEN_PH_PROBE_CAL_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>

namespace PhProbeCal {

// Versioned path: orphan older LittleFS files from the 50/50 shifter era.
constexpr const char* STORE_PATH = "/ph_probe_cal_v2.txt";

// Provisional line, 11 Oct 2026 evening: probe 2 on the PCB (10k/20k divider),
// out of buffers, so a least-squares fit through three strip-checked points at
// 24.2 C: tank 4.0 @ 1.832 V, plain tap water 7.0 @ 2.03 V, and a drop of pH-up
// in water (~10.7, potassium carbonate) @ 2.2778 V
//   → slope ≈ 14.9848, offset ≈ -23.4352 (66.7 mV/pH; within 0.02 pH of all three)
// Strip values are only good to about ±0.5, so recalibrate with real 4.00 / 9.18
// buffers when they arrive. Earlier lines: 16 Sep breadboard 15.36963 / -23.96984;
// 11 Oct midday buffers 31.33587 / -52.30564; 11 Oct two-cup 11.2377 / -16.6140.
// Baked into firmware so a LittleFS wipe still recovers a usable line;
// ENTERPH / MQTT ph_cal overwrite LittleFS as usual, PHRESTORE rewrites this one.
// (DFRobot library factory line was −5.6344 / +15.4516 — wrong for this harness.)
constexpr float DEFAULT_SLOPE = 14.9848f;
constexpr float DEFAULT_OFFSET = -23.4352f;
constexpr float DEFAULT_ACID_V = 1.832f;
constexpr float DEFAULT_ALKALI_V = 2.2778f;

constexpr float ACID_NOMINAL = 4.00f;
constexpr float ALKALI_NOMINAL = 9.18f;

// Plausible board-voltage windows (SEN0169 / this harness, ~25 °C).
// Alkali upper bound raised: this board's 9.18 buffer sat near 2.16 V.
constexpr float ACID_V_MIN = 1.70f;
constexpr float ACID_V_MAX = 2.40f;
constexpr float ALKALI_V_MIN = 1.20f;
constexpr float ALKALI_V_MAX = 2.30f;

// Same sentinel as DS18B20 / TdsProbeCal when the water probe fails.
constexpr float INVALID_WATER_TEMP_C = -127.0f;
// Two-point cal and DFRobot factory line assume ~25 °C buffers.
constexpr float CAL_TEMP_C = 25.0f;
// Typical glass-electrode isopotential point (zero-temp-coefficient pH).
constexpr float ISOPOTENTIAL_PH = 7.0f;

// Buffer pH vs temperature, from the back of the powder-buffer sachets
// (accuracy ±0.01 pH). Rows every 5 °C from BUFFER_TABLE_MIN_C.
constexpr float BUFFER_TABLE_MIN_C = 10.0f;
constexpr float BUFFER_TABLE_STEP_C = 5.0f;
constexpr int BUFFER_TABLE_ROWS = 9;  // 10 … 50 °C
constexpr float BUFFER_TABLE_400[BUFFER_TABLE_ROWS] = {
    4.00f, 4.00f, 4.00f, 4.00f, 4.01f, 4.02f, 4.03f, 4.04f, 4.06f};
constexpr float BUFFER_TABLE_918[BUFFER_TABLE_ROWS] = {
    9.33f, 9.28f, 9.23f, 9.18f, 9.14f, 9.10f, 9.07f, 9.04f, 9.02f};

enum State {
  IDLE = 0,
  ACTIVE,
};

struct Store {
  float slope = DEFAULT_SLOPE;
  float offset = DEFAULT_OFFSET;
  float acidVoltage = DEFAULT_ACID_V;
  float alkaliVoltage = DEFAULT_ALKALI_V;
  // Fitted pH values: the buffers' 25 °C equivalents (see header comment).
  float acidNominal = ACID_NOMINAL;
  float alkaliNominal = ALKALI_NOMINAL;
  bool calibrated = true;  // breadboard defaults above are a real two-point fit
  // Buffer temperature and true buffer pH at that temperature per capture.
  // NAN for the baked default line (no buffer temperature recorded).
  float acidTempC = NAN;
  float alkaliTempC = NAN;
  float acidBufferPh = NAN;
  float alkaliBufferPh = NAN;
};

struct Session {
  State state = IDLE;
  float pendingAcidV = NAN;
  float pendingAlkaliV = NAN;
  float pendingAcidNom = ACID_NOMINAL;
  float pendingAlkaliNom = ALKALI_NOMINAL;
  float pendingAcidTempC = NAN;
  float pendingAlkaliTempC = NAN;
  float pendingAcidBufferPh = NAN;
  float pendingAlkaliBufferPh = NAN;
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

inline float slope() {
  return store().slope;
}

inline float offset() {
  return store().offset;
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

/** Water temp used for pH: DS18B20 °C, or 25 °C if the probe is invalid. */
inline float effectiveTempC(float waterTempC) {
  if (!isfinite(waterTempC) || waterTempC <= INVALID_WATER_TEMP_C ||
      waterTempC < -20.0f || waterTempC > 70.0f) {
    return CAL_TEMP_C;
  }
  return waterTempC;
}

/**
 * Nernst factor: S(T_cal) / S(T) = T_cal_K / T_meas_K.
 * At T > 25 °C the electrode is steeper, so |pH − 7| shrinks vs the 25 °C line.
 */
inline float compensationFactor(float waterTempC) {
  const float tK = 273.15f + effectiveTempC(waterTempC);
  if (tK < 200.0f) {
    return 1.0f;
  }
  return (273.15f + CAL_TEMP_C) / tK;
}

/** Apply Nernst compensation to a pH value from the 25 °C calibration line. */
inline float applyTempCompensation(float phAtCalTemp, float waterTempC) {
  if (!isfinite(phAtCalTemp)) {
    return phAtCalTemp;
  }
  return ISOPOTENTIAL_PH +
         (phAtCalTemp - ISOPOTENTIAL_PH) * compensationFactor(waterTempC);
}

/** Board volts → pH using stored slope/offset only (no temperature correction). */
inline float voltageToPhUncompensated(float boardVoltage) {
  return store().slope * boardVoltage + store().offset;
}

/**
 * Board volts → temperature-compensated pH.
 * Pass the DS18B20 water temperature; invalid / missing temps fall back to 25 °C.
 */
inline float voltageToPh(float boardVoltage, float waterTempC = NAN) {
  return applyTempCompensation(voltageToPhUncompensated(boardVoltage), waterTempC);
}

/** True when a DS18B20 / hand-entered temperature is a usable reading. */
inline bool isValidTempC(float tempC) {
  return isfinite(tempC) && tempC > INVALID_WATER_TEMP_C && tempC >= -20.0f &&
         tempC <= 70.0f;
}

/**
 * True pH of a labelled buffer (4.00 or 9.18) at tempC, interpolated from
 * the sachet table and held at the end rows outside 10–50 °C. NAN for a
 * label with no table (the caller then uses the label as-is).
 */
inline float bufferPhAtTemp(float labelPh, float tempC) {
  const float* table = nullptr;
  if (fabsf(labelPh - 4.00f) < 0.005f) {
    table = BUFFER_TABLE_400;
  } else if (fabsf(labelPh - 9.18f) < 0.005f) {
    table = BUFFER_TABLE_918;
  }
  if (table == nullptr || !isfinite(tempC)) {
    return NAN;
  }
  const float maxC =
      BUFFER_TABLE_MIN_C + BUFFER_TABLE_STEP_C * (BUFFER_TABLE_ROWS - 1);
  const float t = fminf(fmaxf(tempC, BUFFER_TABLE_MIN_C), maxC);
  const float pos = (t - BUFFER_TABLE_MIN_C) / BUFFER_TABLE_STEP_C;
  int i = static_cast<int>(pos);
  if (i >= BUFFER_TABLE_ROWS - 1) {
    i = BUFFER_TABLE_ROWS - 2;
  }
  return table[i] + (table[i + 1] - table[i]) * (pos - i);
}

/**
 * pH the 25 °C line must assign to a buffer of pH phAtT at tempC, so that
 * voltageToPh(V, tempC) gives phAtT back (inverse of the Nernst step).
 */
inline float calTempEquivalentPh(float phAtT, float tempC) {
  return ISOPOTENTIAL_PH +
         (phAtT - ISOPOTENTIAL_PH) * (273.15f + tempC) / (273.15f + CAL_TEMP_C);
}

inline bool computeLine(float vAcid, float phAcid, float vAlkali, float phAlkali,
                        float& outSlope, float& outOffset) {
  // Pure two-point line: pH = m * V_board + b from the captured (V, pH) pairs.
  // No assumed polarity — slope follows whatever the two points imply.
  const float dv = vAlkali - vAcid;
  if (fabsf(dv) < 1e-4f) {
    return false;  // identical voltages → undefined slope
  }
  outSlope = (phAlkali - phAcid) / dv;
  outOffset = phAcid - outSlope * vAcid;
  return isfinite(outSlope) && isfinite(outOffset);
}

inline bool saveStore() {
  File f = LittleFS.open(STORE_PATH, FILE_WRITE);
  if (!f) {
    return false;
  }
  const Store& s = store();
  f.println(s.slope, 6);
  f.println(s.offset, 6);
  f.println(s.acidVoltage, 6);
  f.println(s.alkaliVoltage, 6);
  f.println(s.acidNominal, 3);
  f.println(s.alkaliNominal, 3);
  f.println(s.calibrated ? 1 : 0);
  // Appended in 1.9.15; "nan" when unknown.
  f.println(s.acidTempC, 2);
  f.println(s.alkaliTempC, 2);
  f.println(s.acidBufferPh, 3);
  f.println(s.alkaliBufferPh, 3);
  f.close();
  return true;
}

/** Next line of the store as a float; NAN when absent or "nan". */
inline float readOptionalFloat(File& f) {
  while (f.available() && isspace(f.peek())) {
    f.read();
  }
  if (!f.available()) {
    return NAN;
  }
  String tok = f.readStringUntil('\n');
  tok.trim();
  if (tok.length() == 0 || tok.startsWith("nan") || tok.startsWith("inf")) {
    return NAN;
  }
  return tok.toFloat();
}

inline void applyBreadboardDefaults() {
  Store& s = store();
  s.slope = DEFAULT_SLOPE;
  s.offset = DEFAULT_OFFSET;
  s.acidVoltage = DEFAULT_ACID_V;
  s.alkaliVoltage = DEFAULT_ALKALI_V;
  s.acidNominal = ACID_NOMINAL;
  s.alkaliNominal = ALKALI_NOMINAL;
  s.calibrated = true;
  s.acidTempC = NAN;
  s.alkaliTempC = NAN;
  s.acidBufferPh = NAN;
  s.alkaliBufferPh = NAN;
}

/** Re-apply the baked default line (DEFAULT_*) and persist to LittleFS. */
inline bool restoreBreadboardCalibration() {
  applyBreadboardDefaults();
  if (!saveStore()) {
    session().lastMessage = "restore_persist_failed";
    session().lastOk = false;
    Serial.println("[PH-CAL] Restored in RAM but LittleFS write failed.");
    return false;
  }
  session().lastMessage = "restored_breadboard";
  session().lastOk = true;
  Serial.println("[PH-CAL] Restored default (provisional 2026-10-11) cal → LittleFS");
  Serial.print("         pH = ");
  Serial.print(store().slope, 4);
  Serial.print(" * V_board + ");
  Serial.println(store().offset, 4);
  return true;
}

inline bool loadStore() {
  Store& s = store();
  if (!LittleFS.exists(STORE_PATH)) {
    // Missing file (flash wipe / format) — reinstate the baked default line.
    applyBreadboardDefaults();
    if (saveStore()) {
      Serial.println("[PH-CAL] No store on flash — wrote default (provisional 2026-10-11) cal");
    } else {
      Serial.println("[PH-CAL] No store on flash — using default cal in RAM only");
    }
    return true;
  }
  File f = LittleFS.open(STORE_PATH, FILE_READ);
  if (!f) {
    applyBreadboardDefaults();
    return true;
  }
  s.slope = f.parseFloat();
  s.offset = f.parseFloat();
  s.acidVoltage = f.parseFloat();
  s.alkaliVoltage = f.parseFloat();
  s.acidNominal = f.parseFloat();
  s.alkaliNominal = f.parseFloat();
  s.calibrated = f.parseInt() != 0;
  // Stores written before 1.9.15 end here (buffer temperature unknown).
  s.acidTempC = readOptionalFloat(f);
  s.alkaliTempC = readOptionalFloat(f);
  s.acidBufferPh = readOptionalFloat(f);
  s.alkaliBufferPh = readOptionalFloat(f);
  f.close();
  if (!isfinite(s.slope) || !isfinite(s.offset)) {
    applyBreadboardDefaults();
    saveStore();
    return true;
  }
  // Legacy/corrupt "uncalibrated factory" file left after a wipe — replace.
  if (!s.calibrated) {
    applyBreadboardDefaults();
    saveStore();
    Serial.println("[PH-CAL] Replaced uncalibrated store with default cal");
    return true;
  }
  return s.calibrated;
}

inline bool clearStore() {
  // PHCLEAR intentionally drops back to the baked default line (and
  // rewrites LittleFS), not the DFRobot factory defaults.
  return restoreBreadboardCalibration();
}

inline void enter() {
  Session& sess = session();
  sess.state = ACTIVE;
  sess.pendingAcidV = NAN;
  sess.pendingAlkaliV = NAN;
  sess.pendingAcidNom = ACID_NOMINAL;
  sess.pendingAlkaliNom = ALKALI_NOMINAL;
  sess.pendingAcidTempC = NAN;
  sess.pendingAlkaliTempC = NAN;
  sess.pendingAcidBufferPh = NAN;
  sess.pendingAlkaliBufferPh = NAN;
  sess.lastMessage = "entered";
  sess.lastOk = true;
  Serial.println("[PH-CAL] Entered probe calibration.");
  Serial.println("         Put the pH probe AND the DS18B20 water-temp probe in the");
  Serial.println("         same buffer. Wait ~2 min until PHSHOW volts and temperature");
  Serial.println("         stop moving, then capture. Rinse both probes in DI water");
  Serial.println("         between buffers. Buffer pH is read from the sachet table");
  Serial.println("         at the measured temperature (e.g. 9.18 is 9.23 at 20 C).");
  Serial.println("         Serial: CALPH ACID  (buffer 4.00) or CALPH ALKALI (9.18)");
  Serial.println("                 no DS18B20? give the buffer C: CALPH ACID 21.5");
  Serial.println("         MQTT:   {\"cmd\":\"ph_cal\",\"action\":\"capture\",\"point\":\"acid\"}");
  Serial.println("         Then EXITPH / action save. Type PHABORT to cancel.");
  Serial.println("         Afterwards put the DS18B20 back in the tank.");
}

inline void abortSession() {
  Session& sess = session();
  sess.state = IDLE;
  sess.pendingAcidV = NAN;
  sess.pendingAlkaliV = NAN;
  sess.lastMessage = "aborted";
  sess.lastOk = true;
  Serial.println("[PH-CAL] Aborted — previous saved calibration unchanged.");
}

/**
 * Capture one buffer. labelPh = the buffer's printed value (NAN → 4.00 /
 * 9.18 by point); bufferTempC = DS18B20 or hand-entered buffer °C.
 */
inline bool capturePoint(const char* point, float boardVoltage, float labelPh,
                         float bufferTempC) {
  Session& sess = session();
  if (sess.state != ACTIVE) {
    sess.lastMessage = "not_active";
    sess.lastOk = false;
    Serial.println("[PH-CAL] Not in calibration — send ENTERPH / action enter first.");
    return false;
  }
  if (isnan(boardVoltage) || boardVoltage < 0.2f || boardVoltage > 4.8f) {
    sess.lastMessage = "bad_voltage";
    sess.lastOk = false;
    Serial.println("[PH-CAL] Capture failed — invalid board voltage.");
    return false;
  }

  String p(point != nullptr ? point : "");
  p.toLowerCase();
  const bool acid = (p == "acid" || p == "4" || p == "4.00" || p == "4.0");
  const bool alkali = (p == "alkali" || p == "base" || p == "9.18" || p == "9" ||
                       p == "10" || p == "10.00");
  if (!acid && !alkali) {
    sess.lastMessage = "unknown_point";
    sess.lastOk = false;
    Serial.println("[PH-CAL] Unknown point — use acid or alkali.");
    return false;
  }
  if (!isValidTempC(bufferTempC)) {
    sess.lastMessage = "no_buffer_temp";
    sess.lastOk = false;
    Serial.println("[PH-CAL] Capture refused — no valid DS18B20 reading. Put the");
    Serial.println("         DS18B20 in the buffer, or give the buffer temperature");
    Serial.println("         measured by hand: CALPH ACID 21.5");
    return false;
  }

  const char* name = acid ? "Acid" : "Alkali";
  float label = labelPh;
  if (isnan(label)) {
    label = acid ? ACID_NOMINAL : (p.startsWith("10") ? 10.00f : ALKALI_NOMINAL);
  }
  float bufferPh = bufferPhAtTemp(label, bufferTempC);
  if (isnan(bufferPh)) {
    bufferPh = label;
    Serial.print("[PH-CAL] WARNING: no temperature table for a ");
    Serial.print(label, 2);
    Serial.println(" buffer — using the label value.");
  }
  if (bufferTempC < BUFFER_TABLE_MIN_C ||
      bufferTempC > BUFFER_TABLE_MIN_C + BUFFER_TABLE_STEP_C * (BUFFER_TABLE_ROWS - 1)) {
    Serial.println("[PH-CAL] WARNING: buffer outside the 10–50 C table — using the end row.");
  }
  const float ph25 = calTempEquivalentPh(bufferPh, bufferTempC);

  if (acid && (boardVoltage < ACID_V_MIN || boardVoltage > ACID_V_MAX)) {
    Serial.print("[PH-CAL] WARNING: acid voltage ");
    Serial.print(boardVoltage, 3);
    Serial.println(" V outside typical 1.70–2.40 V — capturing anyway.");
  }
  if (alkali && (boardVoltage < ALKALI_V_MIN || boardVoltage > ALKALI_V_MAX)) {
    Serial.print("[PH-CAL] WARNING: alkali voltage ");
    Serial.print(boardVoltage, 3);
    Serial.println(" V outside typical 1.20–2.30 V — capturing anyway.");
  }

  if (acid) {
    sess.pendingAcidV = boardVoltage;
    sess.pendingAcidNom = ph25;
    sess.pendingAcidTempC = bufferTempC;
    sess.pendingAcidBufferPh = bufferPh;
    sess.lastMessage = "acid_captured";
  } else {
    sess.pendingAlkaliV = boardVoltage;
    sess.pendingAlkaliNom = ph25;
    sess.pendingAlkaliTempC = bufferTempC;
    sess.pendingAlkaliBufferPh = bufferPh;
    sess.lastMessage = "alkali_captured";
  }
  sess.lastOk = true;
  Serial.print("[PH-CAL] ");
  Serial.print(name);
  Serial.print(" point captured: ");
  Serial.print(boardVoltage, 4);
  Serial.print(" V  buffer ");
  Serial.print(label, 2);
  Serial.print(" @ ");
  Serial.print(bufferTempC, 1);
  Serial.print(" C = pH ");
  Serial.print(bufferPh, 3);
  Serial.print("  (25 C line: ");
  Serial.print(ph25, 3);
  Serial.println(")");
  return true;
}

inline bool save() {
  Session& sess = session();
  if (sess.state != ACTIVE) {
    sess.lastMessage = "not_active";
    sess.lastOk = false;
    return false;
  }
  if (isnan(sess.pendingAcidV) || isnan(sess.pendingAlkaliV)) {
    sess.lastMessage = "need_both_points";
    sess.lastOk = false;
    Serial.println("[PH-CAL] Need both acid and alkali captures before save.");
    return false;
  }

  float newSlope = 0.0f;
  float newOffset = 0.0f;
  if (!computeLine(sess.pendingAcidV, sess.pendingAcidNom, sess.pendingAlkaliV,
                   sess.pendingAlkaliNom, newSlope, newOffset)) {
    sess.lastMessage = "fit_failed";
    sess.lastOk = false;
    Serial.println("[PH-CAL] Fit failed — acid and alkali voltages are identical.");
    return false;
  }

  Store& s = store();
  s.slope = newSlope;
  s.offset = newOffset;
  s.acidVoltage = sess.pendingAcidV;
  s.alkaliVoltage = sess.pendingAlkaliV;
  s.acidNominal = sess.pendingAcidNom;
  s.alkaliNominal = sess.pendingAlkaliNom;
  s.calibrated = true;
  s.acidTempC = sess.pendingAcidTempC;
  s.alkaliTempC = sess.pendingAlkaliTempC;
  s.acidBufferPh = sess.pendingAcidBufferPh;
  s.alkaliBufferPh = sess.pendingAlkaliBufferPh;

  if (!saveStore()) {
    sess.lastMessage = "persist_failed";
    sess.lastOk = false;
    Serial.println("[PH-CAL] Fit OK but LittleFS write failed.");
    return false;
  }

  sess.state = IDLE;
  sess.lastMessage = "saved";
  sess.lastOk = true;
  Serial.print("[PH-CAL] Saved  pH = ");
  Serial.print(s.slope, 4);
  Serial.print(" * V_board + ");
  Serial.println(s.offset, 4);
  Serial.println("[PH-CAL] Do not adjust the board gain pot after software calibration.");
  return true;
}

inline void printStatus(float liveBoardVoltage, float waterTempC = NAN) {
  const Store& s = store();
  const Session& sess = session();
  Serial.println("[PH-CAL] --- probe calibration ---");
  Serial.print("  state=");
  Serial.print(stateName());
  Serial.print("  calibrated=");
  Serial.println(s.calibrated ? "yes" : "no");
  Serial.print("  slope=");
  Serial.print(s.slope, 4);
  Serial.print("  offset=");
  Serial.println(s.offset, 4);
  Serial.print("  water T=");
  Serial.print(effectiveTempC(waterTempC), 1);
  Serial.print(" C  Nernst factor=");
  Serial.println(compensationFactor(waterTempC), 4);
  Serial.print("  live board V=");
  Serial.print(liveBoardVoltage, 4);
  Serial.print("  → pH ");
  Serial.print(voltageToPhUncompensated(liveBoardVoltage), 3);
  Serial.print(" (raw) / ");
  Serial.print(voltageToPh(liveBoardVoltage, waterTempC), 3);
  Serial.println(" (temp-comp)");
  if (sess.state == ACTIVE && isValidTempC(waterTempC)) {
    Serial.print("  buffers at ");
    Serial.print(waterTempC, 1);
    Serial.print(" C: 4.00 → ");
    Serial.print(bufferPhAtTemp(ACID_NOMINAL, waterTempC), 2);
    Serial.print("  9.18 → ");
    Serial.println(bufferPhAtTemp(ALKALI_NOMINAL, waterTempC), 2);
  } else if (sess.state == ACTIVE) {
    Serial.println("  buffer temp: DS18B20 invalid — capture needs CALPH ACID <C>");
  }
  if (sess.state == ACTIVE && !isnan(sess.pendingAcidV)) {
    Serial.print("  pending acid V=");
    Serial.print(sess.pendingAcidV, 4);
    Serial.print(" @ ");
    Serial.print(sess.pendingAcidTempC, 1);
    Serial.print(" C  pH ");
    Serial.println(sess.pendingAcidBufferPh, 3);
  }
  if (sess.state == ACTIVE && !isnan(sess.pendingAlkaliV)) {
    Serial.print("  pending alkali V=");
    Serial.print(sess.pendingAlkaliV, 4);
    Serial.print(" @ ");
    Serial.print(sess.pendingAlkaliTempC, 1);
    Serial.print(" C  pH ");
    Serial.println(sess.pendingAlkaliBufferPh, 3);
  }
  if (s.calibrated) {
    Serial.print("  stored acid V=");
    Serial.print(s.acidVoltage, 4);
    Serial.print(" @ pH25 ");
    Serial.print(s.acidNominal, 3);
    if (!isnan(s.acidTempC)) {
      Serial.print("  (buffer ");
      Serial.print(s.acidBufferPh, 3);
      Serial.print(" @ ");
      Serial.print(s.acidTempC, 1);
      Serial.print(" C)");
    }
    Serial.println();
    Serial.print("  stored alkali V=");
    Serial.print(s.alkaliVoltage, 4);
    Serial.print(" @ pH25 ");
    Serial.print(s.alkaliNominal, 3);
    if (!isnan(s.alkaliTempC)) {
      Serial.print("  (buffer ");
      Serial.print(s.alkaliBufferPh, 3);
      Serial.print(" @ ");
      Serial.print(s.alkaliTempC, 1);
      Serial.print(" C)");
    }
    Serial.println();
  }
}

}  // namespace PhProbeCal

#endif  // SMART_GARDEN_PH_PROBE_CAL_H
