#pragma once
/*
 * garden.h — shared includes, config, extern globals, and function prototypes.
 *
 * The sketch .ino only has setup()/loop(). Implementations are in sibling
 * .cpp files in this folder (garden_state, init, sensors, control_loops, …).
 */

#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <pas-co2-ino.hpp>
#include <ESP32Servo.h>
#include <FS.h>
#include <LittleFS.h>
#include <DHT.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include "secrets.h"

// Network / cloud toggles (must be before influx_cloud.h include).
// MQTT on while the local dashboard/broker is up (Influx still primary cloud store).
#define ENABLE_MQTT 1
// 0 since 1.9.10: two panics on 7 Oct ~2 min after boot, when the first TLS
// uploads start; InfluxDB uploads were failing anyway and the Pi (MQTT ->
// SQLite) is the source of truth. Set to 1 to re-enable cloud uploads.
#define ENABLE_INFLUXDB 0
#define ENABLE_WIFI (ENABLE_MQTT || ENABLE_INFLUXDB)
// Boot idle until an operator sets a stage (dashboard / Serial MODE).
// Persisted stage in /grow_stage.txt survives power cycles.
#define SMART_GARDEN_BASELINE_CAPTURE_BOOT 0

#include "shared/sensor_data_store.h"
#include "shared/pump_calibration.h"
#include "shared/tank_geometry.h"
#include "shared/control_law.h"
#include "shared/grow_stage.h"
#include "shared/ph_control_loop.h"
#include "shared/ec_control_loop.h"
#include "shared/level_control_loop.h"
#include "shared/control_event_log.h"
#include "shared/influx_cloud.h"
#include "shared/analog_filter.h"
#include "shared/ph_probe_cal.h"
#include "shared/tds_probe_cal.h"
#include "shared/tds_nutrient_observer.h"
#include "shared/power_monitor.h"
// Every control event goes to Influx and MQTT through one fan-out (network.cpp).
void publishControlEvent(const ControlEventLog::Row& row);
#define SMART_GARDEN_EVENT_HOOK(row) publishControlEvent(row)
#include "shared/stage_controller.h"

#define ENABLE_GROW_LIGHT 1
#define GROW_LIGHT_DATA_PIN 27
#include "shared/grow_light.h"

// Arduino auto-generates prototypes before struct definitions — forward declare.
struct GrowPreset;

/************************************************************
 * Pin Assignment
 ************************************************************/

#define DS18B20_PIN 4
#define DHT11_PIN 16  // RX2 — moved off GPIO 15 (boot strap)

#define LDR_PIN 34
#define TDS_PIN 35
#define PH_PIN  32
// TDS probe power (optional). The Grove TDS board's AC excitation can couple
// into the pH probe in the same tank; the 18 Oct on/off test measured only
// -0.03 pH (-2 mV), so this is off. With it on, the board's VCC comes from this
// GPIO instead of the 3.3 V rail and is on only for the TDS read; pH is read
// with it off. GPIO 17 is free (pH-down on the breadboard, not routed on the
// PCB), not a strapping pin, and the board draws a few mA. 0 = TDS VCC still on the 3.3 V rail (no switching, no waits).
// Set 1 only once VCC is moved to GPIO 17 and off the 3.3 V rail: with both
// connected, driving GPIO 17 low shorts the rail.
#define TDS_POWER_SWITCHED 0
#define TDS_POWER_PIN 17
#if TDS_POWER_SWITCHED
#define TDS_POWER_WARMUP_MS 1000UL  // TDS on -> read (excitation and output settle)
#define TDS_POWER_QUIET_MS  2000UL  // TDS off -> pH read (pH input recovers)
#else
#define TDS_POWER_WARMUP_MS 0UL
#define TDS_POWER_QUIET_MS  0UL
#endif
#define DHTTYPE DHT11

#define PUMP1_PIN 18  // Water refill
#define PUMP2_PIN 19  // Nutrient
#define PUMP3_PIN 23  // pH Up
#define PUMP4_PIN 5   // pH Down — PCB D5 (was GPIO 17 on the breadboard)
// 1 = the pump has no working slot: its automatic doses are refused and the
// other loops carry on. pH-down (PCB D5, GPIO 5) was offline 15-19 Oct while
// the D5 trace / joint was broken; set PH_DOWN_PUMP_OFFLINE 1 again if it fails.
#define WATER_PUMP_OFFLINE 0
#define PH_DOWN_PUMP_OFFLINE 0

#define I2C_SDA 21
#define I2C_SCL 22

// Front panel (hmi.cpp). Buttons are momentary to GND, internal pull-up.
#define HMI_BTN_ON_PIN      13  // green ON button
#define HMI_BTN_OFF_PIN     14  // red OFF button
#define HMI_LED_POWER_PIN   25  // green: firmware running
#define HMI_LED_RUNNING_PIN 26  // yellow: a pump is drawing
#define HMI_LED_ERROR_PIN   33  // red: fault (alone = critical, ON refused)

const int SERVO_STOP = 90;

// Continuous-rotation servo settings. Reverse a run speed to 180 if that pump
// turns in the wrong direction.
const int WATER_REFILL_RUN_SPEED = 0;
const int NUTRIENT_RUN_SPEED = 0;
const int PH_UP_RUN_SPEED = 0;
const int PH_DOWN_RUN_SPEED = 0;

