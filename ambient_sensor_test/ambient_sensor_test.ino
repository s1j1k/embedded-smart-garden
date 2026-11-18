/************************************************************
 * Ambient daughterboard test — DHT11, LDR, PAS CO2 → InfluxDB Cloud
 *
 * Isolates the ambient harness (GPIO 16 / 34 / I2C 21–22) from tank
 * chemistry and pumps. Use this after wiring the JST-XH 7-way cable
 * (prototype order: GND, 3.3 V, 5 V, SDA, SCL, DHT, LDR).
 *
 * Setup:
 *   1. Copy secrets.h.example → secrets.h (repo root) with WiFi + Influx.
 *   2. Install libraries: DHT sensor library, PAS CO2 (Infineon).
 *   3. Open this sketch in Arduino IDE, flash at 115200 (Newline).
 *
 * Serial commands: HELP, READ, INFLUX (connectivity test point)
 *
 * Influx measurement: ambient_reading
 *   Fields: ambient_temp_c, ambient_humidity_pct, light_adc, co2_ppm,
 *           dht_ok, ldr_ok, co2_ok
 ************************************************************/

#include <WiFi.h>
#include <time.h>
#include <DHT.h>
#include <pas-co2-ino.hpp>

#include "../secrets.h"
#include "../shared/ambient_sensors.h"
#include "../shared/influx_cloud.h"

#define DHTTYPE DHT11

const unsigned long READ_INTERVAL_MS = 30000;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;

DHT dht(AmbientSensors::DHT11_PIN, DHTTYPE);
PASCO2Ino co2Sensor(&Wire);

bool ntpValid = false;
uint32_t ntpEpochAtSync = 0;
unsigned long millisAtNtpSync = 0;

String serialLine;

void syncNtp() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeInfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeInfo, 500)) {
      ntpValid = true;
      ntpEpochAtSync = static_cast<uint32_t>(time(nullptr));
      millisAtNtpSync = millis();
      Serial.println("[WIFI] NTP synchronized");
      return;
    }
  }
  ntpValid = false;
  Serial.println("[WIFI] NTP failed — Influx timestamps use uptime fallback");
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  Serial.print("[WIFI] Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Connection failed");
    return false;
  }
  Serial.print("[WIFI] IP ");
  Serial.println(WiFi.localIP());
  syncNtp();
  return true;
}

void recordedAtUtc(char* buf, size_t len) {
  if (buf == nullptr || len == 0) {
    return;
  }
  buf[0] = '\0';
  if (ntpValid) {
    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 50)) {
      strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
      return;
    }
  }
  snprintf(buf, len, "uptime:%lu", millis());
}

bool uploadReading(const AmbientSensors::Reading& r) {
  if (!connectWifi()) {
    return false;
  }
  if (!InfluxCloud::configured()) {
    Serial.println("[INFLUX] Not configured — edit secrets.h");
    return false;
  }

  char ts[24];
  recordedAtUtc(ts, sizeof(ts));

  InfluxCloud::AmbientPoint p;
  p.recordedAtUtc = ts;
  p.uptimeMs = millis();
  p.ambientTempC = r.dhtOk ? r.ambientTempC : NAN;
  p.ambientHumidityPct = r.dhtOk ? r.ambientHumidityPct : NAN;
  p.lightAdc = r.ldrOk ? r.lightAdc : -1;
  p.co2Ppm = r.co2Ok ? r.co2Ppm : -1;
  p.dhtOk = r.dhtOk;
  p.ldrOk = r.ldrOk;
  p.co2Ok = r.co2Ok;

  return InfluxCloud::uploadAmbientReading(
      p, ntpValid, ntpEpochAtSync, millisAtNtpSync);
}

void printReading(const AmbientSensors::Reading& r) {
  Serial.println();
  Serial.println("--- Ambient reading ---");
  if (r.dhtOk) {
    Serial.print("  Temp      ");
    Serial.print(r.ambientTempC, 1);
    Serial.println(" °C");
    Serial.print("  Humidity  ");
    Serial.print(r.ambientHumidityPct, 1);
    Serial.println(" %RH");
  } else {
    Serial.println("  DHT11     FAILED");
  }
  Serial.print("  LDR ADC   ");
  Serial.println(r.ldrOk ? String(r.lightAdc) : String("FAILED"));
  Serial.print("  CO2       ");
  if (r.co2Ok) {
    Serial.print(r.co2Ppm);
    Serial.println(" ppm");
  } else {
    Serial.println("FAILED");
  }
}

void runCycle(bool forceUpload) {
  const AmbientSensors::Reading r = AmbientSensors::readAll(dht, co2Sensor);
  printReading(r);

  const bool anyOk = r.dhtOk || r.ldrOk || r.co2Ok;
  if (!anyOk) {
    Serial.println("[TEST] No sensors returned valid data");
    return;
  }
  if (forceUpload || anyOk) {
    uploadReading(r);
  }
}

void printHelp() {
  Serial.println();
  Serial.println("Ambient sensor test (DHT11 + LDR + PAS CO2 → Influx)");
  Serial.println("  HELP   — this message");
  Serial.println("  READ   — read sensors now and upload to Influx");
  Serial.println("  INFLUX — post connectivity_test point only");
  Serial.println();
  InfluxCloud::printSetupHint();
}

void handleLine(const String& line) {
  String cmd = line;
  cmd.trim();
  cmd.toUpperCase();
  if (cmd.length() == 0) {
    return;
  }
  if (cmd == "HELP" || cmd == "?") {
    printHelp();
    return;
  }
  if (cmd == "READ") {
    runCycle(true);
    return;
  }
  if (cmd == "INFLUX") {
    if (connectWifi() && InfluxCloud::configured()) {
      InfluxCloud::uploadTestPoint(ntpValid, ntpEpochAtSync, millisAtNtpSync);
    }
    return;
  }
  Serial.print("[TEST] Unknown command: ");
  Serial.println(cmd);
}

void serviceSerial() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      handleLine(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 80) {
      serialLine += c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("[TEST] Ambient sensor module test starting");

  analogReadResolution(12);

  AmbientSensors::begin(Wire, dht, co2Sensor);
  InfluxCloud::printSetupHint();
  connectWifi();
  printHelp();

  Serial.println("[TEST] First automatic read in 3 s, then every 30 s");
  delay(3000);
  runCycle(true);
}

unsigned long lastReadMs = 0;

void loop() {
  serviceSerial();

  const unsigned long now = millis();
  if (now - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = now;
    runCycle(true);
  }
}
