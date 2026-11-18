/************************************************************
 * Smart Hydroponic Garden — Sensor Monitoring
 *
 * Hardware:
 *   ESP-WROOM-32 (ESP32) microcontroller
 *   Water sensors:
 *   - SEN0169 pH sensor v1.1 (analog)
 *   - DS18B20 water temperature sensor
 *   - Grove TDS sensor v1.0 (analog)
 *   - Grove water level sensor (I2C)
 *   Ambient sensors:
 *   - DHT11 ambient temperature / humidity sensor
 *   - XENSIV™ PAS CO2 mini-board - CO2 sensor (I2C) - 5V version
 *   - LDR light sensor - photoresistor
 *   Pumps:
 *   - 4 peristaltic pumps (pH/nutrient = 500 ms bursts; water = 1 L top-up jug)
 *   Grow light:
 *   - WS2812B strip (108 LEDs, serpentine 6×18) on GPIO 27
 *
 * Structure (same folder — classic Arduino .h/.cpp split to keep Verify under
 *   low RAM; one giant .ino OOMs the compiler on ~3GB ChromeOS Linux VMs):
 *   embedded-smart-garden.ino  — setup() / loop() only
 *   garden.h                   — includes, pins, externs, prototypes
 *   garden_state.cpp           — globals + small helpers
 *   init.cpp / sensors.cpp / control_loops.cpp / pump_cal.cpp /
 *   serial_cmd.cpp / staged_control.cpp / csv_log.cpp /
 *   network.cpp / mqtt.cpp     — implementations
 *   comms.cpp                  — network task (WiFi, NTP, MQTT; README
 *                                "Data paths and firmware architecture")
 * Board: ESP32 Dev Module, Partition: "No OTA (2MB APP/2MB SPIFFS)"
 * Flash Mode: QIO, Flash Size: 4MB, Debug Level: None
 *
 * Pumps (servo, stopped at 90°):
 *   GPIO18 = Water refill
 *   GPIO19 = Nutrient
 *   GPIO23 = pH Up
 *   GPIO5  = pH Down (PCB D5; GPIO17 on the breadboard)
 *
 *   GPIO27 = WS2812B grow light data
 *
 * Ambient (daughterboard via JST-XH 7-way):
 *   Order: GND, 3.3 V, 5 V, SDA, SCL, DHT signal, LDR signal
 *   GPIO16 (RX2) = DHT11 data
 *   GPIO34 = LDR
 *   GPIO21/22 = I2C (CO2 on ambient board)
 *
 * pH: SEN0169 5 V out → 10 kΩ / 20 kΩ divider → GPIO 32 (gain 1.5)
 * TDS: Grove v1.0 SIG → GPIO 35 (kValue cal via ENTERTDS)
 *
 * I2C is on GPIO21 (SDA) and GPIO22 (SCL).
 *
 * Front panel (hmi.cpp; README "Front panel (HMI)"):
 *   GPIO13 = ON button, GPIO14 = OFF button (to GND, internal pull-up)
 *   GPIO25 = green POWER LED, GPIO26 = yellow RUNNING LED,
 *   GPIO33 = red ERROR LED
 *   Power-up leaves the garden in standby until ON is pressed: pumps held
 *   at STOP, the flash filesystem mounted and WiFi/MQTT up, nothing else
 *   started (no sensors, grow light or commands). OFF stops every pump and
 *   the grow light and returns to standby. MQTT {"cmd":"power",
 *   "action":"on|off|status"} works the same as the buttons. In standby the
 *   node sends only power acks, so the dashboard shows it as offline.
 ************************************************************/

#include "garden.h"

void setup() {
  // TX ring buffer so log lines and DUMP chunks queue instead of blocking
  // the loop (must be set before begin()).
  Serial.setTxBufferSize(SERIAL_TX_BUFFER_BYTES);
  Serial.setRxBufferSize(SERIAL_RX_BUFFER_BYTES);
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("[DEBUG] Smart Hydroponic Garden — sensor monitor starting");
  Serial.print("[DEBUG] Firmware version - ");
  Serial.println(FIRMWARE_VERSION);

  // Pumps get a defined STOP signal before anything else, then the panel.
  initPumps();
  Hmi::begin();

  // No stored calibrations, grow stage or journal without the filesystem:
  // ON is refused rather than dosing on defaults.
  if (!initLittleFS()) {
    Hmi::setCriticalFault("flash filesystem (LittleFS) failed to mount");
    return;
  }
  #if ENABLE_WIFI
  // WiFi, NTP and MQTT run on their own task from here on, in standby too, so
  // MQTT power commands can switch the garden on. Startup does not wait for
  // the network.
  Comms::begin();
  #endif
  #if ENABLE_MQTT
  // No outbox replay until ON: replayed readings would show the node online.
  Comms::setBulkHold(true);
  #endif
  Serial.println("[HMI] Standby — press ON (green button) or send MQTT power on");
}