// Water pump is fill-only. Keep reverse drain off; water_drain_test.ino is
// diagnostic only.
#define ENABLE_TEMPORARY_REVERSE_DRAIN 0
const int WATER_DRAIN_RUN_SPEED = 180;  // opposite of WATER_REFILL_RUN_SPEED
const unsigned long WATER_DRAIN_INCREMENT_MS = 3000;  // few seconds per burst
const unsigned long WATER_DRAIN_SETTLE_MS = 5000;     // pause between bursts
// Fast water-only polls so safety refill does not wait on DHT/CO2 cycles.
const unsigned long WATER_LEVEL_PRIORITY_POLL_MS = 2000;

/************************************************************
 * Grow stage machine (Serial MODE / MQTT cmd / CONFIRM)
 *
 * FILLING      — manual fill; CONFIRM records fill_0 → PH_BASELINE
 * PH_BASELINE  — pH correction on plain water; ready_to_seed when stable
 * SEEDLING     — hold drawdown 0 mm; pH only; no EC dosing
 * TRANSITIONAL — vegetative control; EC = veg live band × 0.5; pH = veg
 * VEGETATIVE   — 50 mm drawdown; EC then pH
 * HARVEST      — 100 mm drawdown; EC then pH (gentler)
 ************************************************************/

#define systemMode (StageController::ctx().stage)

inline const char* systemModeName() {
  return GrowStage::name(systemMode);
}




/** Idle or sensor-baseline: sensors/MQTT only — no auto dosing / level top-up. */




/************************************************************
 * Legacy grow presets (telemetry labels — control uses grow_stage.h)
 ************************************************************/

struct GrowPreset {
  const char* name;
  float phMin;
  float phMax;
  int nutrientTdsMinPpm;
  int nutrientTdsMaxPpm;
};

extern const GrowPreset GROW_PRESETS[] ;


// DWC only — air stone is the aeration / mixing strategy going forward.
extern const char* HYDROPONIC_MODE_NAME ;

// Legacy ControlLaw path (#if 0 loops only). Live pH/EC use StageController +
// PhControl / EcControl: adaptive P, dead zone, online gain (see shared/*_control_loop.h).
extern const char* CHEMISTRY_CONTROL_LAW;

// Dose law for disabled legacy paths. P / PI / PID convert error → mL via
// control_law.h when those loops are re-enabled.
extern const ControlLaw::Mode ACTIVE_CONTROL_LAW ;

// Working volume for chemistry scaling: conservative ~6.55 L (see tank_geometry.h).
extern const float RESERVOIR_WORKING_VOLUME_ML ;

extern const ControlLaw::Gains WATER_CONTROL_GAINS ;
// Legacy ControlLaw path (if not THRESHOLD): mL per pH-error, min/max match grow_stage.
extern const ControlLaw::Gains PH_CONTROL_GAINS ;
extern const ControlLaw::Gains NUTRIENT_CONTROL_GAINS ;

// Dilute only when TDS is clearly high (not on every tick above max).
extern const float EC_DILUTE_OVERSHOOT_RATIO ;

// Chemistry: 500 ms bursts until the cup is readable, then replay and average
// as mL/burst. Water top-up jug: one continuous run (STOP or 8 s cap).
extern const unsigned long CAL_BURST_MS ;
extern const unsigned long CAL_BURST_PAUSE_MS ;
extern const int CAL_MAX_BURSTS ;
extern const int CAL_CONFIRM_RUNS ;
extern const unsigned long CAL_PRIME_MAX_MS ;
extern const unsigned long CAL_WATER_MAX_MS ;
extern const int CAL_FALLBACK_BURSTS ;
extern const int CAL_MAX_DOSE_BURSTS_PH;    // small tank: never dump many bursts at once;
extern const int CAL_MAX_DOSE_BURSTS_NUTRIENT ;
extern const unsigned long PUMP_MIN_RUN_MS ;
extern const unsigned long PUMP_MAX_RUN_MS_PH;
extern const unsigned long PUMP_MAX_RUN_MS_NUTRIENT;
extern const unsigned long PUMP_MAX_RUN_MS_WATER;





// Incremental actuation: short pump run, then settle before the next decision.
extern const unsigned long WATER_REFILL_INCREMENT_MS ;
extern const unsigned long WATER_LEVEL_SETTLE_MS ;

// The SEN0169 board is powered from 5 V. Its analogue output (0–5 V) goes
// through a discrete 10 kΩ (series) / 20 kΩ (to GND) divider; the ADC taps the
// node across the 20 kΩ so GPIO 32 sees ≤ ~3.33 V (ESP32-safe). Recover board
// volts: board_V = gpio_V × (10k+20k)/20k = gpio_V × 1.5. ADC_11db covers the
// full pin range.
extern const float PH_SIGNAL_LEVEL_SHIFT_GAIN;    // (10k+20k)/20k
// Alias kept for older comments / docs.
extern const float PH_SIGNAL_DIVIDER_GAIN ;

// pH = slope * boardVoltage + offset — LittleFS after two-point cal, else the
// baked 2026-09-16 breadboard line in shared/ph_probe_cal.h (ENTERPH overwrites).

// Fallback pulse when a pump is not calibrated or CONTROL_LAW is THRESHOLD.
extern const unsigned long PH_DOSE_INCREMENT_MS ;
extern const unsigned long MS_PER_MINUTE ;
extern const unsigned long MS_PER_HOUR ;

