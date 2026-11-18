/* MQTT payloads: telemetry, alerts, events, commands, outbox replay rows.
 * Building runs on the loop task; sockets are only touched by the network
 * task (comms.cpp), except connectMqtt() which that task calls. */
#include "garden.h"
#include "shared/sensor_ema.h"

/************************************************************
 * MQTT Telemetry (Raspberry Pi dashboard)
 ************************************************************/

#if ENABLE_MQTT

bool publishAlertsToMqtt(const char* calAlert,
                         const char* stockAlert,
                         bool calNeeded,
                         bool waterLevelFault);

const char* mqttStateLabel(int state) {
  switch (state) {
    case -4: return "connection timeout";
    case -3: return "connection lost";
    case -2: return "connect failed (host unreachable / DNS / broker down)";
    case -1: return "disconnected";
    case 0: return "connected";
    case 1: return "bad protocol";
    case 2: return "bad client id";
    case 3: return "broker unavailable";
    case 4: return "bad credentials";
    case 5: return "unauthorized";
    default: return "unknown";
  }
}

bool looksLikeIpLiteral(const char* host) {
  if (host == nullptr || host[0] == '\0') {
    return false;
  }
  // IPv4 literals only contain digits and dots.
  for (const char* p = host; *p; ++p) {
    if (!(isdigit(static_cast<unsigned char>(*p)) || *p == '.')) {
      return false;
    }
  }
  return true;
}

String mdnsServiceName(const char* host) {
  String name(host);
  name.trim();
  name.toLowerCase();
  if (name.endsWith(".local")) {
    name.remove(name.length() - 6);
  }
  return name;
}

bool resolveMqttBroker(const char* brokerHost, IPAddress& brokerIp) {
  // Numeric IP: parse directly.
  if (looksLikeIpLiteral(brokerHost) && brokerIp.fromString(brokerHost)) {
    return true;
  }

  Serial.print("[DEBUG] Resolving MQTT broker hostname - ");
  Serial.println(brokerHost);

  // *.local (e.g. smart-garden.local) must use mDNS, not unicast DNS.
  String host = String(brokerHost);
  host.trim();
  host.toLowerCase();
  if (host.endsWith(".local") || host == "smart-garden") {
    String service = mdnsServiceName(brokerHost);
    Serial.print("[DEBUG] mDNS queryHost - ");
    Serial.println(service);

    // Retry a few times — mDNS replies can be delayed after WiFi joins.
    for (int attempt = 1; attempt <= 3; attempt++) {
      brokerIp = MDNS.queryHost(service.c_str());
      if (brokerIp != IPAddress(0, 0, 0, 0)) {
        Serial.print("[DEBUG] mDNS resolved ");
        Serial.print(brokerHost);
        Serial.print(" -> ");
        Serial.println(brokerIp);
        return true;
      }
      Serial.print("[DEBUG] mDNS attempt ");
      Serial.print(attempt);
      Serial.println(" failed; retrying...");
      delay(500);
    }

    Serial.println("[WARN] mDNS lookup failed; trying WiFi.hostByName as fallback");
  }

  if (WiFi.hostByName(brokerHost, brokerIp) == 1 &&
      brokerIp != IPAddress(0, 0, 0, 0)) {
    Serial.print("[DEBUG] DNS resolved ");
    Serial.print(brokerHost);
    Serial.print(" -> ");
    Serial.println(brokerIp);
    return true;
  }

  Serial.print("[ERROR] Could not resolve MQTT broker '");
  Serial.print(brokerHost);
  Serial.println("'");
  Serial.println("[HINT] Confirm the Pi hostname is smar-gardon (avahi/mDNS).");
  Serial.println("       On the Pi: hostnamectl / ping smar-gardon.local from another device.");
  Serial.println("       Fallback: set mqttBrokerHost in secrets.h to the Pi IP from hostname -I.");
  return false;
}

// One resolve + CONNECT against one broker host.
bool connectMqttTo(const char* brokerHost) {
  IPAddress brokerIp;
  if (!resolveMqttBroker(brokerHost, brokerIp)) {
    return false;
  }

  Serial.print("[DEBUG] Connecting to MQTT broker ");
  Serial.print(brokerHost);
  Serial.print(" -> ");
  Serial.print(brokerIp);
  Serial.print(":");
  Serial.println(MQTT_BROKER_PORT);

  mqttClient.setServer(brokerIp, MQTT_BROKER_PORT);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);

  bool ok;
  if (MQTT_USERNAME != nullptr && MQTT_USERNAME[0] != '\0') {
    ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
  } else {
    ok = mqttClient.connect(MQTT_CLIENT_ID);
  }

  if (!ok) {
    int state = mqttClient.state();
    Serial.print("[ERROR] MQTT connect failed - state ");
    Serial.print(state);
    Serial.print(" (");
    Serial.print(mqttStateLabel(state));
    Serial.println(")");
    Serial.println("[HINT] On the Raspberry Pi confirm Mosquitto listens on 0.0.0.0:1883");
    Serial.println("       On Pi: ./scripts/setup-pi-mqtt-broker.sh  (from the dashboard repo)");
    Serial.println("       Or: sudo systemctl status mosquitto");
    return false;
  }
  return true;
}

// Set when the configured broker failed and the mDNS name answered, so the
// next reconnect goes to the name first.
static bool mqttPreferMdnsFallback = false;

bool connectMqtt() {
  if (mqttClient.connected()) {
    return true;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARN] Skipping MQTT connect - WiFi not ready");
    return false;
  }

  // The configured broker is often the Pi's DHCP address. After the Pi
  // reboots it can come back on a new one (19 Oct: .30 -> .135), so a failed
  // connect also tries the Pi's mDNS name.
  const char* configured = mqttBrokerHost();
  const char* fallback = MQTT_BROKER_MDNS_HOST;
  const bool haveFallback = fallback != nullptr && fallback[0] != '\0' &&
                            strcasecmp(fallback, configured) != 0;
  const char* first = (haveFallback && mqttPreferMdnsFallback) ? fallback : configured;
  const char* second = !haveFallback ? nullptr : (first == fallback ? configured : fallback);

  bool ok = connectMqttTo(first);
  if (!ok && second != nullptr) {
    Serial.print("[NET] MQTT broker ");
    Serial.print(first);
    Serial.print(" did not answer; trying ");
    Serial.println(second);
    ok = connectMqttTo(second);
    if (ok) {
      mqttPreferMdnsFallback = second == fallback;
    }
  }
  if (!ok) {
    Serial.println("       The network task retries with backoff; MQTT CONNECT tries now.");
    return false;
  }

  Serial.print("[DEBUG] MQTT connected; publishing to ");
  Serial.println(MQTT_TOPIC);
  Serial.print("[DEBUG] MQTT alerts (retained) on ");
  Serial.println(MQTT_ALERT_TOPIC);
  // TODO(pi-cal): dashboard publishes {"cmd":"pump_cal","action":"enter","pump":"…"}
  // then prime / run / stop / volume. Human must confirm the cup before "run".
  mqttClient.subscribe(MQTT_CMD_TOPIC);
  Serial.print("[DEBUG] MQTT subscribed to command topic ");
  Serial.println(MQTT_CMD_TOPIC);
  return true;
}