// First ON after power-up: everything setup() leaves for ON.
static void startGarden() {
  Serial.print("[DEBUG] System mode - ");
  Serial.println(systemModeName());
  Serial.print("[DEBUG] Grow preset - ");
  Serial.println(activePreset().name);
  Serial.print("[DEBUG] Hydroponic mode - ");
  Serial.print(hydroponicModeName());
  Serial.print(" (pH re-dose wait ");
  printDurationMs(phSettleMs());
  Serial.println(")");
  Serial.print("[DEBUG] Chemistry control - ");
  Serial.println(CHEMISTRY_CONTROL_LAW);
  Serial.println("[DEBUG] Boot: sensor baseline capture (MODE PH_BASELINE when ready for pH control)");
  Serial.println("[DEBUG] EC dosing from VEGETATIVE; SEEDLING is pH + drawdown only");

#if !ENABLE_PUMPS
  Serial.println("[DEBUG] Auto pump control disabled (ENABLE_PUMPS=0); serial CAL still works");
#endif

  initSensors();
  PowerMonitor::begin();

  #if ENABLE_INFLUXDB
  InfluxCloud::firmwareVersion() = FIRMWARE_VERSION;
  InfluxCloud::printSetupHint();
  #endif
  #if !ENABLE_MQTT && !ENABLE_INFLUXDB
  Serial.println("[DEBUG] WiFi / cloud upload disabled");
  #elif !ENABLE_MQTT
  Serial.println("[DEBUG] MQTT disabled (ENABLE_MQTT=0) — Influx/Serial only");
  #endif

  #if ENABLE_CO2
  Serial.println("[DEBUG] CO2 enabled; starting sensor reads immediately");
  #else
  Serial.println("[DEBUG] CO2 disabled by configuration");
  #endif

  #if ENABLE_GROW_LIGHT
  GrowLight::init();
  #endif
}

static bool gardenStarted = false;

static void ackIfRemote(const char* message) {
  #if ENABLE_MQTT
  if (strcmp(Hmi::requestSource(), "mqtt") == 0) {
    publishPowerAck(true, message);
  }
  #else
  (void)message;
  #endif
}

static void powerOn() {
  if (!gardenStarted) {
    Serial.println("[HMI] ON — starting the garden");
    startGarden();
    gardenStarted = true;
  } else {
    Serial.println("[HMI] ON — resuming");
  }
  #if ENABLE_MQTT
  // The network is up in standby, so dashboard commands sent while off are
  // queued; drop them instead of running them late.
  const int stale = Comms::clearInbox();
  if (stale > 0) {
    Serial.print("[HMI] Dropped ");
    Serial.print(stale);
    Serial.println(" dashboard command(s) sent while the garden was off");
  }
  #endif
  // Same clean start as AUTO RESUME: the tank may have changed while off.
  armControlHolds();
  lightMakeupGardenOn();
  StageController::logEvent("power_on", buildSensorSnapshot(), "", NAN, NAN, NAN, NAN,
                            Hmi::requestSource(), currentIsoTimestamp().c_str());
  Hmi::setSystemOn(true);
  ackIfRemote("garden on");
}

static void powerOff() {
  Serial.println("[HMI] OFF — all pumps stopped, grow light off, standby");
  if (gardenStarted) {
    if (pumpCalibrationActive()) {
      stopCalPump(calPump);
      finishCalSession();
    }
    abortStagedDosingForOperator();
    stopAllDosingPumps();
    #if ENABLE_GROW_LIGHT
    GrowLight::switchOff();
    #endif
    lightMakeupGardenOff();
    StageController::logEvent("power_off", buildSensorSnapshot(), "", NAN, NAN, NAN, NAN,
                              Hmi::requestSource(), currentIsoTimestamp().c_str());
  } else {
    stopAllDosingPumps();
  }
  Hmi::setSystemOn(false);
  #if ENABLE_MQTT
  Comms::setBulkHold(true);
  #endif
  ackIfRemote("garden off, standby");
}

// Standby: Serial commands are not run (they can start pumps); dashboard
// commands queued meanwhile are dropped at ON (powerOn). Serial-link lines
// from the Pi still run, so a power-on can arrive over the USB cable.
static void serviceStandby(unsigned long now) {
  static unsigned long lastHintMs = 0;
  const bool got = serviceSerial(false);
  if (got && now - lastHintMs >= 2000) {
    lastHintMs = now;
    const char* why = Hmi::criticalFault();
    if (why != nullptr) {
      Serial.print("[HMI] Standby, critical fault: ");
      Serial.println(why);
    } else {
      Serial.println("[HMI] Standby — press ON (green button) or send MQTT power on; "
                     "Serial commands run once on");
    }
  }
}