// DWC + air stone: wait for probe settle after each pH dose (no fresh-water chase).
extern const unsigned long PH_SETTLE_MS ;
extern const unsigned long PH_STUCK_PAUSE_MS ;
extern const int PH_MAX_DOWN_DOSES_PER_WINDOW ;
extern const unsigned long PH_DOWN_DOSE_WINDOW_MS ;
extern const int PH_MAX_DOWN_DOSES_PER_BOOT ;

// Safety limits — pumps are uncalibrated; without caps a mis-read or unmixed
// reservoir can empty a pH bottle (e.g. hundreds of mL into a small tank).
extern const int PH_STUCK_DOSE_LIMIT ;
extern const float PH_STUCK_MIN_IMPROVEMENT ;
// Do not auto-dose pH-down when this far above phMax — adjust manually first.
extern const float PH_AUTO_DOWN_MAX_DELTA ;







extern const unsigned long NUTRIENT_DOSE_INCREMENT_MS ;
extern const unsigned long NUTRIENT_DILUTE_INCREMENT_MS ;
extern const unsigned long NUTRIENT_SETTLE_MS ;

// Feature toggles
#define ENABLE_AMBIENT_SENSORS 1
// TEMPORARY — set back to 1 before unattended grow / auto control.
#define ENABLE_PUMPS 1
#define ENABLE_WATER_LEVEL 1
#define ENABLE_CO2 1
#define ENABLE_PH_TDS 1
#define ENABLE_DS18B20 1

// Firmware identity — Serial VERSION, MQTT telemetry "firmware_version".
#define FIRMWARE_VERSION "1.9.30"
#define FIRMWARE_VERSION_MAJOR 1
#define FIRMWARE_VERSION_MINOR 9

/************************************************************
 * WiFi / MQTT (Raspberry Pi dashboard)
 *
 * Fill in WiFi profiles and MQTT host in secrets.h, then flash.
 * Supports home WPA2-PSK and UniMelb UniWireless (WPA2-Enterprise PEAP).
 * Topic matches the dashboard subscription: smart-garden/+/telemetry
 ************************************************************/
extern const uint16_t MQTT_BROKER_PORT ;
extern const char* MQTT_DEVICE_ID ;
extern const char* MQTT_TOPIC ;
extern const char* MQTT_ALERT_TOPIC ;
extern const char* MQTT_CMD_TOPIC ;
extern const char* MQTT_ACK_TOPIC ;
extern const char* MQTT_EVENT_TOPIC ;
extern const char* MQTT_CLIENT_ID ;
extern const unsigned long WIFI_CONNECT_TIMEOUT_MS ;
// 802.1X (UniWireless) often needs longer than home PSK.
extern const unsigned long WIFI_ENTERPRISE_CONNECT_TIMEOUT_MS ;
// Telemetry JSON + PubSubClient buffer. The payload was ~4.6 kB with
// health/sched; the health messages grew it past 6144 on 18 Oct (6167 B), and
// every full snapshot was dropped. If one is still too large, the long health
// text is left out so the readings, faults and alerts still go out.
constexpr size_t MQTT_TELEMETRY_MAX_BYTES = 8192;
extern const uint16_t MQTT_SOCKET_TIMEOUT_SECONDS ;
// The network task services the keepalive every COMMS_TASK_PERIOD_MS, so this
// no longer has to cover blocking sensor reads on the loop task.
extern const uint16_t MQTT_KEEPALIVE_SECONDS ;

/************************************************************
 * Network task (comms.cpp) — WiFi, NTP and MQTT run on core 0 so a slow
 * join, DNS lookup or broker connect never stalls pumps or sensors.
 ************************************************************/
// WiFi rejoin pacing while it is down.
constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 30000;
// MQTT reconnect backoff: first retry after MIN, doubling to MAX. Serial
// MQTT CONNECT tries at once. (Before 1.8.0 MQTT never reconnected by itself.)
// The backoff restarts at MIN when WiFi rejoins. MAX was 10 min until 1.9.25;
// an attempt only costs the network task a few seconds, so 1 min is enough.
constexpr unsigned long MQTT_RECONNECT_MIN_MS = 15000;
constexpr unsigned long MQTT_RECONNECT_MAX_MS = 60000;
// Tried when the configured broker does not answer: the Pi's mDNS name, so a
// new DHCP address after a Pi reboot does not cut the link. "" turns it off.
constexpr const char* MQTT_BROKER_MDNS_HOST = "smar-gardon.local";
// Re-read SNTP time into the millis() mapping this often (crystal drift).
constexpr unsigned long NTP_RELATCH_INTERVAL_MS = 6UL * 3600UL * 1000UL;
constexpr unsigned long COMMS_TASK_PERIOD_MS = 20;
constexpr uint32_t COMMS_TASK_STACK = 12288;
constexpr UBaseType_t COMMS_TASK_PRIORITY = 2;
// Alerts, acks and control events (each well under 1 kB).
constexpr size_t COMMS_SMALL_PAYLOAD_MAX = 896;
constexpr UBaseType_t COMMS_SMALL_QUEUE_DEPTH = 6;
// Dashboard commands waiting for the loop task.
constexpr size_t COMMS_INBOX_PAYLOAD_MAX = 768;
constexpr UBaseType_t COMMS_INBOX_DEPTH = 4;
// Readings the live publish missed, replayed from LittleFS (about 2.5 h).
constexpr size_t COMMS_MQTT_OUTBOX_MAX_BYTES = 256 * 1024;
constexpr int COMMS_REPLAY_BATCH = 5;
// Serial link to the Pi over the USB cable (scripts/serial_mqtt_bridge.py in
// the dashboard repo). The bridge sends "@LINK <epoch>" every few seconds.
// While those arrive and MQTT is down, publishes go out on Serial as
// "@MQ <r|-> <topic> <payload>" lines, and "@CMD <json>" lines run like
// dashboard commands.
constexpr unsigned long SERIAL_LINK_TIMEOUT_MS = 15000;
// A WiFi join holds the network task for up to 65 s, so with the serial
// link up it is tried this rarely.
constexpr unsigned long WIFI_RETRY_WITH_SERIAL_LINK_MS = 10UL * 60UL * 1000UL;
constexpr size_t SERIAL_LINK_LINE_MAX = COMMS_INBOX_PAYLOAD_MAX + 16;
constexpr unsigned long COMMS_REPLAY_INTERVAL_MS = 250;

