/************************************************************
 * TDS nutrient observer — strip pH-reagent salt from raw TDS
 *
 * An EC/TDS probe counts all ions. Citric acid (pH Down) and
 * potassium carbonate (pH Up) leave potassium citrate after
 * neutralisation, which inflates the sensor reading without
 * adding plant-available nutrient. The nutrient loop therefore
 * under-doses against an apparently satisfactory measurement.
 *
 * Report observer (ppm):
 *   TDS_nutrient = TDS_sensor
 *                - (κ_up·∫F_up + κ_down·∫F_down) / V(t)
 *
 * κ units: ppm·L·mL⁻¹. Defaults are provisional until a bench
 * calibration (dose known mL into known V, record settled ΔTDS)
 * replaces them via SETKAPPA. Accumulators reset on full refill
 * (CONFIRM fill_0).
 ************************************************************/

#ifndef SMART_GARDEN_TDS_NUTRIENT_OBSERVER_H
#define SMART_GARDEN_TDS_NUTRIENT_OBSERVER_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>
#include "tank_geometry.h"

namespace TdsNutrientObserver {

constexpr const char* STORE_PATH = "/tds_ph_salt.txt";
// Working-volume fallback when Grove level is unavailable (Tank::conservativeWorkingL).
constexpr float FALLBACK_VOLUME_L = 6.55f;
// Provisional κ from dilute-solute estimate (≈0.4–0.5× stock mg/L per mL
// of 500 g/L Manutec reagent). Replace with measured ΔTDS·V/mL via SETKAPPA.
constexpr float DEFAULT_KAPPA_UP = 250.0f;    // ppm·L / mL  (pH Raise / K2CO3)
constexpr float DEFAULT_KAPPA_DOWN = 200.0f;  // ppm·L / mL  (pH Lower / citric)

struct State {
  float kappaUp = DEFAULT_KAPPA_UP;
  float kappaDown = DEFAULT_KAPPA_DOWN;
  float cumPhUpMl = 0.0f;
  float cumPhDownMl = 0.0f;
  // When true, EC evaluateCheck uses TDS_nutrient; raw stays in telemetry.
  bool liveCorrection = false;  // always false since 1.9.11 (telemetry only)
  bool kappaProvisional = true;
};

inline State& state() {
  static State s;
  return s;
}

inline float volumeLFromLevelPct(int levelPercent) {
  if (levelPercent >= 0) {
    const float ml = Tank::geomMlFromGrovePct(static_cast<float>(levelPercent));
    if (ml >= 500.0f) {
      return ml / 1000.0f;
    }
  }
  return FALLBACK_VOLUME_L;
}

inline float saltOffsetPpm(float volumeL) {
  const State& s = state();
  if (volumeL < 0.1f) {
    volumeL = FALLBACK_VOLUME_L;
  }
  const float molesSaltProxy =
      s.kappaUp * s.cumPhUpMl + s.kappaDown * s.cumPhDownMl;
  return molesSaltProxy / volumeL;
}

inline float nutrientPpm(float sensorPpm, float volumeL) {
  if (isnan(sensorPpm)) {
    return NAN;
  }
  const float corrected = sensorPpm - saltOffsetPpm(volumeL);
  return corrected < 0.0f ? 0.0f : corrected;
}

inline float nutrientPpmFromLevel(float sensorPpm, int levelPercent) {
  return nutrientPpm(sensorPpm, volumeLFromLevelPct(levelPercent));
}

inline bool save() {
  State& s = state();
  File f = LittleFS.open(STORE_PATH, FILE_WRITE);
  if (!f) {
    return false;
  }
  f.printf(
      "%.6f,%.6f,%.4f,%.4f,%d,%d\n",
      s.kappaUp,
      s.kappaDown,
      s.cumPhUpMl,
      s.cumPhDownMl,
      s.liveCorrection ? 1 : 0,
      s.kappaProvisional ? 1 : 0);
  f.close();
  return true;
}

inline bool load() {
  State& s = state();
  if (!LittleFS.exists(STORE_PATH)) {
    return false;
  }
  File f = LittleFS.open(STORE_PATH, FILE_READ);
  if (!f) {
    return false;
  }
  String line = f.readStringUntil('\n');
  f.close();
  line.trim();
  if (line.length() == 0) {
    return false;
  }
  float ku = s.kappaUp;
  float kd = s.kappaDown;
  float up = 0.0f;
  float down = 0.0f;
  int live = 1;
  int prov = 1;
  const int n = sscanf(
      line.c_str(),
      "%f,%f,%f,%f,%d,%d",
      &ku,
      &kd,
      &up,
      &down,
      &live,
      &prov);
  if (n < 4) {
    return false;
  }
  if (ku >= 0.0f) {
    s.kappaUp = ku;
  }
  if (kd >= 0.0f) {
    s.kappaDown = kd;
  }
  s.cumPhUpMl = up >= 0.0f ? up : 0.0f;
  s.cumPhDownMl = down >= 0.0f ? down : 0.0f;
  // Control always uses the total TDS since 1.9.11; a saved "live" flag from
  // older firmware is ignored. The estimate stays in telemetry only.
  (void)live;
  s.liveCorrection = false;
  if (n >= 6) {
    s.kappaProvisional = (prov != 0);
  }
  return true;
}

inline void addPhUpMl(float ml) {
  if (ml <= 0.0f || isnan(ml)) {
    return;
  }
  state().cumPhUpMl += ml;
  save();
}

inline void addPhDownMl(float ml) {
  if (ml <= 0.0f || isnan(ml)) {
    return;
  }
  state().cumPhDownMl += ml;
  save();
}

inline void resetAccumulators() {
  State& s = state();
  s.cumPhUpMl = 0.0f;
  s.cumPhDownMl = 0.0f;
  save();
}

// A reading this low means the tank holds fresh water: the old solution and
// its pH-reagent salt are gone. Tap water here reads about 22 ppm. If some
// salt from pH doses in fresh water is dropped too, the nutrient reading is
// high by that amount, which only shrinks the next nutrient dose.
constexpr float PLAIN_WATER_PPM = 50.0f;

// Clear stale accumulators after a manual dump and refill (no MODE FILL).
// Returns true when it cleared them.
inline bool resetIfPlainWater(float sensorPpm) {
  State& s = state();
  if (isnan(sensorPpm) || sensorPpm >= PLAIN_WATER_PPM ||
      (s.cumPhUpMl <= 0.0f && s.cumPhDownMl <= 0.0f)) {
    return false;
  }
  resetAccumulators();
  return true;
}

inline void setKappa(float up, float down, bool markMeasured) {
  State& s = state();
  if (!isnan(up) && up >= 0.0f) {
    s.kappaUp = up;
  }
  if (!isnan(down) && down >= 0.0f) {
    s.kappaDown = down;
  }
  if (markMeasured) {
    s.kappaProvisional = false;
  }
  save();
}

inline void setLiveCorrection(bool enabled) {
  (void)enabled;  // removed in 1.9.11: control always uses the total TDS
  state().liveCorrection = false;
  save();
}

inline void printStatus(float sensorPpm, int levelPercent) {
  const State& s = state();
  const float V = volumeLFromLevelPct(levelPercent);
  const float salt = saltOffsetPpm(V);
  const float nut = nutrientPpm(sensorPpm, V);
  Serial.print("[TDS-OBS] kappa_up=");
  Serial.print(s.kappaUp, 2);
  Serial.print(" kappa_down=");
  Serial.print(s.kappaDown, 2);
  Serial.print(s.kappaProvisional ? " (provisional)" : " (measured)");
  Serial.print("  cum_up_ml=");
  Serial.print(s.cumPhUpMl, 3);
  Serial.print(" cum_down_ml=");
  Serial.print(s.cumPhDownMl, 3);
  Serial.print("  V=");
  Serial.print(V, 2);
  Serial.print(" L  salt_ppm=");
  Serial.print(salt, 1);
  Serial.print("  raw=");
  Serial.print(sensorPpm, 1);
  Serial.print("  nutrient=");
  Serial.print(nut, 1);
  Serial.print("  live=");
  Serial.println(s.liveCorrection ? "on" : "off");
}

}  // namespace TdsNutrientObserver

#endif  // SMART_GARDEN_TDS_NUTRIENT_OBSERVER_H
