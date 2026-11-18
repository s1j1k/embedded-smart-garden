/************************************************************
 * Shared LittleFS sensor persistence
 *
 * Used by embedded-smart-garden.ino and diagnostic sketches in subfolders
 * (water_drain_test, i2c_sensor_test, …) so every water-level
 * read updates the same trust file and CSV path.
 *
 * From a sketch in a subfolder:
 *   #include "../shared/sensor_data_store.h"
 * From the root sketch:
 *   #include "shared/sensor_data_store.h"
 *
 * CSV is written so a future SQLite import on the Pi can treat
 * empty cells as NULL, timestamps as ISO-8601 UTC, and flags as 0/1.
 * Keep this header in sync with the copies under water_drain_test/
 * and i2c_sensor_test/ if those sketches include the local file.
 *
 * TODO(sqlite): Pi dashboard should ingest this schema (or the MQTT
 * JSON equivalent) into data/sensor_readings.sqlite. Do not invent a
 * second column set when that lands.
 ************************************************************/

#ifndef SMART_GARDEN_SENSOR_DATA_STORE_H
#define SMART_GARDEN_SENSOR_DATA_STORE_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <math.h>
#include <time.h>

namespace SensorDataStore {

constexpr const char* WATER_LEVEL_STORE_PATH = "/last_water_level.txt";
// Grove % where water just touches the net pots (from water_fill_cal).
constexpr const char* NET_POT_TOUCH_PATH = "/net_pot_touch.txt";
constexpr const char* CSV_PATH = "/sensor_readings.csv";
// Firmware before 1.8.0 kept unsent MQTT rows here (rewritten on every
// replay). Removed at boot; see MQTT_OUTBOX_PATH.
constexpr const char* MQTT_UNSENT_PATH = "/mqtt_unsent.csv";
// Readings the live MQTT publish missed: same CSV line as CSV_PATH, append
// only, replayed from a persisted cursor (shared/durable_queue.h).
constexpr const char* MQTT_OUTBOX_PATH = "/mqtt_outbox.csv";
constexpr const char* MQTT_OUTBOX_CURSOR_PATH = "/mqtt_outbox.cur";
// Incremented once per boot. With the per-boot reading seq it identifies a
// reading across the CSV journal, MQTT (live and replay) and Influx, so a
// receiver can drop repeats. boot_count resets on power loss; this does not.
constexpr const char* BOOT_ID_PATH = "/boot_id.txt";
constexpr const char* CSV_DEVICE_ID_DEFAULT = "garden-node-01";

// SQLite-ready header: snake_case identifiers, no spaces, stable order.
// recorded_at_utc is ISO-8601 UTC (YYYY-MM-DDTHH:MM:SSZ) or empty (NULL).
// uptime_ms is millis() since boot (not a wall clock).
// Boolean pump columns are 0/1. Missing / failed sensors are empty cells.
constexpr const char* CSV_HEADER =
  "device_id,recorded_at_utc,uptime_ms,source,grow_stage,"
  "water_temp_c,light_adc,water_level_pct,water_level_raw_pct,water_target_pct,water_max_pct,"
  "tds_adc,tds_ppm,nutrient_tds_min,nutrient_tds_max,nutrient_dose,"
  "ph_voltage,ph,ph_filtered,ph_target_min,ph_target_max,ph_dose,"
  "co2_ppm,ambient_temp_c,ambient_humidity_pct,"
  "water_pump_on,drain_pump_on,nutrient_pump_on,ph_up_pump_on,ph_down_pump_on,"
  "grow_light_on,grow_light_brightness_pct,grow_light_animating,"
  "reset_reason,boot_count,brownout_count,rail_mv,free_heap_b,wifi_rssi_dbm,"
  "ph_ema,tds_ppm_ema,boot_id,seq";

constexpr int CSV_COLUMN_COUNT = 43;
// Longest formatted row is about 330 characters.
constexpr size_t CSV_LINE_MAX = 512;
constexpr float INVALID_WATER_TEMP_C = -127.0f;

// Grove pads often report 0% right after reboot. Ignore that glitch until a
// reading is near the last trusted value. Offline drains (other sketches) can
// drop the level farther than this band — those drops are accepted.
// TODO: Find a more reliable water level sensor + water pump combination.
constexpr int WATER_LEVEL_RETRUST_TOLERANCE_PERCENT = 10;

struct WaterLevelTrust {
  int lastTrustedPercent = -1;
  bool established = true;
};

struct CsvReading {
  const char* deviceId = CSV_DEVICE_ID_DEFAULT;
  const char* recordedAtUtc = "";
  unsigned long uptimeMs = 0;
  const char* source = "esp32";
  const char* growStage = "";
  float waterTempC = NAN;
  int lightAdc = -1;
  int waterLevelPct = -1;
  int waterLevelRawPct = -1;
  int waterTargetPct = -1;
  int waterMaxPct = -1;
  int tdsAdc = -1;
  int tdsPpm = -1;
  int nutrientTdsMin = -1;
  int nutrientTdsMax = -1;
  const char* nutrientDose = "none";
  float phVoltage = NAN;
  float ph = NAN;
  float phFiltered = NAN;
  float phEma = NAN;
  float tdsPpmEma = NAN;
  float phTargetMin = NAN;
  float phTargetMax = NAN;
  const char* phDose = "none";
  int co2Ppm = -1;
  float ambientTempC = NAN;
  float ambientHumidityPct = NAN;
  int waterPumpOn = -1;
  int drainPumpOn = -1;
  int nutrientPumpOn = -1;
  int phUpPumpOn = -1;
  int phDownPumpOn = -1;
  int growLightOn = -1;
  int growLightBrightnessPct = -1;
  int growLightAnimating = -1;
  const char* resetReason = "";
  int bootCount = -1;
  int brownoutCount = -1;
  int railMv = -1;
  int freeHeapB = -1;
  int wifiRssiDbm = 0;
  bool wifiRssiValid = false;
  uint32_t bootId = 0;  // 0 = not set, written as an empty cell
  uint32_t seq = 0;
};

// Print sink over a fixed char buffer (truncates, always NUL-terminated).
class BufferPrint : public Print {
 public:
  BufferPrint(char* buf, size_t len) : buf_(buf), len_(len) {
    if (len_ > 0) {
      buf_[0] = '\0';
    }
  }
  size_t write(uint8_t c) override {
    if (len_ == 0 || used_ + 1 >= len_) {
      overflow_ = true;
      return 0;
    }
    buf_[used_++] = static_cast<char>(c);
    buf_[used_] = '\0';
    return 1;
  }
  size_t length() const { return used_; }
  bool overflowed() const { return overflow_; }