extern const char* CSV_PATH ;
extern const char* CSV_HEADER ;
// Previous journal, kept when CSV_PATH reaches CSV_MAX_BYTES or the column
// layout changes (DUMP PREV). Removed first when flash runs low.
extern const char* CSV_PREV_PATH ;
// SQLite-ready LittleFS CSV journal (ISO-8601 UTC, empty=NULL, 0/1 flags):
// every reading, whether or not any uplink took it. Two files of
// CSV_MAX_BYTES (current + previous) hold about 1.5 days at 10 s.
extern const size_t CSV_MAX_BYTES ;
extern const size_t CSV_MIN_FREE_BYTES ;

// Longest a due sensor cycle waits for a running pump to stop. A sensor cycle
// blocks for several seconds (DHT11 retries), and a pump left running through
// it overruns its dose (the 1.6.0 overruns). This must exceed the longest
// automatic dose: CAL_MAX_DOSE_BURSTS_NUTRIENT (85) x 500 ms = 42.5 s, plus one
// sensor interval.
constexpr unsigned long SENSOR_READ_MAX_DEFER_MS = 60000;

// Reading identity: persistent boot id + per-boot sequence. Carried in the
// CSV journal, MQTT (live and replay) and Influx so receivers de-duplicate.
extern uint32_t bootId;
extern uint32_t readingSeq;
extern uint32_t controlEventSeq;

/************************************************************
 * Sensor Objects
 ************************************************************/

extern OneWire oneWire;
extern DallasTemperature ds18b20;
extern DHT dht;
extern PASCO2Ino co2Sensor;

extern Servo pump1;
extern Servo pump2;
extern Servo pump3;
extern Servo pump4;

/************************************************************
 * Water Level Sensor I2C Addresses
 ************************************************************/

#define WATER_LOW_ADDR  0x77
#define WATER_HIGH_ADDR 0x78

extern unsigned char low_data[8];
extern unsigned char high_data[12];

#define NO_TOUCH              0xFE
#define WATER_TOUCH_THRESHOLD 100

/************************************************************
 * Averaging Settings
 ************************************************************/

extern const int ANALOG_SAMPLE_COUNT    ;
extern const int WATER_LEVEL_SAMPLE_COUNT ;

// Water-level trust/persistence lives in shared/sensor_data_store.h so
// diagnostic sketches update the same LittleFS files as this sketch.

// PAS CO2 single-shot (Infineon Arduino API):
//   startMeasure();
//   do { getCO2(ppm); } while (ppm == 0);
// get_result only fills ppm when MEAS_STS.DRDY is set; otherwise ppm stays 0
// (or getCO2 returns READ_NRDY — treat as not-ready and keep polling).
extern const unsigned long SENSOR_READ_INTERVAL_MS ;
// Faster telemetry while a probe calibration wizard is open (pH or TDS).
extern const unsigned long SENSOR_READ_INTERVAL_PH_CAL_MS ;
extern const unsigned long CO2_SINGLE_SHOT_TIMEOUT_MS ;
extern const unsigned long CO2_SINGLE_SHOT_POLL_MS ;
// Gen1 range to 10k; Gen1.5 can report higher — keep headroom under uint16.
extern const uint16_t CO2_MAX_PPM ;

/************************************************************
 * Latest Sensor Values
 ************************************************************/

extern float waterTempC      ;
extern int   ldrADC          ;
extern int   tdsADC          ;
extern int   tdsPPM          ;
extern int   waterLevelPercent;    // unknown until a trusted sample;
extern int   waterLevelRawPercent ;
extern SensorDataStore::WaterLevelTrust waterLevelTrust;
extern bool  waterLevelOverMaxLatched ;
// Post-FILL Grove stick reading 0% is almost always a sensor fault (powered
// while submerged). No software reset — lift/dry/power-cycle while dry.
extern bool  waterLevelSensorFault ;
extern uint8_t waterLevelZeroStreak ;
extern const uint8_t WATER_LEVEL_FAULT_ZERO_STREAK ;
extern unsigned long lastWaterLevelPriorityPollMs ;
extern int   co2PPM          ;
extern float phVoltage       ;
extern float phValue;     // raw (averaged ADC → pH);
extern float phValueFiltered;     // outlier-rejected multi-sample pH;
extern float phValueEma;          // 3 min EMA of phValueFiltered; control input
extern float tdsPpmEma;           // 3 min EMA of tdsPPM; control input
extern float ambientTempC    ;
extern float ambientHumidityPct ;

