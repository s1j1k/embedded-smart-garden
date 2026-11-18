/************************************************************
 * Control event CSV log (doses, level checks, scheduled checks)
 ************************************************************/

#ifndef SMART_GARDEN_CONTROL_EVENT_LOG_H
#define SMART_GARDEN_CONTROL_EVENT_LOG_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>

namespace ControlEventLog {

constexpr const char* PATH = "/control_events.csv";

constexpr const char* HEADER =
  "recorded_at_utc,uptime_ms,event,stage,current_level_pct,current_drawdown_mm,"
  "target_depth_mm,ec_before,ec_dose_ml,ec_after,ph_before,ph_dose_dir,ph_dose_ml,"
  "ph_after,gain_up,gain_down,gain_ec,water_temp_c,cumulative_session_ml,alert_flags";

struct Row {
  const char* recordedAtUtc = "";
  unsigned long uptimeMs = 0;
  const char* event = "";
  const char* stage = "";
  int currentLevelPct = -1;
  float currentDrawdownMm = NAN;
  int targetDepthMm = -1;
  float ecBefore = NAN;
  float ecDoseMl = NAN;
  float ecAfter = NAN;
  float phBefore = NAN;
  const char* phDoseDir = "";
  float phDoseMl = NAN;
  float phAfter = NAN;
  float gainUp = NAN;
  float gainDown = NAN;
  float gainEc = NAN;
  float waterTempC = NAN;
  float cumulativeSessionMl = NAN;
  const char* alertFlags = "";
};

inline void printOptionalFloat(File& file, float value, int digits) {
  if (!isnan(value)) {
    file.print(value, digits);
  }
}

inline void printOptionalInt(File& file, int value) {
  if (value >= 0) {
    file.print(value);
  }
}

inline bool append(const Row& row) {
  File file = LittleFS.open(PATH, FILE_APPEND);
  if (!file) {
    Serial.println("[ERROR] Failed to open control event CSV");
    return false;
  }

  if (file.size() == 0) {
    file.println(HEADER);
  }

  if (row.recordedAtUtc != nullptr) {
    file.print(row.recordedAtUtc);
  }
  file.print(",");
  file.print(row.uptimeMs);
  file.print(",");
  file.print(row.event != nullptr ? row.event : "");
  file.print(",");
  file.print(row.stage != nullptr ? row.stage : "");
  file.print(",");
  printOptionalInt(file, row.currentLevelPct);
  file.print(",");
  printOptionalFloat(file, row.currentDrawdownMm, 1);
  file.print(",");
  printOptionalInt(file, row.targetDepthMm);
  file.print(",");
  printOptionalFloat(file, row.ecBefore, 0);
  file.print(",");
  printOptionalFloat(file, row.ecDoseMl, 2);
  file.print(",");
  printOptionalFloat(file, row.ecAfter, 0);
  file.print(",");
  printOptionalFloat(file, row.phBefore, 2);
  file.print(",");
  file.print(row.phDoseDir != nullptr ? row.phDoseDir : "");
  file.print(",");
  printOptionalFloat(file, row.phDoseMl, 2);
  file.print(",");
  printOptionalFloat(file, row.phAfter, 2);
  file.print(",");
  printOptionalFloat(file, row.gainUp, 4);
  file.print(",");
  printOptionalFloat(file, row.gainDown, 4);
  file.print(",");
  printOptionalFloat(file, row.gainEc, 6);
  file.print(",");
  printOptionalFloat(file, row.waterTempC, 2);
  file.print(",");
  printOptionalFloat(file, row.cumulativeSessionMl, 2);
  file.print(",");
  file.println(row.alertFlags != nullptr ? row.alertFlags : "");
  file.close();
  return true;
}

inline void logToSerial(const Row& row) {
  Serial.print("[CTRL-LOG] ");
  Serial.print(row.event != nullptr ? row.event : "");
  Serial.print(" stage=");
  Serial.print(row.stage != nullptr ? row.stage : "");
  if (row.currentLevelPct >= 0) {
    Serial.print(" lvl=");
    Serial.print(row.currentLevelPct);
  }
  if (!isnan(row.currentDrawdownMm)) {
    Serial.print(" dd=");
    Serial.print(row.currentDrawdownMm, 1);
    Serial.print("mm");
  }
  if (!isnan(row.phBefore)) {
    Serial.print(" pH=");
    Serial.print(row.phBefore, 2);
  }
  if (!isnan(row.ecBefore)) {
    Serial.print(" EC=");
    Serial.print(row.ecBefore, 0);
  }
  if (row.alertFlags != nullptr && row.alertFlags[0] != '\0') {
    Serial.print(" ALERT=");
    Serial.print(row.alertFlags);
  }
  Serial.println();
}

}  // namespace ControlEventLog

#endif  // SMART_GARDEN_CONTROL_EVENT_LOG_H