void onMqttCommand(char* topic, byte* payload, unsigned int length) {
  String body;
  body.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    body += static_cast<char>(payload[i]);
  }
  Serial.print("[MQTT] command on ");
  Serial.print(topic);
  Serial.print(": ");
  Serial.println(body);

  JsonDocument cmd;
  DeserializationError err = deserializeJson(cmd, body);
  if (err) {
    Serial.println("[MQTT] command JSON parse failed");
    return;
  }

  const char* command = cmd["cmd"] | "";

  if (strcmp(command, "ph_cal") == 0) {
    const char* action = cmd["action"] | "";
    if (strcmp(action, "enter") == 0) {
      PhProbeCal::enter();
    } else if (strcmp(action, "abort") == 0) {
      PhProbeCal::abortSession();
    } else if (strcmp(action, "clear") == 0) {
      PhProbeCal::clearStore();
    } else if (strcmp(action, "save") == 0 || strcmp(action, "exit") == 0) {
      PhProbeCal::save();
    } else if (strcmp(action, "capture") == 0) {
      const char* point = cmd["point"] | "acid";
      // nominal = buffer label (4.00 / 9.18); its pH at the buffer
      // temperature comes from the sachet table in PhProbeCal.
      float nominal = cmd["nominal"] | NAN;
      #if ENABLE_DS18B20
      waterTempC = readDS18B20Average();  // DS18B20 sits in the buffer
      #endif
      // temp_c: hand-measured buffer °C when the DS18B20 is unavailable.
      const float bufferTempC = cmd["temp_c"] | waterTempC;
      waitForTdsQuiet();
      const float boardV = AnalogFilter::readGpioVoltsAverage(
                               PH_PIN, AnalogFilter::CAL_SAMPLES, 5) *
                           PH_SIGNAL_LEVEL_SHIFT_GAIN;
      phVoltage = boardV;
      phValue = convertVoltageToPH(boardV);
      phValueFiltered = phValue;
      PhProbeCal::capturePoint(point, boardV, nominal, bufferTempC);
    } else if (strcmp(action, "status") == 0) {
      #if ENABLE_DS18B20
      waterTempC = readDS18B20Average();
      #endif
      readPhSensorValues();
      PhProbeCal::printStatus(phVoltage, waterTempC);
    } else {
      Serial.println("[PH-CAL] Unknown action — enter|capture|save|abort|clear|status");
      return;
    }
    // Push ph_cal_* fields immediately — don't wait for the next sensor cycle.
    publishTelemetryToMqtt();
    return;
  }

  if (strcmp(command, "tds_cal") == 0) {
    const char* action = cmd["action"] | "";
    if (strcmp(action, "enter") == 0) {
      TdsProbeCal::enter();
    } else if (strcmp(action, "abort") == 0) {
      TdsProbeCal::abortSession();
    } else if (strcmp(action, "clear") == 0) {
      TdsProbeCal::clearStore();
    } else if (strcmp(action, "save") == 0 || strcmp(action, "exit") == 0) {
      TdsProbeCal::save();
    } else if (strcmp(action, "capture") == 0) {
      float knownPpm = cmd["ppm"] | NAN;
      if (isnan(knownPpm)) {
        knownPpm = cmd["nominal"] | NAN;
      }
      #if ENABLE_DS18B20
      waterTempC = readDS18B20Average();
      #endif
      const float tdsV = readTdsGpioVoltsPowered(AnalogFilter::CAL_SAMPLES);
      tdsADC = static_cast<int>(tdsV * 4095.0f / 3.3f + 0.5f);
      tdsPPM = convertTDSToPPM(tdsADC);
      TdsProbeCal::capturePoint(tdsV, waterTempC, knownPpm);
    } else if (strcmp(action, "status") == 0) {
      #if ENABLE_DS18B20
      waterTempC = readDS18B20Average();
      #endif
      tdsADC = readTdsAdcPowered();
      const float tdsV = tdsADC * 3.3f / 4095.0f;
      tdsPPM = convertTDSToPPM(tdsADC);
      TdsProbeCal::printStatus(tdsV, waterTempC);
    } else {
      Serial.println("[TDS-CAL] Unknown action — enter|capture|save|abort|clear|status");
      return;
    }
    publishTelemetryToMqtt();
    return;
  }

  if (strcmp(command, "pump_cal") == 0 || strcmp(command, "calibrate") == 0) {
    const char* action = cmd["action"] | "";
    // Legacy {"cmd":"calibrate","pump":"water"} → same as enter.
    if (strcmp(command, "calibrate") == 0 && action[0] == '\0') {
      action = "enter";
    }

    if (strcmp(action, "enter") == 0 || strcmp(action, "start") == 0) {
      const char* pumpName = cmd["pump"] | "";
      PumpCal::Id id = PumpCal::idFromName(pumpName);
      if (id == PumpCal::COUNT) {
        setCalMqttStatus("unknown_pump", false);
        Serial.println("[CAL] MQTT enter needs pump: water|nutrient|ph_up|ph_down");
      } else {
        beginCalSequence(false, id);
      }
    } else if (strcmp(action, "abort") == 0 || strcmp(action, "cancel") == 0) {
      if (calPhase == CAL_IDLE) {
        setCalMqttStatus("not_active", false);
      } else {
        stopCalPump(calPump);
        finishCalSession();
        setCalMqttStatus("aborted");
      }
    } else if (strcmp(action, "skip") == 0) {
      if (calPhase == CAL_IDLE) {
        setCalMqttStatus("not_active", false);
      } else {
        stopCalPump(calPump);
        Serial.print("[CAL] Skipped ");
        Serial.println(PumpCal::name(calPump));
        advanceCalSequence();
        setCalMqttStatus(calPhase == CAL_IDLE ? "finished" : "skipped");
      }
    } else if (strcmp(action, "prime") == 0) {
      if (calPhase != CAL_WAIT_READY) {
        setCalMqttStatus("not_ready", false);
        Serial.println("[CAL] MQTT prime only while wait_ready (STOP first if pumping).");
      } else {
        startPrime();
      }
    } else if (strcmp(action, "run") == 0 || strcmp(action, "yes") == 0) {
      if (calPhase == CAL_WAIT_READY) {
        startCalBurstRun();
      } else if (calPhase == CAL_PRIMING) {
        setCalMqttStatus("still_priming", false);
        Serial.println("[CAL] Still priming — STOP when liquid comes out, then run.");
      } else {
        setCalMqttStatus("not_ready", false);
        Serial.println("[CAL] MQTT run only while wait_ready.");
      }
    } else if (strcmp(action, "stop") == 0) {
      if (calPhase == CAL_PRIMING) {
        finishPrime("liquid at outlet");
      } else if (calPhase == CAL_RUNNING && calIsContinuous() && calTargetMs == 0) {
        calStopRequested = true;
        setCalMqttStatus("stop_noted");
        Serial.println("[CAL] STOP noted — finishing this continuous run.");
      } else if (calPhase == CAL_RUNNING && !calIsContinuous() && calTargetBursts == 0) {
        calStopRequested = true;
        setCalMqttStatus("stop_noted");
        Serial.println("[CAL] STOP noted — finishing after this burst.");
      } else if (calPhase == CAL_RUNNING) {
        if (calIsContinuous()) {
          calPulseMs = millis() - calRunStartedAt;
        }
        finishCalMeasure();
      } else {
        setCalMqttStatus("nothing_running", false);
      }
    } else if (strcmp(action, "retry") == 0) {
      if (calPhase == CAL_WAIT_VOLUME || calPhase == CAL_WAIT_READY) {
        startCalBurstRun();
      } else {
        setCalMqttStatus("not_ready", false);
      }
    } else if (strcmp(action, "volume") == 0) {
      if (calPhase != CAL_WAIT_VOLUME) {
        setCalMqttStatus("not_wait_volume", false);
        Serial.println("[CAL] MQTT volume only after a completed cup run.");
      } else {
        float ml = cmd["ml"] | NAN;
        if (isnan(ml)) {
          ml = cmd["volume"] | NAN;
        }
        if (isnan(ml)) {
          setCalMqttStatus("bad_volume", false);
        } else {
          acceptCalVolume(ml);
        }
      }
    } else if (strcmp(action, "clear") == 0) {
      PumpCal::clear();
      setCalMqttStatus("cleared");
    } else if (strcmp(action, "status") == 0 || strcmp(action, "show") == 0) {
      PumpCal::printRates();
      if (calMqttMessage[0] == '\0') {
        setCalMqttStatus(calPhaseName());
      }
    } else {
      setCalMqttStatus("unknown_action", false);
      Serial.println("[CAL] Unknown action — enter|prime|run|stop|volume|retry|skip|abort|clear|status");
      return;
    }
    publishTelemetryToMqtt();
    return;
  }

  if (strcmp(command, "light_flash") == 0) {
    // Camera flash: white for a photo, then back to the photoperiod. The camera
    // waits for the ack so it only shoots once the strip is lit.
    const char* action = cmd["action"] | "on";
    bool flashing = false;
    #if ENABLE_GROW_LIGHT
    if (strcmp(action, "off") == 0) {
      GrowLight::endFlash();
    } else {
      GrowLight::startFlash(cmd["ms"] | 15000UL);
    }
    flashing = GrowLight::flashActive();
    #endif
    JsonDocument ack;
    ack["cmd"] = "light_flash";
    ack["flash"] = flashing;
    ack["level_pct"] = (GrowLight::FLASH_LEVEL * 100) / 255;
    #if ENABLE_GROW_LIGHT
    ack["mode"] = GrowLight::status().mode;
    #endif
    char out[160];
    const size_t len = serializeJson(ack, out, sizeof(out));
    Comms::publish(MQTT_ACK_TOPIC, out, len, false);
    return;
  }

  if (strcmp(command, "light_makeup") == 0) {
    // Repay scheduled light missed while the garden was off: action
    // on | off | clear | status. Acked so the dashboard can show refusals.
    const char* action = cmd["action"] | "on";
    char msg[160];
    const bool ok = lightMakeupCommand(action, "mqtt", msg, sizeof(msg));
    Serial.print("[LIGHT] MQTT: ");
    Serial.println(msg);
    JsonDocument ack;
    ack["cmd"] = "light_makeup";
    ack["ok"] = ok;
    ack["message"] = msg;
    ack["armed"] = GrowLight::makeupArmed();
    ack["owed_s"] = GrowLight::makeupOwedS();
    char out[320];
    const size_t len = serializeJson(ack, out, sizeof(out));
    Comms::publish(MQTT_ACK_TOPIC, out, len, false);
    publishTelemetryToMqtt();
    return;
  }

  if (strcmp(command, "light_on") == 0 || strcmp(command, "light_auto") == 0) {
    // light_on {"hours": h}: grow light on now for h hours (no hours: the owed
    // make-up light), then back to the schedule. light_auto ends it.
    const float hours = strcmp(command, "light_auto") == 0 ? -1.0f : (cmd["hours"] | 0.0f);
    char msg[160];
    const bool ok = lightOnCommand(hours, "mqtt", msg, sizeof(msg));
    Serial.print("[LIGHT] MQTT: ");
    Serial.println(msg);
    JsonDocument ack;
    ack["cmd"] = command;
    ack["ok"] = ok;
    ack["message"] = msg;
    ack["manual_left_s"] = GrowLight::manualLeftS();
    char out[320];
    const size_t len = serializeJson(ack, out, sizeof(out));
    Comms::publish(MQTT_ACK_TOPIC, out, len, false);
    publishTelemetryToMqtt();
    return;
  }

  if (strcmp(command, "cycle") == 0) {
    // {"cmd":"cycle"} runs every loop; "loop": "level" | "nutrient" | "ph" runs one.
    const char* loop = cmd["loop"] | "all";
    if (strcmp(loop, "all") == 0) {
      StageController::requestManualCycle();
      Serial.println("[STAGE] MQTT: manual control cycle requested (level → EC → pH).");
    } else if (StageController::requestLoopRun(loop)) {
      Serial.print("[STAGE] MQTT: ");
      Serial.print(loop);
      Serial.println(" loop run requested");
    } else {
      Serial.println("[STAGE] MQTT cycle loop must be all | level | nutrient | ph");
    }
    publishTelemetryToMqtt();
    return;
  } else if (strcmp(command, "auto_pause") == 0 || strcmp(command, "pause") == 0) {
    setAutoControlPaused(true, "mqtt");
    publishTelemetryToMqtt();
    return;
  } else if (strcmp(command, "auto_resume") == 0 || strcmp(command, "resume") == 0) {
    setAutoControlPaused(false, "mqtt");
    publishTelemetryToMqtt();
    return;
  } else if (strcmp(command, "loop_pause") == 0) {
    const char* loop = cmd["loop"] | "";
    const bool paused = cmd["paused"] | true;
    setLoopPaused(loop, paused, "mqtt");
    publishTelemetryToMqtt();
    return;
  } else if (strcmp(command, "refill_proceed") == 0) {
    proceedRefill("mqtt");
    publishTelemetryToMqtt();
    return;
  } else if (strcmp(command, "water_changed") == 0) {
    resetSolutionMonitor("mqtt");
    publishTelemetryToMqtt();
    return;
  } else if (strcmp(command, "session_reset") == 0) {
    const char* loop = cmd["loop"] | "";
    const bool ph = strcmp(loop, "ph") == 0 || strcmp(loop, "all") == 0;
    const bool ec = strcmp(loop, "ec") == 0 || strcmp(loop, "nutrient") == 0 ||
                    strcmp(loop, "all") == 0;
    if (!ph && !ec) {
      Serial.println("[DOSE] session_reset needs loop: ph | ec | all");
      return;
    }
    resetDosingSession(ph, ec, "mqtt");
    publishTelemetryToMqtt();
    return;
  } else if (strcmp(command, "dose") == 0) {
    const char* pumpName = cmd["pump"] | "";
    float ml = cmd["ml"] | NAN;
    if (isnan(ml)) {
      ml = cmd["volume"] | NAN;
    }
    const PumpCal::Id pid = PumpCal::idFromName(pumpName);
    if (pid >= PumpCal::COUNT || isnan(ml)) {
      Serial.println("[DOSE] MQTT needs {\"cmd\":\"dose\",\"pump\":\"ph_up\",\"ml\":0.15}");
      return;
    }
    startOperatorDose(pid, ml, "mqtt");
    publishTelemetryToMqtt();
    return;
  } else if (strcmp(command, "stage") == 0) {
    const char* value = cmd["value"] | "";
    GrowStage::Id stage;
    if (GrowStage::parseName(value, stage)) {
      setSystemMode(stage, "mqtt");
      publishTelemetryToMqtt();
    } else {
      Serial.print("[STAGE] MQTT unknown stage: ");
      Serial.println(value);
    }
    return;
  } else if (strcmp(command, "setpoints") == 0) {
    // Dashboard plant-catalogue apply: EC (ppm500) and pH target bands, plus
    // optional dead limits (the plants' tolerance range) and pH setpoint.
    // Example:
    // {"cmd":"setpoints","stage":"vegetative",
    //  "ec_min":600,"ec_max":800,"ph_min":6.0,"ph_max":6.0,
    //  "ec_dead_low":550,"ec_dead_high":850,
    //  "ph_dead_low":5.5,"ph_dead_high":6.5,"ph_setpoint":6.0}
    const char* stageName = cmd["stage"] | "vegetative";
    GrowStage::Id stage;
    if (!GrowStage::parseName(stageName, stage)) {
      Serial.print("[SETPOINTS] unknown stage: ");
      Serial.println(stageName);
      return;
    }
    float ecMin = cmd["ec_min"] | NAN;
    if (isnan(ecMin)) {
      ecMin = cmd["tds_min"] | NAN;
    }
    float ecMax = cmd["ec_max"] | NAN;
    if (isnan(ecMax)) {
      ecMax = cmd["tds_max"] | NAN;
    }
    float phMin = cmd["ph_min"] | NAN;
    float phMax = cmd["ph_max"] | NAN;
    const float ecDeadLow = cmd["ec_dead_low"] | NAN;
    const float ecDeadHigh = cmd["ec_dead_high"] | NAN;
    const float phDeadLow = cmd["ph_dead_low"] | NAN;
    const float phDeadHigh = cmd["ph_dead_high"] | NAN;
    const float phSetpoint = cmd["ph_setpoint"] | NAN;
    if (!GrowStage::applyChemistryBands(stage, ecMin, ecMax, phMin, phMax, ecDeadLow,
                                        ecDeadHigh, phDeadLow, phDeadHigh, phSetpoint)) {
      Serial.println(
          "[SETPOINTS] need ec_min/ec_max (ppm) and ph_min/ph_max with min<=max; "
          "dead limits must enclose the target band and setpoint");
      return;
    }
    StageController::saveChemistryBands(stage);
    // Gains describe the tank, not the setpoints, so they are kept: a new
    // band changes where the loop aims, not how strongly a mL moves it.
    const GrowStage::Config& applied = GrowStage::config(stage);
    Serial.print("[SETPOINTS] ");
    Serial.print(GrowStage::name(stage));
    Serial.print(" EC ");
    Serial.print(applied.ecTargetMin, 1);
    Serial.print("-");
    Serial.print(applied.ecTargetMax, 1);
    Serial.print(" ppm (dead ");
    Serial.print(applied.ecDeadLow, 1);
    Serial.print("-");
    Serial.print(applied.ecDeadHigh, 1);
    Serial.print(")  pH ");
    Serial.print(applied.phCenter, 2);
    Serial.print(" (dead ");
    Serial.print(applied.phDeadLow, 2);
    Serial.print("-");
    Serial.print(applied.phDeadHigh, 2);
    Serial.println(")");
    publishTelemetryToMqtt();
    return;
  } else if (strcmp(command, "confirm_fill") == 0) {
    confirmWaterFillTouch();
    publishTelemetryToMqtt();
    return;
  }
}

