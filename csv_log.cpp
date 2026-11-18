/* LittleFS CSV schema and append. */
#include "garden.h"

/************************************************************
 * CSV Logging (LittleFS)
 ************************************************************/

const char* phDoseLabel() {
  switch (phControlState) {
    case PH_DOSING_UP: return "up";
    case PH_DOSING_DOWN: return "down";
    case PH_MIXING: return "mixing";
    default: return "none";
  }
}

const char* nutrientDoseLabel() {
  switch (nutrientControlState) {
    case NUTRIENT_DOSING: return "dose";
    case NUTRIENT_DILUTING: return "dilute";
    case NUTRIENT_MIXING: return "mixing";
    default: return "none";
  }
}

static void clearCsvIfOldSchema(const char* path) {
  if (!LittleFS.exists(path)) {
    return;
  }

  File existing = LittleFS.open(path, FILE_READ);
  if (!existing) {
    return;
  }

  String firstLine = existing.readStringUntil('\n');
  existing.close();

  if (SensorDataStore::csvHeaderLooksCurrent(firstLine)) {
    return;
  }

  if (!LittleFS.remove(path)) {
    Serial.print("[ERROR] Failed to clear ");
    Serial.print(path);
    Serial.println(" after schema change");
    return;
  }

  Serial.print("[WARN] CSV schema updated for SQLite-ready columns; cleared ");
  Serial.println(path);
}

static size_t fileSizeOrZero(const char* path) {
  if (!LittleFS.exists(path)) {
    return 0;
  }
  File f = LittleFS.open(path, FILE_READ);
  if (!f) {
    return 0;
  }
  const size_t size = f.size();
  f.close();
  return size;
}

// Keep the current journal as the previous one (replacing an older copy).
static bool archiveJournal(const char* why) {
  if (!LittleFS.exists(CSV_PATH)) {
    return true;
  }
  LittleFS.remove(CSV_PREV_PATH);
  if (!LittleFS.rename(CSV_PATH, CSV_PREV_PATH)) {
    Serial.print("[ERROR] Could not archive ");
    Serial.print(CSV_PATH);
    Serial.println(" — removing it so logging can continue");
    LittleFS.remove(CSV_PATH);
  } else {
    Serial.print("[WARN] ");
    Serial.print(CSV_PATH);
    Serial.print(" archived to ");
    Serial.print(CSV_PREV_PATH);
    Serial.print(" (");
    Serial.print(why);
    Serial.println(") — DUMP PREV reads it");
  }
  csvHeaderWritten = false;
  return true;
}

void ensureCsvSchema() {
  // An older column layout is archived, not deleted: it is the only copy of
  // the readings logged before the upgrade.
  if (LittleFS.exists(CSV_PATH)) {
    File existing = LittleFS.open(CSV_PATH, FILE_READ);
    String firstLine = existing ? existing.readStringUntil('\n') : String();
    if (existing) {
      existing.close();
    }
    if (!SensorDataStore::csvHeaderLooksCurrent(firstLine)) {
      archiveJournal("older column layout");
    }
  }
  // Pre-1.8.0 replay buffer: every row in it is also in the journal, and it
  // was rewritten on every replay. The append-only outbox replaces it.
  if (LittleFS.exists(SensorDataStore::MQTT_UNSENT_PATH)) {
    LittleFS.remove(SensorDataStore::MQTT_UNSENT_PATH);
    Serial.println("[DEBUG] Removed pre-1.8.0 /mqtt_unsent.csv (rows are in the journal)");
  }
  // The outbox has no header and its replay parser reads columns by
  // position: drop it if its first row has a different column count.
  if (LittleFS.exists(SensorDataStore::MQTT_OUTBOX_PATH)) {
    File f = LittleFS.open(SensorDataStore::MQTT_OUTBOX_PATH, FILE_READ);
    const String first = f ? f.readStringUntil('\n') : String();
    if (f) {
      f.close();
    }
    int commas = 0;
    for (unsigned int i = 0; i < first.length(); i++) {
      commas += first[i] == ',' ? 1 : 0;
    }
    if (commas != SensorDataStore::CSV_COLUMN_COUNT - 1) {
      LittleFS.remove(SensorDataStore::MQTT_OUTBOX_PATH);
      LittleFS.remove(SensorDataStore::MQTT_OUTBOX_CURSOR_PATH);
      Serial.println("[WARN] MQTT outbox had another column layout — cleared (rows are in the journal)");
    }
  }
  if (!LittleFS.exists(CSV_PATH)) {
    csvHeaderWritten = false;
  }
}