extern unsigned long lastSensorReadTime ;
extern bool csvHeaderWritten ;
extern bool waterRefillPumpRunning ;
extern unsigned long waterRefillStartedAt ;
extern unsigned long waterRefillLastRunMs ;
extern unsigned long waterLevelControlAllowedAt ;
// Legacy flag — MODE FILL is hand-fill only; jug pump is not started here.
extern bool waterFillManualPumpRunning ;
extern unsigned long waterFillManualStartedAt ;
extern unsigned long lastWaterFillStatusMs ;
extern const unsigned long WATER_FILL_STATUS_MS ;
// TEMPORARY — paired with ENABLE_TEMPORARY_REVERSE_DRAIN; delete with that flag.
extern bool waterDrainPumpRunning ;
extern unsigned long waterDrainStartedAt ;
extern unsigned long waterDrainControlAllowedAt ;
enum PHControlState {
  PH_CONTROL_IDLE,
  PH_DOSING_UP,
  PH_DOSING_DOWN,
  PH_MIXING
};
extern PHControlState phControlState ;
extern unsigned long phControlStateStartedAt ;
extern unsigned long phControlAllowedAt ;
extern unsigned long phControlPausedUntil ;
extern float phReadingBeforeLastDownDose ;
extern int phDownDosesWithoutImprovement ;
extern unsigned long phDownDoseWindowStartedAt ;
extern int phDownDosesThisWindow ;
extern int phDownDosesThisBoot ;
extern bool phDownAutoDisabled ;
// Most pH samples outside 0..14 (probe unpowered / 5 V rail off). The
// reading is dropped, the EMA restarts its warm-up, and pH dosing holds.
extern bool phSensorFault;
extern bool tdsSensorFault;  // TDS reads ~0 ppm: probe dry or unplugged

enum NutrientControlState {
  NUTRIENT_CONTROL_IDLE,
  NUTRIENT_DOSING,
  NUTRIENT_DILUTING,
  NUTRIENT_MIXING
};
extern NutrientControlState nutrientControlState ;
extern unsigned long nutrientControlStateStartedAt ;
extern unsigned long nutrientControlAllowedAt ;

extern unsigned long activeWaterRefillMs ;
extern unsigned long activePhDoseMs ;
extern unsigned long activeNutrientDoseMs ;
extern unsigned long activeNutrientDiluteMs ;
extern bool waterSafetyRefillActive ;
extern int phBurstTarget ;
extern int phBurstDone ;
extern bool phBurstOn ;
extern unsigned long phBurstMark ;
extern int nutrientBurstTarget ;
extern int nutrientBurstDone ;
extern bool nutrientBurstOn ;
extern unsigned long nutrientBurstMark ;

extern ControlLaw::State waterPidState;
extern ControlLaw::State phPidState;
extern ControlLaw::State nutrientPidState;

enum CalPhase {
  CAL_IDLE,
  CAL_WAIT_READY,
  CAL_PRIMING,
  CAL_RUNNING,
  CAL_WAIT_VOLUME
};
extern CalPhase calPhase ;
extern PumpCal::Id calPump ;
extern bool calSequence ;
extern int calSequenceIndex ;
extern unsigned long calPulseMs ;
extern unsigned long calRunStartedAt ;
extern int calBurstsDone ;
extern int calTargetBursts ;
extern unsigned long calTargetMs ;
extern bool calBurstOn ;
extern bool calStopRequested ;
extern float calRateSum ;
extern int calRateCount ;
extern float calLastSample ;
extern const char* calMqttMessage ;
extern bool calMqttOk ;
extern String serialLineAcc;

// Soft prime: fill dry tubing while sensors/Influx keep running.
// Auto pH/EC/level dosing is paused until STOP (or safety timeout).
extern bool manualPrimeActive ;
extern PumpCal::Id manualPrimePump ;
extern unsigned long manualPrimeStartedAt;
// Operator hold: pause auto level/EC/pH while keeping the current grow stage
// (used for open-loop bump tests). Soft-prime also pauses auto while running.
extern bool autoControlPaused;

extern int netPotTouchPercent;

#if ENABLE_WIFI
// Written by the network task (comms.cpp), read by the loop task. Read the
// millis-to-epoch mapping through ntpSnapshot(): the task re-latches it every
// few hours and a torn read would mis-stamp a row by hours.
extern volatile bool ntpTimeSynced ;
extern volatile unsigned long millisAtNtpSyncMs ;
extern volatile uint32_t ntpEpochSecondsAtSync ;
extern volatile bool ntpEpochValid ;
struct NtpMap {
  bool valid;
  uint32_t epochAtSync;
  unsigned long millisAtSync;
};
NtpMap ntpSnapshot();
// ISO-8601 UTC for a millis() value from this boot; empty before NTP.
bool recordedAtUtcForUptime(unsigned long uptimeMs, char* buf, size_t buflen);
#endif

#if ENABLE_MQTT
extern WiFiClient wifiClient;
extern PubSubClient mqttClient;
#endif

// Staged dosing flags (defined in staged_control.cpp).
extern bool stagedWaterTopUp;
extern bool stagedEcDose;
extern bool stagedPhDose;