// Builds the telemetry document on the loop task and hands it to the network
// task. seq > 0: a sensor reading (journal row csvLine is replayed if the live
// publish misses). seq == 0: a status snapshot after a dashboard command.
bool publishTelemetryToMqtt(uint32_t seq, const char* csvLine) {
  const GrowPreset& preset = activePreset();
  JsonDocument doc;

  doc["device_id"] = MQTT_DEVICE_ID;
  doc["firmware_version"] = FIRMWARE_VERSION;
  // Reading identity: the Pi stores one row per (device_id, boot_id, seq).
  doc["kind"] = seq > 0 ? "reading" : "status";
  doc["boot_id"] = bootId;
  if (seq > 0) {
    doc["seq"] = seq;
  }
  doc["timestamp"] = currentIsoTimestamp();
  doc["system_mode"] = systemModeName();
  doc["grow_stage"] = preset.name;
  doc["stage_persisted"] = StageController::ctx().stagePersisted;
  doc["control_idle"] = isControlIdleMode();
  doc["auto_control_paused"] = autoControlPaused;
  doc["ready_to_seed"] = StageController::ctx().readyToSeed;
  doc["fill_0_pct"] = fill0Percent();
  {
    const GrowStage::Config& cfg = GrowStage::config(systemMode);
    doc["target_drawdown_mm"] = cfg.targetDrawdownMm;
    doc["drawdown_tolerance_mm"] = cfg.drawdownToleranceMm;
    doc["level_check_s"] = cfg.levelCheckIntervalMs / 1000UL;
    doc["ph_check_s"] = cfg.phCheckIntervalMs / 1000UL;
    doc["ec_check_s"] = cfg.ecCheckIntervalMs / 1000UL;
    doc["ec_control_enabled"] = cfg.ecControlEnabled;
    doc["ec_dead_low"] = cfg.ecDeadLow;
    doc["ec_dead_high"] = cfg.ecDeadHigh;
    doc["ec_min_dose_ml"] = cfg.ecMinDoseMl;
    doc["ec_max_dose_ml"] = cfg.ecMaxDoseMl;
    doc["ec_settle_s"] = cfg.ecSettleMs / 1000UL;
    doc["ph_dead_low"] = cfg.phDeadLow;
    doc["ph_dead_high"] = cfg.phDeadHigh;
    doc["ph_center"] = cfg.phCenter;
    doc["ph_min_dose_ml"] = cfg.phMinDoseMl;
    doc["ph_max_dose_ml"] = cfg.phMaxDoseMl;
    doc["ph_session_cap_ml"] = cfg.phSessionCapMl;
    doc["ec_session_cap_ml"] = ControlDesign::EC_SESSION_CAP_ML;
    doc["ph_session_ml"] = StageController::ctx().ph.cumulativeSessionMl;
    // pH doses that settled without moving pH (ControlDesign::PH_NO_RESPONSE_DOSES).
    doc["ph_up_no_response"] = StageController::ctx().ph.noResponseFaultUp;
    doc["ph_down_no_response"] = StageController::ctx().ph.noResponseFaultDown;
    doc["ph_up_unanswered_doses"] = StageController::ctx().ph.noResponseUp;
    doc["ph_down_unanswered_doses"] = StageController::ctx().ph.noResponseDown;
    doc["nutrient_no_response"] = StageController::ctx().ec.noResponseFault;
    doc["nutrient_unanswered_doses"] = StageController::ctx().ec.noResponseCount;
    doc["ec_session_ml"] = StageController::ctx().ec.cumulativeSessionMl;
    // gain_* are process gains (change per mL): ppm/mL for EC, pH/mL for pH.
    doc["gain_units"] = "per_ml";
    doc["ctrl_lambda"] = ControlDesign::LAMBDA;
    doc["ctrl_lambda_ph"] = ControlDesign::LAMBDA_PH;
    doc["gain_ec_confirmed"] = StageController::ctx().ec.gainConfirmed;
    doc["gain_ph_up_confirmed"] = StageController::ctx().ph.gainUpConfirmed;
    doc["gain_ph_down_confirmed"] = StageController::ctx().ph.gainDownConfirmed;
    doc["ph_baseline_recheck_s"] = cfg.phBaselineRecheckMs / 1000UL;
    doc["ph_control_enabled"] = GrowStage::allowsPh(systemMode);
    doc["level_control_enabled"] = GrowStage::allowsLevelAuto(systemMode) &&
                                   !isWaterLevelSensorFault() &&
                                   !isWaterLevelInitRequired();
    doc["transitional_strength_fraction"] =
        GrowStage::TRANSITIONAL_STRENGTH_FRACTION;
  }
  doc["current_drawdown_mm"] =
      LevelControl::drawdownMm(fill0Percent(), waterLevelPercent);
  doc["gain_ph_up"] = StageController::ctx().ph.gainUp;
  doc["gain_ph_down"] = StageController::ctx().ph.gainDown;
  doc["gain_ec"] = StageController::ctx().ec.gainEc;
  doc["nutrients_allowed"] = nutrientsAllowedByMode();
  doc["hydroponic_mode"] = hydroponicModeName();
  doc["ph_settle_s"] = phSettleMs() / 1000UL;
  doc["control_law"] = CHEMISTRY_CONTROL_LAW;
  doc["pumps_calibrated"] = PumpCal::calibratedCount();
  doc["water_calibrated"] = PumpCal::isCalibrated(PumpCal::WATER);
  doc["nutrient_calibrated"] = PumpCal::isCalibrated(PumpCal::NUTRIENT);
  doc["ph_up_calibrated"] = PumpCal::isCalibrated(PumpCal::PH_UP);
  doc["ph_down_calibrated"] = PumpCal::isCalibrated(PumpCal::PH_DOWN);
  if (PumpCal::isCalibrated(PumpCal::WATER)) {
    doc["water_ml_per_s"] = PumpCal::mlPerSec(PumpCal::WATER);
  }
  if (PumpCal::isCalibrated(PumpCal::NUTRIENT)) {
    doc["nutrient_ml_per_burst"] = PumpCal::mlPerBurst(PumpCal::NUTRIENT);
  }
  if (PumpCal::isCalibrated(PumpCal::PH_UP)) {
    doc["ph_up_ml_per_burst"] = PumpCal::mlPerBurst(PumpCal::PH_UP);
  }
  if (PumpCal::isCalibrated(PumpCal::PH_DOWN)) {
    doc["ph_down_ml_per_burst"] = PumpCal::mlPerBurst(PumpCal::PH_DOWN);
  }
  doc["pump_cal_active"] = pumpCalibrationActive();
  doc["pump_cal_state"] = calPhaseName();
  doc["pump_cal_pump"] = pumpCalibrationActive() ? PumpCal::name(calPump) : "";
  doc["pump_cal_message"] = calMqttMessage;
  doc["pump_cal_ok"] = calMqttOk;
  doc["pump_cal_mode"] = calIsContinuous() ? "continuous" : "burst";
  doc["pump_cal_run"] = calRateCount;
  doc["pump_cal_runs_needed"] = 1 + CAL_CONFIRM_RUNS;
  doc["pump_cal_bursts"] = calBurstsDone;
  doc["pump_cal_pulse_ms"] = static_cast<int>(calPulseMs);
  doc["pump_cal_target_bursts"] = calTargetBursts;
  doc["pump_cal_target_ms"] = static_cast<int>(calTargetMs);
  if (!isnan(calLastSample)) {
    doc["pump_cal_last_sample"] = calLastSample;
  }
  doc["pump_cal_unit"] = calIsContinuous() ? "mL/s" : "mL/burst";
  doc["water_max_pct"] = waterCeilingPercent();
  doc["net_pot_touch_pct"] = netPotTouchPercent;
  doc["water_safety_floor_pct"] = waterSafetyFloorPercent();
  doc["water_air_gap_mm"] = GrowStage::config(systemMode).targetDrawdownMm;

  doc["water_cap_ml"] = PumpCal::capacityMl(PumpCal::WATER);
  doc["water_rem_ml"] = PumpCal::remainingMl(PumpCal::WATER);
  doc["water_empty"] = PumpCal::isEmpty(PumpCal::WATER);
  doc["nutrient_cap_ml"] = PumpCal::capacityMl(PumpCal::NUTRIENT);
  doc["nutrient_rem_ml"] = PumpCal::remainingMl(PumpCal::NUTRIENT);
  doc["nutrient_empty"] = PumpCal::isEmpty(PumpCal::NUTRIENT);
  doc["ph_up_cap_ml"] = PumpCal::capacityMl(PumpCal::PH_UP);
  doc["ph_up_rem_ml"] = PumpCal::remainingMl(PumpCal::PH_UP);
  doc["ph_up_empty"] = PumpCal::isEmpty(PumpCal::PH_UP);
  doc["ph_down_cap_ml"] = PumpCal::capacityMl(PumpCal::PH_DOWN);
  doc["ph_down_rem_ml"] = PumpCal::remainingMl(PumpCal::PH_DOWN);
  doc["ph_down_empty"] = PumpCal::isEmpty(PumpCal::PH_DOWN);
  char stockAlert[96];
  PumpCal::fillStockAlert(stockAlert, sizeof(stockAlert));
  doc["stock_alert"] = stockAlert;
  char calAlert[96];
  PumpCal::fillCalAlert(calAlert, sizeof(calAlert));
  doc["cal_alert"] = calAlert;
  doc["cal_needed"] =
      PumpCal::needsCalibrationAlert(
          GrowStage::allowsEc(systemMode));

  JsonArray alerts = doc["alerts"].to<JsonArray>();
  if (calAlert[0] != '\0') {
    alerts.add(calAlert);
  }
  if (stockAlert[0] != '\0') {
    alerts.add(stockAlert);
  }
  if (PumpCal::needsCalibrationAlert(GrowStage::allowsEc(systemMode))) {
    alerts.add("action:run_pump_cal");
  }
  if (isWaterLevelSensorFault()) {
    alerts.add("water_level_sensor_fault");
    alerts.add("action:lift_dry_power_cycle_water_level");
  }
  if (isWaterLevelInitRequired()) {
    alerts.add("water_level_init_required");
    alerts.add("action:lift_and_lower_water_level_stick");
  }
  if (phSensorFault) {
    alerts.add("ph_sensor_fault");
  }
  if (tdsSensorFault) {
    alerts.add("tds_sensor_fault");
  }
  if (StageController::ctx().ph.noResponseFaultUp) {
    alerts.add("ph_up_no_response");
  }
  if (StageController::ctx().ph.noResponseFaultDown) {
    alerts.add("ph_down_no_response");
  }
  if (StageController::ctx().ec.noResponseFault) {
    alerts.add("nutrient_no_response");
  }
  {
    // Refill progress and hold (level loop, 1.9.24).
    const StageController::Context& c = StageController::ctx();
    if (c.refillHold) {
      alerts.add(strcmp(c.refillHoldReason, "no_level_rise") == 0 ? "refill_hold_no_level_rise"
                                                                   : "refill_hold_noisy_level");
      alerts.add("action:confirm_refill");
    }
    JsonObject lp = doc["loops_paused"].to<JsonObject>();
    lp["level"] = c.levelPaused;
    lp["nutrient"] = c.nutrientPaused;
    lp["ph"] = c.phPaused;
    JsonObject r = doc["refill"].to<JsonObject>();
    r["active"] = c.refilling;
    r["hold"] = c.refillHold;
    r["hold_reason"] = c.refillHoldReason;
    r["start_pct"] = c.refillStartPct;
    r["now_pct"] = waterLevelPercentForControl();
    r["target_pct"] = StageController::refillTargetPercent();
    r["planned_ml"] = c.refillPlannedMl;
    r["delivered_ml"] = c.refillDeliveredMl;
    r["minutes"] = c.refilling ? (millis() - c.refillStartedMs) / 60000UL : 0;
    r["spread_pct"] = waterLevelSpreadPercent();
    r["ml_per_mm"] = c.mlPerMmEst;
    if (c.refillLastDoneMs != 0) {
      r["last_done_min_ago"] = (millis() - c.refillLastDoneMs) / 60000UL;
      r["last_delivered_ml"] = c.refillLastDeliveredMl;
      r["last_minutes"] = c.refillDoneMin;
    }
  }
  addSolutionJson(doc, alerts);

  doc["water_temp_c"] = waterTempC;
  doc["ambient_temp_c"] = ambientTempC;
  doc["ambient_humidity_pct"] = ambientHumidityPct;
  doc["light_adc"] = ldrADC;
  doc["water_level_pct"] = waterLevelPercent;
  doc["water_level_raw_pct"] = waterLevelRawPercent;
  // Solution volume from the level reading and the tank footprint (112 mL/mm).
  if (waterLevelPercent >= 0 && !isWaterLevelSensorFault()) {
    doc["tank_volume_l"] = Tank::geomMlFromGrovePct(static_cast<float>(waterLevelPercent)) / 1000.0f;
  } else {
    doc["tank_volume_l"] = nullptr;
  }
  doc["water_level_sensor_fault"] = isWaterLevelSensorFault();
  doc["water_level_init_required"] = isWaterLevelInitRequired();
  if (waterLevelTrust.lastTrustedPercent > 0) {
    doc["water_level_saved_pct"] = waterLevelTrust.lastTrustedPercent;
  }
  doc["water_target_pct"] = waterStageTargetPercent();
  doc["tds_ppm"] = tdsPPM;
  if (!isnan(tdsPpmEma)) {
    doc["tds_ppm_ema"] = tdsPpmEma;
  }
  {
    const TdsNutrientObserver::State& obs = TdsNutrientObserver::state();
    const float V = TdsNutrientObserver::volumeLFromLevelPct(waterLevelPercent);
    doc["tds_ppm_nutrient"] =
        TdsNutrientObserver::nutrientPpm(static_cast<float>(tdsPPM), V);
    doc["tds_ph_salt_ppm"] = TdsNutrientObserver::saltOffsetPpm(V);
    doc["tds_kappa_up"] = obs.kappaUp;
    doc["tds_kappa_down"] = obs.kappaDown;
    doc["tds_ph_up_ml"] = obs.cumPhUpMl;
    doc["tds_ph_down_ml"] = obs.cumPhDownMl;
    doc["tds_salt_correction_live"] = obs.liveCorrection;
    doc["tds_kappa_provisional"] = obs.kappaProvisional;
  }
  doc["tds_k"] = TdsProbeCal::kValue();
  doc["tds_temp_c"] = TdsProbeCal::effectiveTempC(waterTempC);
  doc["tds_comp_coeff"] = TdsProbeCal::compensationCoefficient(waterTempC);
  doc["tds_probe_calibrated"] = TdsProbeCal::isCalibrated();
  doc["tds_cal_active"] = TdsProbeCal::isActive();
  doc["tds_cal_state"] = TdsProbeCal::stateName();
  doc["tds_cal_message"] = TdsProbeCal::session().lastMessage;
  doc["tds_cal_ok"] = TdsProbeCal::session().lastOk;
  if (!isnan(TdsProbeCal::session().pendingK)) {
    doc["tds_cal_pending_k"] = TdsProbeCal::session().pendingK;
    doc["tds_cal_pending_ppm"] = TdsProbeCal::session().pendingRefPpm;
  }
  if (TdsProbeCal::isCalibrated()) {
    doc["tds_cal_stored_ppm"] = TdsProbeCal::store().refPpm;
    doc["tds_cal_stored_k"] = TdsProbeCal::store().kValue;
  }
  doc["nutrient_tds_min"] = preset.nutrientTdsMinPpm;
  doc["nutrient_tds_max"] = preset.nutrientTdsMaxPpm;
  doc["ph_raw"] = phValue;
  doc["ph_filtered"] = phValueFiltered;
  if (!isnan(phValueEma)) {
    doc["ph_ema"] = phValueEma;
  }
  doc["ph"] = SensorEma::forControl(phValueEma, phValueFiltered);
  doc["ph_voltage"] = phVoltage;
  doc["ph_sensor_fault"] = phSensorFault;
  doc["tds_sensor_fault"] = tdsSensorFault;
  doc["ph_temp_c"] = PhProbeCal::effectiveTempC(waterTempC);
  doc["ph_comp_factor"] = PhProbeCal::compensationFactor(waterTempC);
  doc["ph_slope"] = PhProbeCal::slope();
  doc["ph_offset"] = PhProbeCal::offset();
  doc["ph_probe_calibrated"] = PhProbeCal::isCalibrated();
  doc["ph_cal_active"] = PhProbeCal::isActive();
  doc["ph_cal_state"] = PhProbeCal::stateName();
  doc["ph_cal_message"] = PhProbeCal::session().lastMessage;
  doc["ph_cal_ok"] = PhProbeCal::session().lastOk;
  {
    const PhProbeCal::Session& sess = PhProbeCal::session();
    if (!isnan(sess.pendingAcidV)) {
      doc["ph_cal_acid_v"] = sess.pendingAcidV;
      doc["ph_cal_acid_temp_c"] = sess.pendingAcidTempC;
      doc["ph_cal_acid_buffer_ph"] = sess.pendingAcidBufferPh;
    }
    if (!isnan(sess.pendingAlkaliV)) {
      doc["ph_cal_alkali_v"] = sess.pendingAlkaliV;
      doc["ph_cal_alkali_temp_c"] = sess.pendingAlkaliTempC;
      doc["ph_cal_alkali_buffer_ph"] = sess.pendingAlkaliBufferPh;
    }
  }
  if (PhProbeCal::isCalibrated()) {
    const PhProbeCal::Store& st = PhProbeCal::store();
    doc["ph_cal_stored_acid_v"] = st.acidVoltage;
    doc["ph_cal_stored_alkali_v"] = st.alkaliVoltage;
    if (!isnan(st.acidTempC)) {
      doc["ph_cal_stored_acid_temp_c"] = st.acidTempC;
      doc["ph_cal_stored_acid_buffer_ph"] = st.acidBufferPh;
    }
    if (!isnan(st.alkaliTempC)) {
      doc["ph_cal_stored_alkali_temp_c"] = st.alkaliTempC;
      doc["ph_cal_stored_alkali_buffer_ph"] = st.alkaliBufferPh;
    }
  }
  if (phControlAllowedByMode()) {
    doc["ph_target_min"] = preset.phMin;
    doc["ph_target_max"] = preset.phMax;
  }
  doc["co2_ppm"] = co2PPM;
  doc["ph_dose"] = phDoseLabel();
  doc["nutrient_dose"] = nutrientDoseLabel();

  doc["water_pump_on"] = waterRefillPumpRunning;
  doc["drain_pump_on"] = waterDrainPumpRunning;
  doc["nutrient_pump_on"] = nutrientControlState == NUTRIENT_DOSING;
  doc["ph_up_pump_on"] = phControlState == PH_DOSING_UP;
  doc["ph_down_pump_on"] = phControlState == PH_DOSING_DOWN;

  #if ENABLE_GROW_LIGHT
  {
    const GrowLight::Status gl = GrowLight::status();
    doc["grow_light_on"] = gl.on;
    doc["grow_light_brightness_pct"] = gl.brightnessPct;
    doc["grow_light_animating"] = gl.animating;
    doc["grow_light_mode"] = gl.mode;
    doc["grow_light_photoperiod_h"] = gl.photoperiodOnHours;
    doc["grow_light_schedule_start_utc"] = gl.scheduleStartHourUtc;
    doc["grow_light_ntp_schedule"] = gl.ntpSchedule;
    doc["grow_light_schedule_off_utc"] = gl.scheduleOffHourUtc;
    doc["grow_light_want_on"] = gl.wantOn;
    doc["grow_light_next_change_s"] = gl.nextChangeS;
    doc["grow_light_flash"] = gl.flash;
    doc["grow_light_makeup"] = gl.makeup;
    doc["grow_light_makeup_armed"] = gl.makeupArmed;
    doc["grow_light_makeup_owed_s"] = gl.makeupOwedS;
    doc["grow_light_makeup_max_night_s"] = GrowLight::MAKEUP_MAX_PER_NIGHT_S;
    doc["grow_light_manual"] = gl.manual;
    doc["grow_light_manual_left_s"] = gl.manualLeftS;
  }
  #endif

  doc["uptime_ms"] = millis();
  doc["reset_reason"] = PowerMonitor::lastResetReasonName();
  doc["boot_count"] = PowerMonitor::bootCountValue();
  doc["brownout_count"] = PowerMonitor::brownoutCountValue();
  doc["last_reset_brownout"] = PowerMonitor::lastResetWasBrownout();
  doc["free_heap_b"] = PowerMonitor::freeHeapBytes();
  {
    const int railMv = PowerMonitor::readRailMv();
    if (railMv >= 0) {
      doc["rail_mv"] = railMv;
    }
  }
  if (PowerMonitor::wifiRssiValid()) {
    doc["wifi_rssi_dbm"] = PowerMonitor::wifiRssiDbm();
  }

  addHealthJson(doc);

#if ENABLE_INFLUXDB
  {
    JsonObject ix = doc["influx"].to<JsonObject>();
    const InfluxCloud::Stats& st = InfluxCloud::stats();
    ix["queued"] = InfluxCloud::queuedLines();
    ix["queue_max"] = InfluxCloud::kBacklogSlots;
    ix["hold"] = static_cast<bool>(InfluxCloud::controlHoldFlag());
    ix["ok_ago_s"] = InfluxCloud::secondsSinceOk(millis());
    ix["uploaded"] = st.uploadedLines;
    ix["dropped"] = st.droppedLines;
    ix["last_http"] = st.lastCode;
  }
#endif

  // Aliases accepted by older dashboard parsers.
  doc["temperature"] = ambientTempC;
  doc["humidity"] = ambientHumidityPct;
  doc["light_intensity"] = ldrADC;
  doc["water_level"] = waterLevelPercent;
  doc["pump_on"] = waterRefillPumpRunning;

  // Never drop the whole snapshot for size: the health text is the only
  // open-ended part, so it goes first (the health states and alerts stay).
  if (measureJson(doc) >= MQTT_TELEMETRY_MAX_BYTES) {
    Serial.print("[WARN] Telemetry ");
    Serial.print(measureJson(doc));
    Serial.println(" B over the MQTT limit - sent without health_msg");
    doc.remove("health_msg");
  }
  if (measureJson(doc) >= MQTT_TELEMETRY_MAX_BYTES) {
    doc.remove("sched");
  }

  const bool queued = Comms::submitTelemetry(doc, seq, csvLine);
  if (queued) {
    // Retained banner state rides along with each snapshot (dropped while
    // MQTT is down; the next one after reconnect restores it).
    publishAlertsToMqtt(calAlert, stockAlert,
                        PumpCal::needsCalibrationAlert(
                            GrowStage::allowsEc(systemMode)),
                        isWaterLevelSensorFault());
  }
  return queued;
}

