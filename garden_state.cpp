/* Global variables, hardware objects, and small mode helpers. */
#include "garden.h"

const GrowPreset GROW_PRESETS[] = {
  // Labels only; live bands come from GrowStage::config() via activePreset().
  { "seedling",   5.5f, 5.8f,    0,    0 },
  { "vegetative", 5.5f, 5.8f,  800, 1200 },
  { "harvest",    5.5f, 5.8f,    0,  200 }
};
const char* HYDROPONIC_MODE_NAME = "dwc";
const char* CHEMISTRY_CONTROL_LAW = "scheduled_P";  // fixed tuned gains scheduled on V, TDS
const ControlLaw::Mode ACTIVE_CONTROL_LAW = ControlLaw::THRESHOLD;
// Conservative working volume ≈ Tank::conservativeWorkingMl() (7.28 L fill × 0.9).
const float RESERVOIR_WORKING_VOLUME_ML = 6550.0f;
const ControlLaw::Gains WATER_CONTROL_GAINS = { 4.0f, 0.05f, 0.0f, 2.0f, 20.0f };
const ControlLaw::Gains PH_CONTROL_GAINS = { 0.12f, 0.0f, 0.0f, 0.05f, 0.15f };
const ControlLaw::Gains NUTRIENT_CONTROL_GAINS = { 0.0005f, 0.0f, 0.0f, 0.1f, 0.5f };
const float EC_DILUTE_OVERSHOOT_RATIO = 1.10f;
const unsigned long CAL_BURST_MS = PumpCal::BURST_MS;
const unsigned long CAL_BURST_PAUSE_MS = 400;
const int CAL_MAX_BURSTS = 20;
const int CAL_CONFIRM_RUNS = 3;
const unsigned long CAL_PRIME_MAX_MS = 45000;
const unsigned long CAL_WATER_MAX_MS = 8000;
const int CAL_FALLBACK_BURSTS = 1;
// Run-time ceilings for one automatic dose, just above the controller's own
// per-dose hard maximum (GrowStage *_HARD_MAX_DOSE_ML) at the calibrated rates
// (nutrient 0.405 mL/burst, pH 0.29-0.31 mL/burst). The controller sizes the
// dose; these only stop a runaway if the volume or calibration is wrong.
// Run-time backstop only (a stuck output cannot run on): sized above the
// controller's pH hard limit (3.0 mL ~ 4.4 s at ~0.69 mL/s) so it never
// truncates a planned dose. The dose itself comes from the control law:
// distance to the setpoint x volume / tuned gain (7 Oct: 5 bursts cut a
// planned 2.88 mL pH-up dose to 1.72 mL).
const int CAL_MAX_DOSE_BURSTS_PH = 12;        // 6 s ~ 4 mL (hard max 3.0 mL)
const int CAL_MAX_DOSE_BURSTS_NUTRIENT = 85;  // ~34 mL (hard max 32.5 mL)
const unsigned long PUMP_MIN_RUN_MS = 150;
const unsigned long PUMP_MAX_RUN_MS_PH = 2500;
const unsigned long PUMP_MAX_RUN_MS_NUTRIENT = 2500;
const unsigned long WATER_REFILL_INCREMENT_MS = 1500;
const unsigned long WATER_LEVEL_SETTLE_MS = 30000;
const float PH_SIGNAL_LEVEL_SHIFT_GAIN = 1.5f;  // (10k+20k)/20k
const float PH_SIGNAL_DIVIDER_GAIN = PH_SIGNAL_LEVEL_SHIFT_GAIN;
const unsigned long PH_DOSE_INCREMENT_MS = 2000;
const unsigned long MS_PER_MINUTE = 60UL * 1000UL;
const unsigned long MS_PER_HOUR = 60UL * MS_PER_MINUTE;
const unsigned long PH_SETTLE_MS = 30UL * MS_PER_MINUTE;
const unsigned long PH_STUCK_PAUSE_MS = 2UL * MS_PER_HOUR;
const int PH_MAX_DOWN_DOSES_PER_WINDOW = 4;
const unsigned long PH_DOWN_DOSE_WINDOW_MS = 6UL * MS_PER_HOUR;
const int PH_MAX_DOWN_DOSES_PER_BOOT = 8;
const int PH_STUCK_DOSE_LIMIT = 2;
const float PH_STUCK_MIN_IMPROVEMENT = 0.08;
const float PH_AUTO_DOWN_MAX_DELTA = 0.5f;
const unsigned long NUTRIENT_DOSE_INCREMENT_MS = 2000;
const unsigned long NUTRIENT_DILUTE_INCREMENT_MS = 2000;
const unsigned long NUTRIENT_SETTLE_MS = 60000;
const uint16_t MQTT_BROKER_PORT = 1883;
const char* MQTT_DEVICE_ID = "garden-node-01";
const char* MQTT_TOPIC = "smart-garden/garden-node-01/telemetry";
const char* MQTT_ALERT_TOPIC = "smart-garden/garden-node-01/alerts";
const char* MQTT_CMD_TOPIC = "smart-garden/garden-node-01/cmd";
// Immediate replies to commands that a caller waits on (camera flash).
const char* MQTT_ACK_TOPIC = "smart-garden/garden-node-01/ack";
// Control events (dose decisions, checks), one JSON object per event.
const char* MQTT_EVENT_TOPIC = "smart-garden/garden-node-01/events";
const char* MQTT_CLIENT_ID = "esp32-garden-node-01";
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;
const unsigned long WIFI_ENTERPRISE_CONNECT_TIMEOUT_MS = 45000;
// WiFi reconnect pacing only — MQTT does not auto-retry (see docs/mqtt_connect_policy.md).
const uint16_t MQTT_SOCKET_TIMEOUT_SECONDS = 5;
const uint16_t MQTT_KEEPALIVE_SECONDS = 60;
const char* CSV_PATH = SensorDataStore::CSV_PATH;
const char* CSV_HEADER = SensorDataStore::CSV_HEADER;
const char* CSV_PREV_PATH = "/sensor_readings_prev.csv";
// Current + previous journal: 2 x 640 KB. Leaves room in the ~1.9 MB
// LittleFS for the MQTT and Influx outboxes and the control-event log.
const size_t CSV_MAX_BYTES = 640 * 1024;
const size_t CSV_MIN_FREE_BYTES = 32 * 1024;
uint32_t bootId = 0;
uint32_t readingSeq = 0;
uint32_t controlEventSeq = 0;
OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);
DHT dht(DHT11_PIN, DHTTYPE);
PASCO2Ino co2Sensor(&Wire);
Servo pump1;
Servo pump2;
Servo pump3;
Servo pump4;
unsigned char low_data[8];
unsigned char high_data[12];
const int ANALOG_SAMPLE_COUNT    = 10;
const int WATER_LEVEL_SAMPLE_COUNT = 3;
const unsigned long SENSOR_READ_INTERVAL_MS = 10000;
const unsigned long SENSOR_READ_INTERVAL_PH_CAL_MS = 2000;
const unsigned long CO2_SINGLE_SHOT_TIMEOUT_MS = 15000;
const unsigned long CO2_SINGLE_SHOT_POLL_MS = 100;
const uint16_t CO2_MAX_PPM = 32000;
float waterTempC      = 0.0;
int   ldrADC          = 0;
int   tdsADC          = 0;
int   tdsPPM          = 0;
float tdsPpmEma       = NAN;   // 3 min EMA of tdsPPM
int   waterLevelPercent = -1;  // unknown until a trusted sample
int   waterLevelRawPercent = -1;
SensorDataStore::WaterLevelTrust waterLevelTrust;
bool  waterLevelOverMaxLatched = false;
bool  waterLevelSensorFault = false;
uint8_t waterLevelZeroStreak = 0;
const uint8_t WATER_LEVEL_FAULT_ZERO_STREAK = 3;
unsigned long lastWaterLevelPriorityPollMs = 0;
int   co2PPM          = 0;
float phVoltage       = 0.0;
float phValue         = 7.0;   // raw (averaged ADC → pH)
float phValueFiltered = 7.0;   // outlier-rejected multi-sample pH
float phValueEma      = NAN;   // 3 min EMA of phValueFiltered
float ambientTempC    = 0.0;
float ambientHumidityPct = 0.0;
unsigned long lastSensorReadTime = 0;
bool csvHeaderWritten = false;
bool waterRefillPumpRunning = false;
unsigned long waterRefillStartedAt = 0;
unsigned long waterRefillLastRunMs = 0;
unsigned long waterLevelControlAllowedAt = 0;
bool waterFillManualPumpRunning = false;
unsigned long waterFillManualStartedAt = 0;
unsigned long lastWaterFillStatusMs = 0;
const unsigned long WATER_FILL_STATUS_MS = 2000;
bool waterDrainPumpRunning = false;
unsigned long waterDrainStartedAt = 0;
unsigned long waterDrainControlAllowedAt = 0;
PHControlState phControlState = PH_CONTROL_IDLE;
unsigned long phControlStateStartedAt = 0;
unsigned long phControlAllowedAt = 0;
unsigned long phControlPausedUntil = 0;
float phReadingBeforeLastDownDose = NAN;
int phDownDosesWithoutImprovement = 0;
unsigned long phDownDoseWindowStartedAt = 0;
int phDownDosesThisWindow = 0;
int phDownDosesThisBoot = 0;
bool phDownAutoDisabled = false;
bool phSensorFault = false;
bool tdsSensorFault = false;
NutrientControlState nutrientControlState = NUTRIENT_CONTROL_IDLE;
unsigned long nutrientControlStateStartedAt = 0;
unsigned long nutrientControlAllowedAt = 0;
unsigned long activeWaterRefillMs = WATER_REFILL_INCREMENT_MS;
unsigned long activePhDoseMs = PH_DOSE_INCREMENT_MS;
unsigned long activeNutrientDoseMs = NUTRIENT_DOSE_INCREMENT_MS;
unsigned long activeNutrientDiluteMs = NUTRIENT_DILUTE_INCREMENT_MS;
bool waterSafetyRefillActive = false;
int phBurstTarget = 0;
int phBurstDone = 0;
bool phBurstOn = false;
unsigned long phBurstMark = 0;
int nutrientBurstTarget = 0;
int nutrientBurstDone = 0;
bool nutrientBurstOn = false;
unsigned long nutrientBurstMark = 0;
ControlLaw::State waterPidState;
ControlLaw::State phPidState;
ControlLaw::State nutrientPidState;
CalPhase calPhase = CAL_IDLE;
PumpCal::Id calPump = PumpCal::WATER;
bool calSequence = false;
int calSequenceIndex = 0;
unsigned long calPulseMs = 0;
unsigned long calRunStartedAt = 0;
int calBurstsDone = 0;
int calTargetBursts = 0;
unsigned long calTargetMs = 0;
bool calBurstOn = false;
bool calStopRequested = false;
float calRateSum = 0.0f;
int calRateCount = 0;
float calLastSample = NAN;
const char* calMqttMessage = "";
bool calMqttOk = true;
String serialLineAcc;
bool manualPrimeActive = false;
PumpCal::Id manualPrimePump = PumpCal::WATER;
unsigned long manualPrimeStartedAt = 0;
bool autoControlPaused = false;
volatile bool ntpTimeSynced = false;
volatile unsigned long millisAtNtpSyncMs = 0;
volatile uint32_t ntpEpochSecondsAtSync = 0;
volatile bool ntpEpochValid = false;
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