/* ---- helpers (garden_state.cpp) ---- */
bool isWaterFillMode();
bool isPrimingPhMode();
bool isSensorBaselineMode();
bool isControlIdleMode();
bool phControlAllowedByMode();
bool nutrientsAllowedByMode();
bool waterAutoControlAllowedByMode();
const GrowPreset& activePreset();
int fill0Percent();
int waterCeilingPercent();
int waterSafetyFloorPercent();
int waterRefillToPercent();
int waterStageTargetPercent();
const char* hydroponicModeName();
unsigned long phSettleMs();
unsigned long phStuckPauseMs();
int phMaxDownDosesPerWindow();
unsigned long phDownDoseWindowMs();
int phMaxDownDosesPerBoot();
void printDurationMs(unsigned long ms);

/* ---- function prototypes (implemented in *.cpp) ---- */
void initPumps();
bool initLittleFS();  // false if the flash filesystem cannot be mounted
// Settle holds before the first automatic dose (boot and panel ON).
void armControlHolds();
void initSensors();

bool pumpCalibrationActive();
bool softPrimeActive();
void stopAllDosingPumps();
void startCalPump(PumpCal::Id id);
void stopCalPump(PumpCal::Id id);
void printSerialHelp(bool verbose);
// Serial DUMP (non-blocking, framed; see serial_cmd.cpp).
// 12 kB holds a whole serial-link telemetry frame (up to 8 kB), so writing
// one does not hold the UART lock while the loop task waits to log.
constexpr size_t SERIAL_TX_BUFFER_BYTES = 12288;
// Room for one "@CMD" line from the Pi while the loop task is busy.
constexpr size_t SERIAL_RX_BUFFER_BYTES = 1024;
constexpr int SERIAL_DUMP_LINES_PER_PASS = 6;
void startSerialDump(const char* path, bool raw, uint32_t fromLine);
void serviceSerialDump();
void stopSerialDump();
bool calIsContinuous();
const char* calPhaseName();
void setCalMqttStatus(const char* msg, bool ok = true);
void resetCalPumpStats();
void promptCalReady();
void beginCalSequence(bool allPumps, PumpCal::Id only);
void finishCalSession();
void advanceCalSequence();
void startCalBurstOn(unsigned long now);
void startCalBurstRun();
void finishCalMeasure();
void startPrime();
void finishPrime(const char* reason);
void updatePumpCalibration(unsigned long now);
bool parseVolumeMl(const String& line, float& outMl);
void acceptCalVolume(float volumeMl);

void printSystemModeBanner();
void stopWaterFillManualPump(const char* reason);
bool setSystemMode(GrowStage::Id next, const char* via);
bool parseSystemModeToken(const String& token, GrowStage::Id& out);
void recordBaselineFill0();
void logBaselineCaptureEvent(const char* eventName);
void startBaselineCapture();
void endBaselineCapture();
void confirmWaterFillTouch();
void startWaterFillManualPump();
void serviceWaterFillMode(unsigned long now);
void handleSerialLine(String line);
// Reads Serial lines. '@' lines (Pi serial bridge) run in standby too; other
// lines run only with the garden on. Returns true if one was ignored (standby).
bool serviceSerial(bool systemOn);
unsigned long plannedRunMs(PumpCal::Id id, float doseMl, unsigned long fallbackMs,
                           unsigned long maxMs);
int plannedBursts(PumpCal::Id id, float doseMl, int fallbackBursts, int maxBursts);
unsigned long plannedDoseMs(PumpCal::Id id, float doseMl, int maxBursts);
bool reservoirAllows(PumpCal::Id id, float doseMl, const char* action);
float waterDoseMl(float errorPercent);
float phDoseMl(float errorPh);
float nutrientDoseMl(float errorPpm);

void abortStagedDosingForOperator();
void startManualPrime(PumpCal::Id id);
void finishManualPrime(const char* reason, bool announceResume);
void updateManualPrime(unsigned long now);
bool stagedPumpBusy();
void setAutoControlPaused(bool paused, const char* via);
void loadAutoControlPaused();  // after LittleFS mount
bool startOperatorDose(PumpCal::Id id, float doseMl, const char* via);
bool sharedRailPumpDrawing();
void readPhSensorValues();
float readPhFilteredAverage();
StageController::Sensors buildSensorSnapshot();
void resetDosingSession(bool ph, bool ec, const char* via);  // operator override of the cap
bool startStagedAction(const StageController::Action& action, unsigned long now);
bool startStagedAction(const StageController::Action& action, unsigned long now,
                       int maxBurstsOverride);
void onStagedWaterComplete(unsigned long now);
// Refill target level (%) while a staged top-up runs, else -1 (live cutoff).
int stagedTopUpStopPercent();
int waterLevelSmoothedPercent();   // median of the last 30 s of stick readings, -1 if none
int waterLevelSpreadPercent();     // highest minus lowest stick reading over 60 s
// Operator go-ahead for a refill on hold (Serial REFILL PROCEED, MQTT refill_proceed).
bool proceedRefill(const char* via);
// Operator pause of one control loop ("level", "nutrient", "ph"); the others run on.
bool setLoopPaused(const char* loop, bool paused, const char* via);
void onStagedEcComplete(unsigned long now, float deliveredMl);
void onStagedPhComplete(unsigned long now, float deliveredMl);