// Retained alert snapshot for the Pi dashboard (banner / toast without
// parsing every telemetry frame). Empty strings + cal_needed=false clears it.
bool publishAlertsToMqtt(const char* calAlert,
                         const char* stockAlert,
                         bool calNeeded,
                         bool waterLevelFault) {
  if (!Comms::linkUp()) {
    return false;
  }

  JsonDocument doc;
  doc["device_id"] = MQTT_DEVICE_ID;
  doc["timestamp"] = currentIsoTimestamp();
  doc["cal_alert"] = calAlert != nullptr ? calAlert : "";
  doc["stock_alert"] = stockAlert != nullptr ? stockAlert : "";
  doc["cal_needed"] = calNeeded;
  doc["water_level_sensor_fault"] = waterLevelFault;
  doc["water_level_alert"] =
      waterLevelFault ? "stuck_0_after_fill:lift_dry_power_cycle" : "";
  doc["water_calibrated"] = PumpCal::isCalibrated(PumpCal::WATER);
  doc["nutrient_calibrated"] = PumpCal::isCalibrated(PumpCal::NUTRIENT);
  doc["ph_up_calibrated"] = PumpCal::isCalibrated(PumpCal::PH_UP);
  doc["ph_down_calibrated"] = PumpCal::isCalibrated(PumpCal::PH_DOWN);
  doc["pumps_calibrated"] = PumpCal::calibratedCount();
  doc["action"] = calNeeded ? "run_serial_CAL"
                 : waterLevelFault ? "lift_dry_power_cycle_water_level"
                                   : "";

  char payload[640];
  size_t len = serializeJson(doc, payload, sizeof(payload));
  if (len == 0 || len >= sizeof(payload)) {
    return false;
  }

  // retain=true so a late-connecting Pi still sees the last alert state.
  bool ok = Comms::publish(MQTT_ALERT_TOPIC, payload, len, true);
  if (ok && (calNeeded || waterLevelFault ||
             (calAlert != nullptr && calAlert[0] != '\0') ||
             (stockAlert != nullptr && stockAlert[0] != '\0'))) {
    Serial.print("[ALERT] MQTT ");
    Serial.print(MQTT_ALERT_TOPIC);
    Serial.print(" — ");
    if (calAlert != nullptr && calAlert[0] != '\0') {
      Serial.print(calAlert);
      Serial.print(" ");
    }
    if (stockAlert != nullptr && stockAlert[0] != '\0') {
      Serial.print(stockAlert);
      Serial.print(" ");
    }
    if (waterLevelFault) {
      Serial.print("water_level_sensor_fault");
    }
    Serial.println();
  }
  return ok;
}