void rotateCsvIfNeeded() {
  const size_t freeBytes = LittleFS.totalBytes() - LittleFS.usedBytes();
  const size_t fileBytes = fileSizeOrZero(CSV_PATH);

  if (fileBytes >= CSV_MAX_BYTES) {
    archiveJournal("size cap");
    return;
  }
  if (freeBytes > CSV_MIN_FREE_BYTES) {
    return;
  }
  // Low on flash: drop the archived journal first, the live one last.
  if (LittleFS.exists(CSV_PREV_PATH)) {
    LittleFS.remove(CSV_PREV_PATH);
    Serial.println("[WARN] Flash low — removed the archived journal");
    return;
  }
  if (LittleFS.exists(CSV_PATH) && !LittleFS.remove(CSV_PATH)) {
    Serial.println("[ERROR] Failed to rotate CSV; logging may stall when flash is full");
    return;
  }
  csvHeaderWritten = false;
  Serial.print("[WARN] CSV removed to free space (was ");
  Serial.print(fileBytes);
  Serial.print(" B; free was ");
  Serial.print(freeBytes);
  Serial.println(" B)");
}

uint32_t nextReadingSeq() {
  return ++readingSeq;
}

// Journal one row and return its seq. lineOut (optional) receives the exact
// CSV line, which the MQTT outbox stores if the live publish misses.
uint32_t saveSensorDataToCSV(char* lineOut, size_t lineOutLen) {
  const GrowPreset& preset = activePreset();

  rotateCsvIfNeeded();

  char recordedAtUtc[24] = "";
#if ENABLE_WIFI
  recordedAtUtcForUptime(
      millis(),
      recordedAtUtc,
      sizeof(recordedAtUtc));
#endif

  SensorDataStore::CsvReading row;
  row.deviceId = MQTT_DEVICE_ID;
  row.recordedAtUtc = recordedAtUtc;
  row.uptimeMs = millis();
  row.source = "esp32";
  row.growStage = systemModeName();
  row.waterTempC = waterTempC;
  row.lightAdc = ldrADC;
  row.waterLevelPct = waterLevelPercent;
  row.waterLevelRawPct = waterLevelRawPercent;
  row.waterTargetPct = waterStageTargetPercent();
  row.waterMaxPct = waterCeilingPercent();
  row.tdsAdc = tdsADC;
  row.tdsPpm = tdsPPM;
  row.nutrientTdsMin = preset.nutrientTdsMinPpm;
  row.nutrientTdsMax = preset.nutrientTdsMaxPpm;
  row.nutrientDose = nutrientDoseLabel();
  row.phVoltage = phVoltage;
  row.ph = phValue;
  row.phFiltered = phValueFiltered;
  row.phEma = phValueEma;
  row.tdsPpmEma = tdsPpmEma;
  if (phControlAllowedByMode()) {
    row.phTargetMin = preset.phMin;
    row.phTargetMax = preset.phMax;
  }
  row.phDose = phDoseLabel();
  row.co2Ppm = co2PPM;
  row.ambientTempC = ambientTempC;
  row.ambientHumidityPct = ambientHumidityPct;
  row.waterPumpOn = waterRefillPumpRunning ? 1 : 0;
  row.drainPumpOn = waterDrainPumpRunning ? 1 : 0;
  row.nutrientPumpOn = nutrientControlState == NUTRIENT_DOSING ? 1 : 0;
  row.phUpPumpOn = phControlState == PH_DOSING_UP ? 1 : 0;
  row.phDownPumpOn = phControlState == PH_DOSING_DOWN ? 1 : 0;
  #if ENABLE_GROW_LIGHT
  {
    const GrowLight::Status gl = GrowLight::status();
    row.growLightOn = gl.on ? 1 : 0;
    row.growLightBrightnessPct = gl.brightnessPct;
    row.growLightAnimating = gl.animating ? 1 : 0;
  }
  #endif
  row.resetReason = PowerMonitor::lastResetReasonName();
  row.bootCount = static_cast<int>(PowerMonitor::bootCountValue());
  row.brownoutCount = static_cast<int>(PowerMonitor::brownoutCountValue());
  row.railMv = PowerMonitor::readRailMv();
  row.freeHeapB = PowerMonitor::freeHeapBytes();
  row.wifiRssiValid = PowerMonitor::wifiRssiValid();
  row.wifiRssiDbm = PowerMonitor::wifiRssiDbm();
  row.bootId = bootId;
  row.seq = nextReadingSeq();

  if (SensorDataStore::appendCsvReading(row, CSV_PATH, lineOut, lineOutLen)) {
    csvHeaderWritten = true;
  }
  return row.seq;
}