// Hardware-timed dose stop (staged_control.cpp). The loop books the dose once
// the timer has fired; it stops the pump itself only if the timer is more than
// DOSE_TIMER_GRACE_MS late.
constexpr unsigned long DOSE_TIMER_GRACE_MS = 250;
void armDoseStopTimer(int pumpNumber, unsigned long runMs);
bool doseStopTimerFired();

// Non-blocking sensor acquisition (sensor_acq.cpp). One short step per loop
// pass; no delay() anywhere in the cycle, and the pH/TDS bursts never run
// while a pump draws on the shared rail.
bool sensorCycleActive();
void startSensorCycle(unsigned long now);
bool serviceSensorCycle(unsigned long now);  // true on the pass the cycle completes
void serviceWaterLevelPriorityPoll(unsigned long now);
void serviceStagedControl(unsigned long now);

// Solution monitor (solution.cpp): when the nutrient solution needs changing.
void loadSolutionMonitor();                  // after LittleFS mount
void resetSolutionMonitor(const char* via);  // water changed
void solutionOnTopUp(float deliveredMl);
void serviceSolutionMonitor(unsigned long now);
void addSolutionJson(JsonDocument& doc, JsonArray alerts);
void printSolutionStatus();

// Grow-light make-up (light_makeup.cpp): scheduled light missed while the
// garden was off, repaid after the scheduled OFF once an operator starts it.
void loadLightMakeup();                  // after LittleFS mount
void lightMakeupGardenOn();              // panel ON: count the gap once the clock is set
void lightMakeupGardenOff();             // panel OFF: save when the light went off
void serviceLightMakeup(unsigned long now);
// action "on" | "off" | "clear". Fills msg (Serial / MQTT ack); false if refused.
bool lightMakeupCommand(const char* action, const char* via, char* msg, size_t msgLen);
// Light on now for hours (0: the owed make-up light; < 0: off, back to the schedule).
bool lightOnCommand(float hours, const char* via, char* msg, size_t msgLen);

bool waterAdditionAllowed();
bool chemistryBusy();
void enforceWaterLevelCeiling();
bool pump1ForwardBusy();
bool waterPumpBusy();
bool reverseDrainModeActive();
void consumeActivePhBursts();
void pausePHDosingForReverseDrain(unsigned long now);
void controlWaterDrain(unsigned long now);
#if 0  // legacy — still present in control_loops.cpp under #if 0
void controlWaterLevel(int levelPercent, unsigned long now);
bool allowPHDownDose(float currentPH, float phMax, unsigned long now);
void controlPH(float currentPH, unsigned long now);
void controlNutrients(int currentTdsPpm, unsigned long now);
#endif
void updatePHControl(unsigned long now);
void updateNutrientControl(unsigned long now);
void updateWaterRefillPump(unsigned long now);
void updateWaterDrainPump(unsigned long now);

void scanI2CDevices();
// I2C diagnostics (sensors.cpp): Wire error names, idle SDA/SCL levels, a
// rate-limited failure line per device, and SCL-clocking bus recovery.
#define CO2_I2C_ADDR 0x28
constexpr unsigned long I2C_FAIL_LOG_MS = 60000UL;
constexpr unsigned long CO2_REPROBE_MS = 30000UL;
struct I2cFailLog {
  uint8_t lastCode = 0;
  unsigned long lastMs = 0;
};
const char* i2cErrorName(uint8_t code);
const char* i2cLineState();
void logI2cFailure(I2cFailLog& f, const char* name, uint8_t addr, uint8_t code);
void logI2cRecovered(I2cFailLog& f, const char* name, uint8_t addr);
void i2cRecoverBus();
bool co2Init();         // begin(); sets the ready flag
bool co2SensorReady();  // false while the CO2 is off the bus (re-probed every 30 s)
// Short, non-waiting pieces of the sensor cycle (sensors.cpp).
void readLdrSensor();
void readTdsSensor();
// TDS probe power (sensors.cpp). With TDS_POWER_SWITCHED 0 these do nothing
// and the probe is always on.
void initTdsPower();
void setTdsPower(bool on);
bool tdsPowerOn();
// True while the TDS probe is on or was switched off less than
// TDS_POWER_QUIET_MS ago: a pH read now would pick up its excitation.
bool tdsExcitationActive();
// Blocking TDS read for the calibration commands: power on, wait, read, off.
float readTdsGpioVoltsPowered(int samples);
int readTdsAdcPowered();
// Blocking: switch the TDS probe off and wait out TDS_POWER_QUIET_MS (pH capture).
void waitForTdsQuiet();
void printAnalogSensors();
void ds18b20StartConversion();
float ds18b20ReadConversion();      // NAN if invalid
bool readAmbientDHT11Once(int attempt);
enum Co2Poll { CO2_POLL_WAIT, CO2_POLL_READY, CO2_POLL_COMM_RETRY, CO2_POLL_ERROR };
bool co2StartSingleShot();
Co2Poll co2PollReady();
int co2ReadPpm();                   // ppm, -1 bad/error, -2 I2C error worth a retry
void printCo2Timeout(int commFails);
void applyWaterLevelReading(int rawPercent, bool verbose);
void updateWaterLevelReading(bool verbose);  // blocking (FILL mode / calibration only)
bool isWaterLevelSensorFault();
// True from boot until the Grove stick is lifted and lowered back in and
// reads a non-zero level again. Readings are ignored meanwhile.
bool isWaterLevelInitRequired();
// Dashboard status: "health" (per sensor/actuator state + message) and
// "sched" (next read/check times and dose thresholds). See health.cpp.
void addHealthJson(JsonDocument& doc);
int waterLevelPercentForControl();
void updateWaterLevelSensorFault(int rawPercent);
int readAnalogAverage(int pin);
float readPHBoardVoltageAverage();
float convertVoltageToPH(float voltage);
unsigned long phWarmupElapsedS();   // 60 once the pH EMA is seeded
unsigned long tdsWarmupElapsedS();  // 60 once the TDS EMA is seeded
float readDS18B20Average();
int readWaterLevelAverage();
int readWaterLevelPercent();
int convertTDSToPPM(int adc);
void printSensorData();
void printPlotterData();