bool isWaterFillMode() {
  return systemMode == GrowStage::FILLING;
}

bool isPrimingPhMode() {
  return systemMode == GrowStage::PH_BASELINE;
}

bool isSensorBaselineMode() {
  return systemMode == GrowStage::SENSOR_BASELINE;
}

bool isControlIdleMode() {
  return GrowStage::isPassiveLogging(systemMode);
}

bool phControlAllowedByMode() {
  // Pause auto pH dosing while the probe two-point wizard is open.
  return GrowStage::allowsPh(systemMode) && !PhProbeCal::isActive();
}

bool nutrientsAllowedByMode() {
  // Pause auto nutrient dosing while the TDS probe wizard is open.
  return GrowStage::allowsEc(systemMode) && !TdsProbeCal::isActive();
}

bool waterAutoControlAllowedByMode() {
  // Never auto top-up without a live Grove stick (and only in stages that
  // schedule drawdown control: SEEDLING / VEGETATIVE / HARVEST).
  return GrowStage::allowsLevelAuto(systemMode) && !isWaterLevelSensorFault();
}

const GrowPreset& activePreset() {
  const GrowStage::Config& cfg = GrowStage::config(systemMode);
  static GrowPreset p;
  p.name = GrowStage::name(systemMode);
  p.phMin = cfg.phTargetMin;
  p.phMax = cfg.phTargetMax;
  p.nutrientTdsMinPpm = static_cast<int>(cfg.ecTargetMin + 0.5f);
  p.nutrientTdsMaxPpm = static_cast<int>(cfg.ecTargetMax + 0.5f);
  return p;
}

