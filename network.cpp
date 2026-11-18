/* WiFi, NTP, and InfluxDB upload. */
#include "garden.h"
#include "shared/sensor_ema.h"

/************************************************************
 * WiFi / NTP / cloud upload
 ************************************************************/

#if ENABLE_WIFI

void syncNtpTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Runs on the network task. Only waits for the first answer here; the
  // task latches the epoch (and re-latches it) in Comms::latchNtp(). The
  // loop passes ntpEpochValid to GrowLight on every pass.
  struct tm timeInfo;
  for (int i = 0; i < 20; i++) {
    if (getLocalTime(&timeInfo, 500)) {
      Serial.println("[DEBUG] NTP time synchronized");
      return;
    }
  }
  // SNTP keeps polling in the background; the network task latches the time
  // when it arrives instead of waiting for the next WiFi reconnect.
  Serial.println("[WARN] NTP not answered yet; timestamps wait (journal rows have uptime_ms)");
}

// Profile of the network joined last (network task); picks the MQTT broker.
static const WifiNetworkProfile* joinedWifiProfile = nullptr;

const char* mqttBrokerHost() {
  const WifiNetworkProfile* net = joinedWifiProfile;
  if (net != nullptr && net->mqttBrokerHost != nullptr && net->mqttBrokerHost[0] != '\0') {
    return net->mqttBrokerHost;
  }
  return MQTT_BROKER_HOST;
}

bool wifiProfileConfigured(const WifiNetworkProfile& net) {
  if (net.ssid == nullptr || net.ssid[0] == '\0') {
    return false;
  }
  if (net.password == nullptr || net.password[0] == '\0') {
    return false;
  }
  if (net.auth == 1) {
    // UniWireless PEAP needs an inner username (usually user@student.unimelb.edu.au).
    if (net.eapUsername == nullptr || net.eapUsername[0] == '\0') {
      return false;
    }
  }
  return true;
}

bool waitForWifiConnected(unsigned long timeoutMs) {
  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

bool beginWifiProfile(const WifiNetworkProfile& net) {
  WiFi.disconnect(true /*wifioff*/, true /*eraseAP*/);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  Serial.print("[DEBUG] Connecting to WiFi SSID - ");
  Serial.print(net.ssid);
  if (net.auth == 1) {
    Serial.print(" (WPA2-Enterprise PEAP, user ");
    Serial.print(net.eapUsername);
    Serial.print(")");
  }
  Serial.println();

  if (net.auth == 0) {
    WiFi.begin(net.ssid, net.password);
    return waitForWifiConnected(WIFI_CONNECT_TIMEOUT_MS);
  }

  // UniMelb UniWireless: PEAP + MSCHAPv2, real user in phase 2.
  // ca_pem omitted → do not validate campus CA (matches typical phone "Don't validate").
  // The core skips an empty identity (esp_eap_client_set_identity rejects
  // length 0), so the EAP-Identity reply would be blank. With no anonymous
  // identity, send the username, as the Pi's NetworkManager and phones do.
  const char* outerIdentity =
      (net.eapIdentity != nullptr && net.eapIdentity[0] != '\0') ? net.eapIdentity
                                                                 : net.eapUsername;
  WiFi.begin(
      net.ssid,
      WPA2_AUTH_PEAP,
      outerIdentity,
      net.eapUsername,
      net.password);
  return waitForWifiConnected(WIFI_ENTERPRISE_CONNECT_TIMEOUT_MS);
}

bool connectWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    return true;
  }

  // Rank configured networks by current RSSI when visible; fall back to
  // secrets order if scan finds none (or scan fails).
  int order[8];
  int orderCount = 0;
  const size_t maxProfiles =
      WIFI_NETWORK_COUNT < 8 ? WIFI_NETWORK_COUNT : 8;

  WiFi.mode(WIFI_STA);
  const int scanned = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  if (scanned > 0) {
    int rssiForProfile[8];
    for (size_t i = 0; i < maxProfiles; i++) {
      rssiForProfile[i] = -9999;
      if (!wifiProfileConfigured(WIFI_NETWORKS[i])) {
        continue;
      }
      for (int s = 0; s < scanned; s++) {
        if (WiFi.SSID(s) == WIFI_NETWORKS[i].ssid) {
          const int rssi = WiFi.RSSI(s);
          if (rssi > rssiForProfile[i]) {
            rssiForProfile[i] = rssi;
          }
        }
      }
      if (rssiForProfile[i] > -9999) {
        order[orderCount++] = static_cast<int>(i);
      }
    }
    // Insertion sort strongest RSSI first.
    for (int i = 1; i < orderCount; i++) {
      const int key = order[i];
      int j = i - 1;
      while (j >= 0 && rssiForProfile[order[j]] < rssiForProfile[key]) {
        order[j + 1] = order[j];
        j--;
      }
      order[j + 1] = key;
    }
  }
  WiFi.scanDelete();

  if (orderCount == 0) {
    for (size_t i = 0; i < maxProfiles; i++) {
      if (wifiProfileConfigured(WIFI_NETWORKS[i])) {
        order[orderCount++] = static_cast<int>(i);
      }
    }
  }

  if (orderCount == 0) {
    Serial.println("[ERROR] No WiFi profiles configured in secrets.h");
    return false;
  }

  for (int i = 0; i < orderCount; i++) {
    const WifiNetworkProfile& net = WIFI_NETWORKS[order[i]];
    if (beginWifiProfile(net)) {
      // Modem sleep drops TLS handshakes to Influx and the failure
      // is reported as HTTP -1 "connection refused".
      WiFi.setSleep(false);
      joinedWifiProfile = &net;
      Serial.print("[DEBUG] WiFi connected - SSID ");
      Serial.print(net.ssid);
      Serial.print(" IP ");
      Serial.print(WiFi.localIP());
      Serial.print(" MQTT broker ");
      Serial.println(mqttBrokerHost());

      if (!MDNS.begin(INFLUX_DEVICE_ID)) {
        Serial.println("[WARN] mDNS begin failed");
      }

      syncNtpTime();
      return true;
    }
    Serial.print("[WARN] WiFi connect failed for ");
    Serial.println(net.ssid);
  }

  Serial.println("[ERROR] WiFi connection failed for all configured networks");
  return false;
}