// Publish a single CSV row (from LittleFS) to MQTT.
// CSV column order matches SensorDataStore::CSV_HEADER.
int publishCsvRowToMqtt(const char* csvLine) {
  if (!Comms::linkUp()) {
    return 0;
  }

  // Parse CSV tokens while preserving empty fields.
  // We avoid strtok because it drops empty tokens.
  char tokenBuf[40];
  const char* cursor = csvLine;

  char deviceIdBuf[32] = "";
  char recordedAtUtcBuf[24] = "";
  unsigned long uptimeMs = 0;
  uint32_t rowBootId = 0;
  uint32_t rowSeq = 0;
  char sourceBuf[24] = "esp32";
  char growStageBuf[32] = "";

  float waterTempCVal = NAN;
  float waterLevelPctVal = NAN;
  float waterLevelRawPctVal = NAN;
  float waterTargetPctVal = NAN;
  float waterMaxPctVal = NAN;
  float tdsPpmVal = NAN;
  float nutrientTdsMinVal = NAN;
  float nutrientTdsMaxVal = NAN;
  float phVoltageVal = NAN;
  float phValueVal = NAN;
  float phFilteredVal = NAN;
  float phEmaVal = NAN;
  float tdsPpmEmaVal = NAN;
  float co2PpmVal = NAN;
  float ambientTempCVal = NAN;
  float ambientHumidityPctVal = NAN;
  float lightAdcVal = NAN;
  float phTargetMinVal = NAN;
  float phTargetMaxVal = NAN;

  char nutrientDoseBuf[16] = "none";
  char phDoseBuf[16] = "none";
  int waterPumpOn = -1;
  int drainPumpOn = -1;
  int nutrientPumpOn = -1;
  int phUpPumpOn = -1;
  int phDownPumpOn = -1;

  auto tokenToFloat = [](const char* token, size_t tokenLen) -> float {
    if (tokenLen == 0) {
      return NAN;
    }
    return strtof(token, nullptr);
  };
  auto tokenToFlag = [](const char* token, size_t tokenLen) -> int {
    if (tokenLen == 0) {
      return -1;
    }
    return atoi(token) ? 1 : 0;
  };

  for (int fieldIndex = 0; fieldIndex < SensorDataStore::CSV_COLUMN_COUNT; fieldIndex++) {
    const char* comma = strchr(cursor, ',');
    size_t tokenLen = comma ? (size_t)(comma - cursor) : strlen(cursor);

    if (tokenLen >= sizeof(tokenBuf)) {
      return -1;
    }
    memcpy(tokenBuf, cursor, tokenLen);
    tokenBuf[tokenLen] = '\0';

    cursor = comma ? comma + 1 : cursor + tokenLen;

    switch (fieldIndex) {
      case 0:
        strncpy(deviceIdBuf, tokenBuf, sizeof(deviceIdBuf) - 1);
        break;
      case 1:
        strncpy(recordedAtUtcBuf, tokenBuf, sizeof(recordedAtUtcBuf) - 1);
        break;
      case 2:
        uptimeMs = static_cast<unsigned long>(strtoul(tokenBuf, nullptr, 10));
        break;
      case 3:
        strncpy(sourceBuf, tokenBuf, sizeof(sourceBuf) - 1);
        break;
      case 4:
        strncpy(growStageBuf, tokenBuf, sizeof(growStageBuf) - 1);
        break;
      case 5:
        waterTempCVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 6:
        lightAdcVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 7:
        waterLevelPctVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 8:
        waterLevelRawPctVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 9:
        waterTargetPctVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 10:
        waterMaxPctVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 11:
        break;
      case 12:
        tdsPpmVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 13:
        nutrientTdsMinVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 14:
        nutrientTdsMaxVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 15:
        strncpy(nutrientDoseBuf, tokenBuf, sizeof(nutrientDoseBuf) - 1);
        break;
      case 16:  // ph_voltage
        phVoltageVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 17:  // ph (raw)
        phValueVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 18:  // ph_filtered
        phFilteredVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 39:  // ph_ema
        phEmaVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 40:  // tds_ppm_ema
        tdsPpmEmaVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 41:  // boot_id
        rowBootId = static_cast<uint32_t>(strtoul(tokenBuf, nullptr, 10));
        break;
      case 42:  // seq
        rowSeq = static_cast<uint32_t>(strtoul(tokenBuf, nullptr, 10));
        break;
      case 19:
        phTargetMinVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 20:
        phTargetMaxVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 21:
        strncpy(phDoseBuf, tokenBuf, sizeof(phDoseBuf) - 1);
        break;
      case 22:
        co2PpmVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 23:
        ambientTempCVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 24:
        ambientHumidityPctVal = tokenToFloat(tokenBuf, tokenLen);
        break;
      case 25:
        waterPumpOn = tokenToFlag(tokenBuf, tokenLen);
        break;
      case 26:
        drainPumpOn = tokenToFlag(tokenBuf, tokenLen);
        break;
      case 27:
        nutrientPumpOn = tokenToFlag(tokenBuf, tokenLen);
        break;
      case 28:
        phUpPumpOn = tokenToFlag(tokenBuf, tokenLen);
        break;
      case 29:
        phDownPumpOn = tokenToFlag(tokenBuf, tokenLen);
        break;
      default:
        break;
    }
  }

  JsonDocument doc;

  doc["device_id"] = deviceIdBuf[0] != '\0' ? deviceIdBuf : MQTT_DEVICE_ID;

  if (recordedAtUtcBuf[0] != '\0') {
    doc["timestamp"] = recordedAtUtcBuf;
  } else if (ntpEpochValid && rowBootId == bootId) {
    // Logged before NTP answered, this boot: uptime maps onto the latch.
    // A row from an earlier boot has no usable wall clock.
    char isoBuf[24] = "";
    recordedAtUtcForUptime(
      uptimeMs,
        isoBuf,
        sizeof(isoBuf));
    if (isoBuf[0] != '\0') {
      doc["timestamp"] = isoBuf;
    }
  }
  doc["kind"] = "reading";
  doc["uptime_ms"] = uptimeMs;
  if (rowBootId > 0) {
    doc["boot_id"] = rowBootId;
  }
  if (rowSeq > 0) {
    doc["seq"] = rowSeq;
  }

  if (growStageBuf[0] != '\0') {
    doc["grow_stage"] = growStageBuf;
  }
  // Only what the row recorded: this runs on the network task, which must
  // not read the loop's live state (replayed rows never set live state on
  // the dashboard either).
  if (!isnan(waterTempCVal)) {
    doc["water_temp_c"] = waterTempCVal;
  } else {
    doc["water_temp_c"] = nullptr;
  }
  if (!isnan(ambientTempCVal)) {
    doc["ambient_temp_c"] = ambientTempCVal;
  } else {
    doc["ambient_temp_c"] = nullptr;
  }
  if (!isnan(ambientHumidityPctVal)) {
    doc["ambient_humidity_pct"] = ambientHumidityPctVal;
  } else {
    doc["ambient_humidity_pct"] = nullptr;
  }
  if (!isnan(lightAdcVal)) {
    doc["light_adc"] = lightAdcVal;
  } else {
    doc["light_adc"] = nullptr;
  }
  if (!isnan(waterLevelPctVal)) {
    doc["water_level_pct"] = waterLevelPctVal;
    doc["tank_volume_l"] = Tank::geomMlFromGrovePct(waterLevelPctVal) / 1000.0f;
  } else {
    doc["water_level_pct"] = nullptr;
    doc["tank_volume_l"] = nullptr;
  }
  if (!isnan(waterLevelRawPctVal)) {
    doc["water_level_raw_pct"] = waterLevelRawPctVal;
  }
  if (!isnan(waterTargetPctVal)) {
    doc["water_target_pct"] = waterTargetPctVal;
  }
  if (!isnan(waterMaxPctVal)) {
    doc["water_max_pct"] = waterMaxPctVal;
  }
  if (!isnan(tdsPpmVal)) {
    doc["tds_ppm"] = tdsPpmVal;
  } else {
    doc["tds_ppm"] = nullptr;
  }
  if (!isnan(nutrientTdsMinVal)) {
    doc["nutrient_tds_min"] = nutrientTdsMinVal;
  }
  if (!isnan(nutrientTdsMaxVal)) {
    doc["nutrient_tds_max"] = nutrientTdsMaxVal;
  }
  if (!isnan(phVoltageVal)) {
    doc["ph_voltage"] = phVoltageVal;
  }
  if (!isnan(phValueVal)) {
    doc["ph_raw"] = phValueVal;
    doc["ph"] = phValueVal;
  } else {
    doc["ph_raw"] = nullptr;
    doc["ph"] = nullptr;
  }
  if (!isnan(phFilteredVal)) {
    doc["ph_filtered"] = phFilteredVal;
  }
  if (!isnan(phEmaVal)) {
    doc["ph_ema"] = phEmaVal;
    doc["ph"] = phEmaVal;
  }
  if (!isnan(tdsPpmEmaVal)) {
    doc["tds_ppm_ema"] = tdsPpmEmaVal;
  }
  if (!isnan(co2PpmVal)) {
    doc["co2_ppm"] = co2PpmVal;
  } else {
    doc["co2_ppm"] = nullptr;
  }

  doc["nutrient_dose"] = nutrientDoseBuf;
  doc["ph_dose"] = phDoseBuf;

  if (!isnan(phTargetMinVal)) {
    doc["ph_target_min"] = phTargetMinVal;
  }
  if (!isnan(phTargetMaxVal)) {
    doc["ph_target_max"] = phTargetMaxVal;
  }
  if (waterPumpOn >= 0) {
    doc["water_pump_on"] = waterPumpOn == 1;
  }
  if (drainPumpOn >= 0) {
    doc["drain_pump_on"] = drainPumpOn == 1;
  }
  if (nutrientPumpOn >= 0) {
    doc["nutrient_pump_on"] = nutrientPumpOn == 1;
  }
  if (phUpPumpOn >= 0) {
    doc["ph_up_pump_on"] = phUpPumpOn == 1;
  }
  if (phDownPumpOn >= 0) {
    doc["ph_down_pump_on"] = phDownPumpOn == 1;
  }

  if (sourceBuf[0] != '\0') {
    doc["source"] = sourceBuf;
  }
  // Recorded while MQTT was down: history for the Pi, not the live state.
  doc["replay"] = true;

  static char payload[2048];  // network task only
  size_t len = serializeJson(doc, payload, sizeof(payload));
  if (len == 0 || len >= sizeof(payload)) {
    return -1;
  }

  return Comms::linkPublish(MQTT_TOPIC, payload, len, false) ? 1 : 0;
}