const char* phDoseLabel();
const char* nutrientDoseLabel();
void ensureCsvSchema();
void rotateCsvIfNeeded();
uint32_t nextReadingSeq();
// Journal one reading (CSV_PATH) and return its seq. lineOut receives the
// same CSV line for the MQTT outbox.
uint32_t saveSensorDataToCSV(char* lineOut = nullptr, size_t lineOutLen = 0);

#if ENABLE_WIFI
void syncNtpTime();
bool wifiProfileConfigured(const WifiNetworkProfile& net);
bool waitForWifiConnected(unsigned long timeoutMs);
bool beginWifiProfile(const WifiNetworkProfile& net);
bool connectWifi();
String currentIsoTimestamp();

namespace Comms {
struct Stats {
  unsigned long livePublished = 0;
  unsigned long liveFailed = 0;
  unsigned long parked = 0;          // readings moved to the outbox
  unsigned long outboxRejected = 0;  // outbox full or write failed
  unsigned long replayed = 0;
  unsigned long replayBadRows = 0;
  unsigned long statusDropped = 0;
  unsigned long smallPublished = 0;
  unsigned long smallDropped = 0;
  unsigned long inboxDropped = 0;
  unsigned long mqttAttempts = 0;
  unsigned long mqttConnects = 0;
  unsigned long mqttDrops = 0;
  unsigned long wifiAttempts = 0;
};
// setup(): start the network task (first WiFi join happens inside it).
void begin();
// Loop task. Copies a telemetry document into a send slot and returns.
// seq > 0 marks a sensor reading: if it cannot go live it is kept in the
// MQTT outbox (csvLine) and replayed. seq == 0 is a status snapshot.
bool submitTelemetry(const JsonDocument& doc, uint32_t seq, const char* csvLine);
// Loop task. Small message (alert, ack, event); dropped if MQTT is down.
bool publish(const char* topic, const char* payload, size_t len, bool retained);
// Loop task. Runs at most one dashboard command received by the network task.
void serviceInbox();
// Loop task, panel ON: drop commands that arrived while the garden was off
// so a stale dose or stage change does not run late. Returns the count.
int clearInbox();
void requestConnect(const char* via);
void setCalPause(bool paused);
// True while a motor draws on the shared rail: outbox replay waits.
void setBulkHold(bool hold);
bool mqttConnected();
bool wifiConnected();
bool calPauseActive();
// MQTT or the serial link can carry a publish.
bool linkUp();
bool serialLinkActive();
// Loop task. A line starting with '@' from the Pi serial bridge.
void handleSerialLinkLine(const char* line);
// Network task only: publish on the link chosen for this pass.
bool linkPublish(const char* topic, const char* payload, size_t len, bool retained);
const Stats& stats();
size_t outboxPendingBytes();
void printStatus();
}  // namespace Comms
#endif

#if ENABLE_INFLUXDB
bool uploadTelemetryToInflux();
void influxUploadControlEventHook(const ControlEventLog::Row& row);
#endif

#if ENABLE_MQTT
const char* mqttStateLabel(int state);
bool connectMqtt();
// Broker for the joined WiFi network (secrets.h mqttBrokerHost, else
// MQTT_BROKER_HOST).
const char* mqttBrokerHost();
void onMqttCommand(char* topic, byte* payload, unsigned int length);
// Live reading (seq > 0, with its CSV line) or a status snapshot (seq 0).
bool publishTelemetryToMqtt(uint32_t seq = 0, const char* csvLine = nullptr);
bool publishAlertsToMqtt(const char* calAlert, const char* stockAlert, bool calNeeded,
                         bool waterLevelFault);
// Network task. 1 = sent, 0 = link failed (retry), -1 = unparseable row.
int publishCsvRowToMqtt(const char* csvLine);
void publishControlEventToMqtt(const ControlEventLog::Row& row);
// {"cmd":"power","ok":…,"state":"on|standby","message":…} on the ack topic.
void publishPowerAck(bool ok, const char* message);
#endif

// Front panel soft switch and status LEDs (hmi.cpp).
namespace Hmi {
void begin();                    // pins, LEDs off, stuck-button check
void service(unsigned long now); // buttons + LEDs, every loop pass
bool takeOnRequest();            // ON pressed (and no critical fault)
bool takeOffRequest();           // OFF pressed
// Network task: MQTT {"cmd":"power","action":"on|off|status"}. Runs on the
// next service() pass, with the same critical-fault check as the ON button.
void requestRemote(const char* action);
const char* requestSource();     // "panel_button" or "mqtt", for the last request
void setSystemOn(bool on);
bool isSystemOn();
void setCriticalFault(const char* reason);  // nullptr clears
const char* criticalFault();     // nullptr when ON is allowed
}  // namespace Hmi