void loop() {
  Hmi::service(millis());
  if (Hmi::takeOffRequest() && Hmi::isSystemOn()) {
    powerOff();
  }
  if (Hmi::takeOnRequest()) {
    powerOn();
  }
  if (!Hmi::isSystemOn()) {
    serviceStandby(millis());
    return;
  }

  serviceSerial(true);
  // Non-blocking DUMP: a few journal lines per pass while TX has room.
  serviceSerialDump();

  #if ENABLE_GROW_LIGHT
  // Photoperiod can wait during pump CAL so Serial stays quiet.
  if (!pumpCalibrationActive()) {
    GrowLight::setNtpValid(ntpEpochValid);
    GrowLight::update(millis());
    serviceLightMakeup(millis());
  }
  #endif

  // Calibration owns the board: no network, sensors, CSV, drain, or auto dosing.
  if (pumpCalibrationActive()) {
#if ENABLE_INFLUXDB
    InfluxCloud::setControlHold(true);
#endif
    updatePumpCalibration(millis());
    return;
  }

  #if ENABLE_MQTT
  // Dashboard commands received by the network task run here, on the loop
  // task, like Serial commands (one per pass).
  Comms::serviceInbox();
  #endif

  unsigned long now = millis();

  // Init: water level + optional jug pump only — no ambient/pH/TDS cycle.
  if (isWaterFillMode()) {
#if ENABLE_INFLUXDB
    InfluxCloud::setControlHold(true);
#endif
    serviceWaterFillMode(now);
    return;
  }

  // Passive baseline: still run full sensor + Influx cycle (not the FILL-only path).

  // Highest priority: keep roots in air. Service drain before other pumps and
  // before blocking network/CSV work.
  updateWaterDrainPump(now);
  enforceWaterLevelCeiling();
  controlWaterDrain(now);

  // Fast water-level-only poll so over-max is caught between sensor cycles
  // (non-blocking: one level sample per pass, sensor_acq.cpp).
  serviceWaterLevelPriorityPoll(now);

  now = millis();
  updateWaterRefillPump(now);
  updateManualPrime(now);
  updatePHControl(now);
  updateNutrientControl(now);
  if (!isSensorBaselineMode() && !softPrimeActive()) {
    serviceStagedControl(now);
  }
  enforceWaterLevelCeiling();
  controlWaterDrain(now);

  now = millis();
  const unsigned long sensorInterval =
      (PhProbeCal::isActive() || TdsProbeCal::isActive())
          ? SENSOR_READ_INTERVAL_PH_CAL_MS
          : SENSOR_READ_INTERVAL_MS;
  // The sensor cycle is non-blocking (sensor_acq.cpp): it starts when due and
  // advances one short step per pass, so the pump timers and the staged
  // controller keep running while it waits for conversions. The pH and TDS
  // bursts themselves wait for any running pump to stop.
  const bool sensorDue = now - lastSensorReadTime >= sensorInterval;
  if (!sensorCycleActive() && sensorDue) {
    lastSensorReadTime = now;

    Serial.println();
    Serial.println("[DEBUG] --- Sensor read cycle ---");
    Serial.print("[DEBUG] System mode - ");
    Serial.println(systemModeName());

    // Water level first: act on over-max before the slower reads.
    startSensorCycle(now);
  }
  if (sensorCycleActive() && serviceSensorCycle(millis())) {
    updateWaterDrainPump(millis());
    enforceWaterLevelCeiling();
    controlWaterDrain(millis());
    if (!isControlIdleMode()) {
      serviceStagedControl(millis());
    }
    printSensorData();
    // One reading, three outlets, no network I/O here:
    //  1. journal: append to /sensor_readings.csv (assigns boot_id + seq)
    //  2. MQTT: hand the JSON to the network task; if it cannot go live the
    //     journal line goes to the MQTT outbox and is replayed later
    //  3. Influx: queue the line for the uploader task
    char csvLine[SensorDataStore::CSV_LINE_MAX];
    const uint32_t seq = saveSensorDataToCSV(csvLine, sizeof(csvLine));
    #if ENABLE_MQTT
    publishTelemetryToMqtt(seq, csvLine);
    #endif
    #if ENABLE_INFLUXDB
    uploadTelemetryToInflux();
    #endif
    (void)seq;
    Serial.println("[DEBUG] --- Sensor read cycle complete ---");
  }

  const bool railBusy = softPrimeActive() || sharedRailPumpDrawing();
#if ENABLE_INFLUXDB
  // Hold uploads only while a motor draws on the shared rail. A pending CYCLE
  // no longer holds: startStagedAction() preempts before any pump starts, and
  // a request that could not be planned (held, settling) kept Influx paused.
  InfluxCloud::setControlHold(railBusy);
#endif
#if ENABLE_MQTT
  // Same rule for the MQTT outbox replay burst; live readings still go out.
  Comms::setBulkHold(railBusy);
#endif
  (void)railBusy;
}