String currentIsoTimestamp() {
  if (ntpTimeSynced) {
    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 50)) {
      char buffer[32];
      strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
      return String(buffer);
    }
  }
  return String("millis:") + String(millis());
}

#endif  // ENABLE_WIFI

void publishControlEvent(const ControlEventLog::Row& row) {
  controlEventSeq++;
#if ENABLE_INFLUXDB
  influxUploadControlEventHook(row);
#endif
#if ENABLE_MQTT
  publishControlEventToMqtt(row);
#endif
}

#if ENABLE_INFLUXDB

bool uploadTelemetryToInflux() {
  if (!InfluxCloud::configured()) {
    static unsigned long lastHintMs = 0;
    const unsigned long now = millis();
    if (lastHintMs == 0 || now - lastHintMs > 30000) {
      lastHintMs = now;
      Serial.println("[INFLUX] skip — not configured (type INFLUX on Serial)");
    }
    return false;
  }
  if (!ntpEpochValid) {
    // Without wall-clock time the point would be stamped with uptime (1970).
    // The reading is still in the journal and on MQTT.
    static unsigned long lastNoNtpMs = 0;
    if (lastNoNtpMs == 0 || millis() - lastNoNtpMs > 60000) {
      lastNoNtpMs = millis() | 1UL;
      Serial.println("[INFLUX] sensor_reading skipped — no NTP time yet");
    }
    return false;
  }

  const GrowPreset& preset = activePreset();
  char recordedAtUtc[24] = "";
#if ENABLE_WIFI
  recordedAtUtcForUptime(
      millis(),
      recordedAtUtc,
      sizeof(recordedAtUtc));
#endif

  InfluxCloud::SensorPoint p;
  p.stage = systemModeName();
  p.recordedAtUtc = recordedAtUtc;
  p.uptimeMs = millis();
  p.waterTempC = waterTempC;
  p.ambientTempC = ambientTempC;
  p.ambientHumidityPct = ambientHumidityPct;
  p.lightAdc = ldrADC;
  p.waterLevelPct = waterLevelPercent;
  p.waterLevelRawPct = waterLevelRawPercent;
  p.waterTargetPct = waterStageTargetPercent();
  p.fill0Pct = fill0Percent();
  p.currentDrawdownMm =
      LevelControl::drawdownMm(fill0Percent(), waterLevelPercent);
  p.targetDrawdownMm = GrowStage::config(systemMode).targetDrawdownMm;
  p.tdsPpm = tdsPPM;
  p.tdsPpmEma = tdsPpmEma;
  {
    const TdsNutrientObserver::State& obs = TdsNutrientObserver::state();
    const float V = TdsNutrientObserver::volumeLFromLevelPct(waterLevelPercent);
    p.tdsPpmNutrient = TdsNutrientObserver::nutrientPpm(static_cast<float>(tdsPPM), V);
    p.tdsPhSaltPpm = TdsNutrientObserver::saltOffsetPpm(V);
    p.tdsKappaUp = obs.kappaUp;
    p.tdsKappaDown = obs.kappaDown;
    p.tdsPhUpMl = obs.cumPhUpMl;
    p.tdsPhDownMl = obs.cumPhDownMl;
    p.tdsSaltCorrectionLive = obs.liveCorrection ? 1 : 0;
    p.tdsKappaProvisional = obs.kappaProvisional ? 1 : 0;
  }
  p.phRaw = phValue;
  p.phFiltered = phValueFiltered;
  p.phEma = phValueEma;
  p.ph = SensorEma::forControl(phValueEma, phValueFiltered);
  p.phVoltage = phVoltage;
  if (phControlAllowedByMode()) {
    p.phTargetMin = preset.phMin;
    p.phTargetMax = preset.phMax;
  }
  p.phDoseState = phDoseLabel();
  p.phSessionMl = StageController::ctx().ph.cumulativeSessionMl;
  p.ecSessionMl = StageController::ctx().ec.cumulativeSessionMl;
  p.gainPhUp = StageController::ctx().ph.gainUp;
  p.gainPhDown = StageController::ctx().ph.gainDown;
  p.gainEc = StageController::ctx().ec.gainEc;
  p.readyToSeed = StageController::ctx().readyToSeed;
  p.baselineCapture = isSensorBaselineMode();
  p.waterLevelSensorFault = isWaterLevelSensorFault();
  p.co2Ppm = co2PPM;
  p.waterPumpOn = waterRefillPumpRunning ? 1 : 0;
  p.nutrientPumpOn = nutrientControlState == NUTRIENT_DOSING ? 1 : 0;
  p.phUpPumpOn = phControlState == PH_DOSING_UP ? 1 : 0;
  p.phDownPumpOn = phControlState == PH_DOSING_DOWN ? 1 : 0;
  #if ENABLE_GROW_LIGHT
  {
    const GrowLight::Status gl = GrowLight::status();
    p.growLightOn = gl.on ? 1 : 0;
    p.growLightBrightnessPct = gl.brightnessPct;
    p.growLightAnimating = gl.animating ? 1 : 0;
    p.growLightMode = gl.mode;
    p.growLightPhotoperiodH = gl.photoperiodOnHours;
  }
  #endif
  p.resetReason = PowerMonitor::lastResetReasonName();
  p.bootCount = static_cast<int>(PowerMonitor::bootCountValue());
  p.brownoutCount = static_cast<int>(PowerMonitor::brownoutCountValue());
  p.lastResetBrownout = PowerMonitor::lastResetWasBrownout() ? 1 : 0;
  p.railMv = PowerMonitor::readRailMv();
  p.freeHeapB = PowerMonitor::freeHeapBytes();
  p.wifiRssiValid = PowerMonitor::wifiRssiValid();
  p.wifiRssiDbm = PowerMonitor::wifiRssiDbm();

  p.bootId = bootId;
  p.seq = readingSeq;

  const NtpMap ntp = ntpSnapshot();
  return InfluxCloud::uploadSensor(p, ntp.valid, ntp.epochAtSync, ntp.millisAtSync);
}

void influxUploadControlEventHook(const ControlEventLog::Row& row) {
  if (!InfluxCloud::configured()) {
    return;
  }
  if (!ntpEpochValid) {
    // Influx needs a wall-clock timestamp; the row stays in /control_events.csv.
    Serial.println("[INFLUX] control_event skipped — no NTP time yet");
    return;
  }
  const NtpMap ntp = ntpSnapshot();
  InfluxCloud::uploadControlEvent(row, ntp.valid, ntp.epochAtSync, ntp.millisAtSync);
}

#endif  // ENABLE_INFLUXDB