// One control event as JSON on MQTT_EVENT_TOPIC. Identity is
// (device_id, boot_id, event_seq). Dropped while MQTT is down: the event is
// still in /control_events.csv on the device and in Influx.
void publishControlEventToMqtt(const ControlEventLog::Row& row) {
  JsonDocument doc;
  doc["device_id"] = MQTT_DEVICE_ID;
  doc["kind"] = "control_event";
  doc["boot_id"] = bootId;
  doc["event_seq"] = controlEventSeq;
  if (row.recordedAtUtc != nullptr && row.recordedAtUtc[0] != '\0') {
    doc["timestamp"] = row.recordedAtUtc;
  }
  doc["uptime_ms"] = row.uptimeMs;
  doc["event"] = row.event;
  doc["stage"] = row.stage;
  auto putF = [&doc](const char* key, float v) {
    if (!isnan(v)) {
      doc[key] = v;
    }
  };
  if (row.currentLevelPct >= 0) {
    doc["current_level_pct"] = row.currentLevelPct;
  }
  putF("current_drawdown_mm", row.currentDrawdownMm);
  if (row.targetDepthMm >= 0) {
    doc["target_depth_mm"] = row.targetDepthMm;
  }
  putF("ec_before", row.ecBefore);
  putF("ec_dose_ml", row.ecDoseMl);
  putF("ec_after", row.ecAfter);
  putF("ph_before", row.phBefore);
  if (row.phDoseDir != nullptr && row.phDoseDir[0] != '\0') {
    doc["ph_dose_dir"] = row.phDoseDir;
  }
  putF("ph_dose_ml", row.phDoseMl);
  putF("ph_after", row.phAfter);
  putF("gain_up", row.gainUp);
  putF("gain_down", row.gainDown);
  putF("gain_ec", row.gainEc);
  putF("water_temp_c", row.waterTempC);
  putF("cumulative_session_ml", row.cumulativeSessionMl);
  if (row.alertFlags != nullptr && row.alertFlags[0] != '\0') {
    doc["alert_flags"] = row.alertFlags;
  }
  char out[COMMS_SMALL_PAYLOAD_MAX];
  const size_t len = serializeJson(doc, out, sizeof(out));
  if (len == 0 || len >= sizeof(out)) {
    return;
  }
  Comms::publish(MQTT_EVENT_TOPIC, out, len, false);
}

// Answer to {"cmd":"power",…}. The only message the node sends in standby, so
// the dashboard can tell a switched-off garden from one that is unreachable.
void publishPowerAck(bool ok, const char* message) {
  JsonDocument ack;
  ack["cmd"] = "power";
  ack["ok"] = ok;
  ack["state"] = Hmi::isSystemOn() ? "on" : "standby";
  ack["message"] = message;
  char out[256];
  const size_t len = serializeJson(ack, out, sizeof(out));
  if (len == 0 || len >= sizeof(out)) {
    return;
  }
  Comms::publish(MQTT_ACK_TOPIC, out, len, false);
}

#endif  // ENABLE_MQTT