 private:
  char* buf_;
  size_t len_;
  size_t used_ = 0;
  bool overflow_ = false;
};

inline bool begin(bool formatOnFail = true) {
  if (!LittleFS.begin(formatOnFail)) {
    Serial.println("[ERROR] LittleFS mount failed — sensor store disabled");
    return false;
  }
  return true;
}

// Read, increment and store the persistent boot id. Call once after mount.
inline uint32_t nextBootId() {
  uint32_t id = 0;
  File in = LittleFS.open(BOOT_ID_PATH, FILE_READ);
  if (in) {
    id = static_cast<uint32_t>(in.readStringUntil('\n').toInt());
    in.close();
  }
  id++;
  File out = LittleFS.open(BOOT_ID_PATH, FILE_WRITE);
  if (out) {
    out.println(id);
    out.close();
  }
  return id;
}

inline void formatIsoUtc(time_t epochSeconds, char* buf, size_t buflen) {
  if (buf == nullptr || buflen < 21) {
    return;
  }
  struct tm timeInfo;
  gmtime_r(&epochSeconds, &timeInfo);
  strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
}

// Convert a row's millis() into ISO-8601 UTC using the NTP/millis offset.
// Leaves buf empty when the wall clock is unknown (SQLite NULL).
inline bool recordedAtUtcFromNtp(
    bool ntpValid,
    uint32_t ntpEpochSecondsAtSync,
    unsigned long millisAtNtpSyncMs,
    unsigned long uptimeMs,
    char* buf,
    size_t buflen) {
  if (buf == nullptr || buflen == 0) {
    return false;
  }
  buf[0] = '\0';
  if (!ntpValid || buflen < 21) {
    return false;
  }

  // Wrap-safe across the 49.7-day millis() rollover (see InfluxCloud::epochMs).
  long long deltaMs = static_cast<long long>(static_cast<int32_t>(
      static_cast<uint32_t>(uptimeMs) - static_cast<uint32_t>(millisAtNtpSyncMs)));
  long long epochSeconds =
      static_cast<long long>(ntpEpochSecondsAtSync) + (deltaMs / 1000LL);
  if (epochSeconds <= 0) {
    return false;
  }

  formatIsoUtc(static_cast<time_t>(epochSeconds), buf, buflen);
  return buf[0] != '\0';
}

inline bool csvHeaderLooksCurrent(const String& headerLine) {
  return headerLine.indexOf("recorded_at_utc") >= 0 &&
         headerLine.indexOf("device_id") >= 0 &&
         headerLine.indexOf("light_adc") >= 0 &&
         headerLine.indexOf("water_pump_on") >= 0 &&
         headerLine.indexOf("grow_light_on") >= 0 &&
         headerLine.indexOf("water_level_raw_pct") >= 0 &&
         headerLine.indexOf("ph_filtered") >= 0 &&
         headerLine.indexOf("ph_ema") >= 0 &&
         headerLine.indexOf("tds_ppm_ema") >= 0 &&
         headerLine.indexOf(",ph,") >= 0 &&
         headerLine.indexOf("brownout_count") >= 0 &&
         headerLine.indexOf("free_heap_b") >= 0 &&
         headerLine.indexOf(",boot_id,seq") >= 0;
}

// The Grove stick reads 0% after every power-up until it is lifted out and
// lowered slowly back in. Every boot starts "init required": readings are
// ignored (-1) until a non-zero level re-establishes trust.
inline void loadWaterLevelTrust(WaterLevelTrust& trust) {
  trust.lastTrustedPercent = -1;
  trust.established = false;

  if (!LittleFS.exists(WATER_LEVEL_STORE_PATH)) {
    Serial.println("[ALERT] Water level INIT REQUIRED — lift the stick out, lower it "
                   "slowly into the water (no stored level yet)");
    return;
  }

  File file = LittleFS.open(WATER_LEVEL_STORE_PATH, FILE_READ);
  if (!file) {
    Serial.println("[WARN] Failed to read stored water level");
    return;
  }

  int stored = file.parseInt();
  file.close();

  if (stored <= 0 || stored > 100) {
    Serial.print("[WARN] Ignoring invalid stored water level - ");
    Serial.println(stored);
    return;
  }

  trust.lastTrustedPercent = stored;

  Serial.print("[ALERT] Water level INIT REQUIRED — lift the stick out, lower it slowly "
               "into the water. Last saved ");
  Serial.print(trust.lastTrustedPercent);
  Serial.print("%; ignoring readings until within +/- ");
  Serial.print(WATER_LEVEL_RETRUST_TOLERANCE_PERCENT);
  Serial.println("% (or a lower non-zero level from an offline change)");
}

inline bool saveTrustedWaterLevel(int levelPercent) {
  if (levelPercent <= 0 || levelPercent > 100) {
    return false;
  }

  File file = LittleFS.open(WATER_LEVEL_STORE_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("[WARN] Failed to store trusted water level");
    return false;
  }

  file.println(levelPercent);
  file.close();
  return true;
}

// Persist a measured level from any sketch (main or diagnostic) so the next
// boot of smart-garden trusts the real tank state.
inline bool recordWaterLevel(int levelPercent) {
  if (levelPercent < 0 || levelPercent > 100) {
    return false;
  }
  // Still skip storing pure 0% — that is the known post-boot glitch value.
  if (levelPercent == 0) {
    return false;
  }
  if (!saveTrustedWaterLevel(levelPercent)) {
    return false;
  }
  Serial.print("[STORE] Water level saved to ");
  Serial.print(WATER_LEVEL_STORE_PATH);
  Serial.print(": ");
  Serial.print(levelPercent);
  Serial.println("%");
  return true;
}

inline bool saveNetPotTouchPercent(int touchPercent) {
  if (touchPercent < 5 || touchPercent > 100) {
    Serial.print("[WARN] Net-pot touch % out of range: ");
    Serial.println(touchPercent);
    return false;
  }

  File file = LittleFS.open(NET_POT_TOUCH_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("[WARN] Failed to store net-pot touch %");
    return false;
  }

  file.println(touchPercent);
  file.close();
  Serial.print("[STORE] Net-pot touch saved to ");
  Serial.print(NET_POT_TOUCH_PATH);
  Serial.print(": ");
  Serial.print(touchPercent);
  Serial.println("%");
  return true;
}

// Returns false when no file / invalid; leaves `touchPercent` unchanged.
inline bool loadNetPotTouchPercent(int& touchPercent) {
  if (!LittleFS.exists(NET_POT_TOUCH_PATH)) {
    Serial.println("[DEBUG] No net-pot touch file yet — use water_fill_cal TOUCH/DONE");
    return false;
  }

  File file = LittleFS.open(NET_POT_TOUCH_PATH, FILE_READ);
  if (!file) {
    Serial.println("[WARN] Failed to read net-pot touch %");
    return false;
  }

  int stored = file.parseInt();
  file.close();

  if (stored < 5 || stored > 100) {
    Serial.print("[WARN] Ignoring invalid net-pot touch % - ");
    Serial.println(stored);
    return false;
  }

  touchPercent = stored;
  Serial.print("[DEBUG] Loaded net-pot touch ");
  Serial.print(touchPercent);
  Serial.println("% (5 mm ≈ 5 % on Grove)");
  return true;
}

inline void establishTrust(WaterLevelTrust& trust, int levelPercent) {
  trust.established = true;
  trust.lastTrustedPercent = levelPercent;
  saveTrustedWaterLevel(levelPercent);
  Serial.print("[DEBUG] Water level trust restored at ");
  Serial.print(levelPercent);
  Serial.println("%");
}

// Reject the known post-reboot 0% glitch until the sensor returns a usable
// non-zero level. Accept:
//   - values within ±WATER_LEVEL_RETRUST_TOLERANCE_PERCENT of the store
//   - any lower non-zero level (offline drain / other sketch)
//   - a stable higher non-zero level seen twice in a row (offline refill)
inline int filterWaterLevelReading(WaterLevelTrust& trust, int rawPercent) {
  if (rawPercent < 0) {
    return -1;
  }

  static int pendingOverridePercent = -1;
  static uint8_t pendingOverrideCount = 0;

  if (!trust.established) {
    if (rawPercent == 0) {
      pendingOverridePercent = -1;
      pendingOverrideCount = 0;
      Serial.print("[WARN] Water level INIT REQUIRED — ignoring 0% (last saved ");
      Serial.print(trust.lastTrustedPercent);
      Serial.println("%); lift the stick out and lower it slowly into the water");
      return -1;
    }

    if (trust.lastTrustedPercent < 0) {
      pendingOverridePercent = -1;
      pendingOverrideCount = 0;
      establishTrust(trust, rawPercent);
      return rawPercent;
    }

    int delta = rawPercent - trust.lastTrustedPercent;
    if (delta < 0) {
      delta = -delta;
    }

    if (delta <= WATER_LEVEL_RETRUST_TOLERANCE_PERCENT) {
      pendingOverridePercent = -1;
      pendingOverrideCount = 0;
      establishTrust(trust, rawPercent);
      return rawPercent;
    }

    // Tank was drained while another sketch ran (or since last save).
    if (rawPercent < trust.lastTrustedPercent) {
      pendingOverridePercent = -1;
      pendingOverrideCount = 0;
      Serial.print("[WARN] Water level ");
      Serial.print(rawPercent);
      Serial.print("% is below last trusted ");
      Serial.print(trust.lastTrustedPercent);
      Serial.println("%; accepting offline drop and updating store");
      establishTrust(trust, rawPercent);
      return rawPercent;
    }

    // Higher than stored by more than the band — require two matching samples
    // so a single glitch cannot jump the ceiling.
    if (rawPercent == pendingOverridePercent) {
      pendingOverrideCount++;
    } else {
      pendingOverridePercent = rawPercent;
      pendingOverrideCount = 1;
    }

    if (pendingOverrideCount >= 2) {
      pendingOverridePercent = -1;
      pendingOverrideCount = 0;
      Serial.print("[WARN] Water level ");
      Serial.print(rawPercent);
      Serial.print("% is above last trusted ");
      Serial.print(trust.lastTrustedPercent);
      Serial.println("%; accepting stable override and updating store");
      establishTrust(trust, rawPercent);
      return rawPercent;
    }

    Serial.print("[WARN] Water level ");
    Serial.print(rawPercent);
    Serial.print("% is outside +/- ");
    Serial.print(WATER_LEVEL_RETRUST_TOLERANCE_PERCENT);
    Serial.print("% of last trusted ");
    Serial.print(trust.lastTrustedPercent);
    Serial.println("%; still untrusted after reboot (need another matching sample to override up)");
    return -1;
  }

  if (rawPercent > 0 && rawPercent != trust.lastTrustedPercent) {
    trust.lastTrustedPercent = rawPercent;
    saveTrustedWaterLevel(rawPercent);
  }

  return rawPercent;
}

inline bool isValidWaterTempC(float value) {
  return !isnan(value) && value > INVALID_WATER_TEMP_C + 0.01f;
}

// One CSV row in CSV_HEADER order, without the line ending.
inline void formatCsvReading(const CsvReading& row, Print& file) {
  auto printOptionalInt = [&file](int value) {
    if (value >= 0) {
      file.print(value);
    }
  };
  auto printOptionalFloat = [&file](float value, int digits) {
    if (!isnan(value)) {
      file.print(value, digits);
    }
  };
  auto printOptionalTempC = [&file](float value, int digits) {
    if (isValidWaterTempC(value)) {
      file.print(value, digits);
    }
  };
  auto printFlag = [&file](int value) {
    if (value == 0 || value == 1) {
      file.print(value);
    }
  };

  file.print(row.deviceId != nullptr ? row.deviceId : CSV_DEVICE_ID_DEFAULT);
  file.print(",");
  if (row.recordedAtUtc != nullptr) {
    file.print(row.recordedAtUtc);
  }
  file.print(",");
  file.print(row.uptimeMs);
  file.print(",");
  file.print(row.source != nullptr ? row.source : "esp32");
  file.print(",");
  file.print(row.growStage != nullptr ? row.growStage : "");
  file.print(",");
  printOptionalTempC(row.waterTempC, 2);
  file.print(",");
  printOptionalInt(row.lightAdc);
  file.print(",");
  printOptionalInt(row.waterLevelPct);
  file.print(",");
  printOptionalInt(row.waterLevelRawPct);
  file.print(",");
  printOptionalInt(row.waterTargetPct);
  file.print(",");
  printOptionalInt(row.waterMaxPct);
  file.print(",");
  printOptionalInt(row.tdsAdc);
  file.print(",");
  printOptionalInt(row.tdsPpm);
  file.print(",");
  printOptionalInt(row.nutrientTdsMin);
  file.print(",");
  printOptionalInt(row.nutrientTdsMax);
  file.print(",");
  file.print(row.nutrientDose != nullptr ? row.nutrientDose : "none");
  file.print(",");
  printOptionalFloat(row.phVoltage, 3);
  file.print(",");
  printOptionalFloat(row.ph, 2);
  file.print(",");
  printOptionalFloat(row.phFiltered, 2);
  file.print(",");
  printOptionalFloat(row.phTargetMin, 1);
  file.print(",");
  printOptionalFloat(row.phTargetMax, 1);
  file.print(",");
  file.print(row.phDose != nullptr ? row.phDose : "none");
  file.print(",");
  printOptionalInt(row.co2Ppm);
  file.print(",");
  printOptionalTempC(row.ambientTempC, 1);
  file.print(",");
  if (!isnan(row.ambientHumidityPct) && row.ambientHumidityPct >= 0.0f) {
    file.print(row.ambientHumidityPct, 1);
  }
  file.print(",");
  printFlag(row.waterPumpOn);
  file.print(",");
  printFlag(row.drainPumpOn);
  file.print(",");
  printFlag(row.nutrientPumpOn);
  file.print(",");
  printFlag(row.phUpPumpOn);
  file.print(",");
  printFlag(row.phDownPumpOn);
  file.print(",");
  printFlag(row.growLightOn);
  file.print(",");
  printOptionalInt(row.growLightBrightnessPct);
  file.print(",");
  printFlag(row.growLightAnimating);
  file.print(",");
  file.print(row.resetReason != nullptr ? row.resetReason : "");
  file.print(",");
  printOptionalInt(row.bootCount);
  file.print(",");
  printOptionalInt(row.brownoutCount);
  file.print(",");
  printOptionalInt(row.railMv);
  file.print(",");
  printOptionalInt(row.freeHeapB);
  file.print(",");
  if (row.wifiRssiValid) {
    file.print(row.wifiRssiDbm);
  }
  file.print(",");
  printOptionalFloat(row.phEma, 2);
  file.print(",");
  printOptionalFloat(row.tdsPpmEma, 1);
  file.print(",");
  if (row.bootId > 0) {
    file.print(row.bootId);
  }
  file.print(",");
  if (row.seq > 0) {
    file.print(row.seq);
  }
}

// Append one row to path (header first on an empty file). When lineOut is
// given it receives the same line, so other outlets carry identical bytes.
inline bool appendCsvReading(const CsvReading& row, const char* path = CSV_PATH,
                             char* lineOut = nullptr, size_t lineOutLen = 0) {
  char line[CSV_LINE_MAX];
  BufferPrint bp(line, sizeof(line));
  formatCsvReading(row, bp);
  if (lineOut != nullptr && lineOutLen > 0) {
    strncpy(lineOut, line, lineOutLen - 1);
    lineOut[lineOutLen - 1] = '\0';
  }

  File file = LittleFS.open(path, FILE_APPEND);
  if (!file) {
    Serial.println("[ERROR] Failed to open CSV file for writing");
    return false;
  }
  if (file.size() == 0) {
    file.println(CSV_HEADER);
    Serial.println("[DEBUG] CSV header written");
  }
  file.println(line);
  file.close();

  Serial.print("[STORE] Sensor row appended to ");
  Serial.print(path);
  Serial.print(" (source=");
  Serial.print(row.source != nullptr ? row.source : "esp32");
  if (row.waterLevelPct >= 0) {
    Serial.print(", water_level_pct=");
    Serial.print(row.waterLevelPct);
  }
  Serial.println(")");
  return true;
}

inline bool appendCsvRow(
    const char* sourceLabel,
    int waterLevelPct,
    int waterTargetPct = -1,
    float waterTempC = NAN,
    int ldrAdc = -1,
    int tdsAdc = -1,
    int tdsPpm = -1,
    float phVoltage = NAN,
    float phValue = NAN,
    int co2Ppm = -1,
    float ambientTempC = NAN,
    float ambientHumidityPct = NAN) {
  CsvReading row;
  row.source = sourceLabel != nullptr ? sourceLabel : "unknown";
  row.growStage = "";
  row.uptimeMs = millis();
  row.waterLevelPct = waterLevelPct;
  row.waterTargetPct = waterTargetPct;
  row.waterTempC = waterTempC;
  row.lightAdc = ldrAdc;
  row.tdsAdc = tdsAdc;
  row.tdsPpm = tdsPpm;
  row.phVoltage = phVoltage;
  row.ph = phValue;
  row.co2Ppm = co2Ppm;
  row.ambientTempC = ambientTempC;
  row.ambientHumidityPct = ambientHumidityPct;
  return appendCsvReading(row);
}

// Convenience for water-level-only diagnostic sketches.
inline bool logWaterLevelReading(
    int levelPercent,
    const char* sourceLabel,
    int waterTargetPct = -1) {
  if (levelPercent < 0) {
    return false;
  }

  bool ok = recordWaterLevel(levelPercent);
  ok = appendCsvRow(sourceLabel, levelPercent, waterTargetPct) && ok;
  return ok;
}

}  // namespace SensorDataStore

#endif  // SMART_GARDEN_SENSOR_DATA_STORE_H