const unsigned long PUMP_MAX_RUN_MS_WATER = 8000;

// Grove % where water just touches the net pots (= fill_0 reference).
int netPotTouchPercent = Tank::NET_POT_TOUCH_DEFAULT_PERCENT;

int fill0Percent() {
  const int f0 = StageController::ctx().fill0Percent;
  return f0 >= 0 ? f0 : netPotTouchPercent;
}

int waterCeilingPercent() {
  return Tank::clampPercent(fill0Percent());
}

int waterSafetyFloorPercent() {
  const GrowStage::Config& cfg = GrowStage::config(systemMode);
  return LevelControl::levelPercentForDrawdownMm(
      fill0Percent(),
      static_cast<float>(cfg.targetDrawdownMm + cfg.drawdownToleranceMm + 20));
}

int waterRefillToPercent() {
  const GrowStage::Config& cfg = GrowStage::config(systemMode);
  return LevelControl::levelPercentForDrawdownMm(
      fill0Percent(), static_cast<float>(cfg.targetDrawdownMm));
}

int waterStageTargetPercent() {
  return waterRefillToPercent();
}

const char* hydroponicModeName() {
  return HYDROPONIC_MODE_NAME;
}

unsigned long phSettleMs() {
  return GrowStage::config(systemMode).phSettleMs;
}

unsigned long phStuckPauseMs() {
  return PH_STUCK_PAUSE_MS;
}

int phMaxDownDosesPerWindow() {
  return PH_MAX_DOWN_DOSES_PER_WINDOW;
}

unsigned long phDownDoseWindowMs() {
  return PH_DOWN_DOSE_WINDOW_MS;
}

int phMaxDownDosesPerBoot() {
  return PH_MAX_DOWN_DOSES_PER_BOOT;
}

void printDurationMs(unsigned long ms) {
  if (ms >= MS_PER_HOUR) {
    Serial.print(ms / MS_PER_HOUR);
    Serial.print(" h");
    unsigned long remMin = (ms % MS_PER_HOUR) / MS_PER_MINUTE;
    if (remMin > 0) {
      Serial.print(" ");
      Serial.print(remMin);
      Serial.print(" min");
    }
    return;
  }
  if (ms >= MS_PER_MINUTE) {
    Serial.print(ms / MS_PER_MINUTE);
    Serial.print(" min");
    return;
  }
  Serial.print(ms);
  Serial.print(" ms");
}

