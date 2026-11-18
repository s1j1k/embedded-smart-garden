/* Sensor reads and serial printouts. */
#include "garden.h"
#include "shared/sensor_ema.h"
#include "shared/level_smoother.h"

/************************************************************
 * Sensor Reading With Averaging
 ************************************************************/

void scanI2CDevices() {
  byte error;
  byte address;
  int deviceCount = 0;

  Serial.println("[DEBUG] Scanning I2C bus...");

  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      deviceCount++;
      Serial.print("[DEBUG] I2C device found at 0x");
      if (address < 16) {
        Serial.print("0");
      }
      Serial.println(address, HEX);
    }
  }

  if (deviceCount == 0) {
    Serial.println("[WARN] No I2C devices detected. Check power, GND, SDA/SCL wiring, and pull-up resistors.");
  } else {
    Serial.print("[DEBUG] Found ");
    Serial.print(deviceCount);
    Serial.println(" I2C device(s)");
  }
}

// Wire.endTransmission() codes (ESP32 core 3.x): 0 OK, 1 data too long,
// 2 address NACK, 3 data NACK, 4 other bus error, 5 timeout.
const char* i2cErrorName(uint8_t code) {
  switch (code) {
    case 0: return "OK";
    case 1: return "data too long";
    case 2: return "address NACK";
    case 3: return "data NACK";
    case 4: return "bus error";
    case 5: return "timeout";
    default: return "unknown";
  }
}

// Idle line levels separate a missing device from a stuck bus: both high
// with a NACK is a device unpowered or unplugged; SDA low is a device holding
// the bus; both low is the pull-ups unpowered or a short to GND.
const char* i2cLineState() {
  const bool sda = digitalRead(I2C_SDA) == HIGH;
  const bool scl = digitalRead(I2C_SCL) == HIGH;
  if (sda && scl) return "SDA/SCL idle high";
  if (!sda && scl) return "SDA held low";
  if (sda && !scl) return "SCL held low";
  return "SDA and SCL low";
}

// One line per device when its error changes, then at most once a minute.
void logI2cFailure(I2cFailLog& f, const char* name, uint8_t addr, uint8_t code) {
  const unsigned long now = millis();
  if (code == f.lastCode && f.lastMs != 0 && now - f.lastMs < I2C_FAIL_LOG_MS) {
    return;
  }
  f.lastCode = code;
  f.lastMs = now;
  Serial.printf("[I2C] %s 0x%02X: %s (%u); %s\n", name, addr, i2cErrorName(code), code,
                i2cLineState());
}

void logI2cRecovered(I2cFailLog& f, const char* name, uint8_t addr) {
  if (f.lastMs == 0) {
    return;
  }
  f = I2cFailLog{};
  Serial.printf("[I2C] %s 0x%02X answering again\n", name, addr);
}

// A device reset mid-byte can hold SDA low until it sees the rest of the
// byte: nine SCL clocks and a STOP free it. Wire is restarted afterwards.
void i2cRecoverBus() {
  Wire.end();
  pinMode(I2C_SDA, INPUT_PULLUP);
  pinMode(I2C_SCL, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SCL, HIGH);
  for (int i = 0; i < 9 && digitalRead(I2C_SDA) == LOW; i++) {
    digitalWrite(I2C_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(I2C_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(I2C_SDA, LOW);
  delayMicroseconds(5);
  digitalWrite(I2C_SDA, HIGH);
  delayMicroseconds(5);
  pinMode(I2C_SDA, INPUT_PULLUP);
  const bool freed = digitalRead(I2C_SDA) == HIGH;
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println(freed ? "[I2C] Bus recovery: SDA released"
                       : "[I2C] Bus recovery: SDA still low - a device or the wiring holds "
                         "it (unplug the I2C sensors one at a time)");
}

static SensorEma::State tdsEmaState;

unsigned long tdsWarmupElapsedS() {
  return SensorEma::warmupElapsedS(tdsEmaState, millis());
}

// Pieces of the sensor cycle. The non-blocking cycle (sensor_acq.cpp) calls
// them one per loop pass; none of them waits.

void readLdrSensor() {
  #if ENABLE_PH_TDS
  ldrADC = readAnalogAverage(LDR_PIN);
  #else
  ldrADC = 0;
  #endif
}

// TDS probe power on TDS_POWER_PIN: on only for the TDS read, so its AC
// excitation is not in the water while pH is read.
static bool tdsPowered = false;
static unsigned long tdsPoweredOffAt = 0;

void initTdsPower() {
  #if TDS_POWER_SWITCHED
  pinMode(TDS_POWER_PIN, OUTPUT);
  digitalWrite(TDS_POWER_PIN, LOW);
  tdsPowered = false;
  tdsPoweredOffAt = millis();
  Serial.print("[DEBUG] TDS probe power on GPIO ");
  Serial.print(TDS_POWER_PIN);
  Serial.println(" (off; on only for the TDS read)");
  #endif
}

void setTdsPower(bool on) {
  #if TDS_POWER_SWITCHED
  if (on == tdsPowered) {
    return;
  }
  digitalWrite(TDS_POWER_PIN, on ? HIGH : LOW);
  tdsPowered = on;
  if (!on) {
    tdsPoweredOffAt = millis();
  }
  #else
  (void)on;
  #endif
}

bool tdsPowerOn() {
  #if TDS_POWER_SWITCHED
  return tdsPowered;
  #else
  return true;
  #endif
}

bool tdsExcitationActive() {
  #if TDS_POWER_SWITCHED
  return tdsPowered || millis() - tdsPoweredOffAt < TDS_POWER_QUIET_MS;
  #else
  return false;
  #endif
}

float readTdsGpioVoltsPowered(int samples) {
  setTdsPower(true);
  #if TDS_POWER_SWITCHED
  delay(TDS_POWER_WARMUP_MS);
  #endif
  const float v = AnalogFilter::readGpioVoltsAverage(TDS_PIN, samples, 5);
  setTdsPower(false);
  return v;
}

void waitForTdsQuiet() {
  setTdsPower(false);
  while (tdsExcitationActive()) {
    delay(50);
  }
}

int readTdsAdcPowered() {
  setTdsPower(true);
  #if TDS_POWER_SWITCHED
  delay(TDS_POWER_WARMUP_MS);
  #endif
  const int adc = readAnalogAverage(TDS_PIN);
  setTdsPower(false);
  return adc;
}

// 16-sample TDS burst (about 80 ms, four 50 Hz cycles), then the control filter.
// The sensor cycle powers the probe and waits TDS_POWER_WARMUP_MS first.
void readTdsSensor() {
  #if ENABLE_PH_TDS
  if (sharedRailPumpDrawing()) {
    Serial.println("[DEBUG] TDS read held — pump on shared 5V rail");
    return;
  }
  tdsADC = readAnalogAverage(TDS_PIN);
  tdsPPM = convertTDSToPPM(tdsADC);
  // A dry or unplugged probe reads 0; even rain or RO water reads over 10 ppm.
  // Drop the reading and restart the warm-up, as for a pH fault, so no dose is
  // sized from a filter still ramping up after the probe goes back in.
  if (tdsPPM < ControlDesign::TDS_PROBE_DRY_PPM) {
    if (!tdsSensorFault) {
      Serial.print("[ALERT] TDS sensor fault — ");
      Serial.print(tdsPPM);
      Serial.println(" ppm: probe out of the water or unplugged? Nutrient dosing held.");
    }
    tdsSensorFault = true;
    tdsPpmEma = NAN;
    SensorEma::reset(tdsEmaState);
    return;
  }
  if (tdsSensorFault) {
    tdsSensorFault = false;
    Serial.println("[ALERT] TDS sensor recovered — EMA restarts 60 s warm-up.");
  }
  tdsPpmEma = SensorEma::update(tdsEmaState, static_cast<float>(tdsPPM), millis());
  #else
  tdsADC = 0;
  tdsPPM = 0;
  tdsPpmEma = NAN;
  #endif
}

void printAnalogSensors() {
  #if ENABLE_PH_TDS
  Serial.print("[DEBUG] LDR ADC - ");
  Serial.println(ldrADC);
  Serial.print("[DEBUG] TDS ADC - ");
  Serial.println(tdsADC);
  Serial.print("[DEBUG] TDS ");
  Serial.print(tdsPPM);
  Serial.print(" ppm, ema ");
  Serial.print(tdsPpmEma, 0);
  Serial.print(" ppm @ water ");
  Serial.print(TdsProbeCal::effectiveTempC(waterTempC), 1);
  Serial.print(" C (comp coeff ");
  Serial.print(TdsProbeCal::compensationCoefficient(waterTempC), 3);
  Serial.println(")");
  #else
  phVoltage = 0.0;
  phValue = 7.0;
  phValueFiltered = 7.0;
  phValueEma = NAN;
  Serial.println("[DEBUG] pH/TDS/LDR disabled by configuration");
  #endif
}

// DS18B20: start a conversion without waiting (the cycle waits 750 ms by
// millis), then read it.
void ds18b20StartConversion() {
  #if ENABLE_DS18B20
  ds18b20.setWaitForConversion(false);
  ds18b20.requestTemperatures();
  ds18b20.setWaitForConversion(true);
  #endif
}

float ds18b20ReadConversion() {
  #if ENABLE_DS18B20
  const float temp = ds18b20.getTempCByIndex(0);
  return (temp > -50 && temp < 100) ? temp : NAN;
  #else
  return NAN;
  #endif
}

// One DHT11 read (about 25 ms). Returns true and updates the ambient values on
// success; the cycle retries after DHT_RETRY_MS on failure.
bool readAmbientDHT11Once(int attempt) {
  #if ENABLE_AMBIENT_SENSORS
  const float temp = dht.readTemperature();
  const float humidity = dht.readHumidity();
  if (!isnan(temp) && !isnan(humidity)) {
    ambientTempC = temp;
    ambientHumidityPct = humidity;
    Serial.print("[DEBUG] DHT11 attempt ");
    Serial.print(attempt);
    Serial.print(" -> temp=");
    Serial.print(temp);
    Serial.print(" C, humidity=");
    Serial.print(humidity);
    Serial.println(" %");
    return true;
  }
  Serial.print("[WARN] DHT11 attempt ");
  Serial.print(attempt);
  Serial.println(" failed");
  return false;
  #else
  ambientTempC = -127.0;
  ambientHumidityPct = -1.0;
  return true;
  #endif
}

void updateWaterLevelReading(bool verbose) {
  #if ENABLE_WATER_LEVEL
  if (verbose) {
    Serial.println("[DEBUG] Reading water level...");
  }
  applyWaterLevelReading(readWaterLevelAverage(), verbose);
  #else
  waterLevelPercent = 0;
  waterLevelRawPercent = 0;
  #endif
}

// Apply one (averaged) raw level reading: fault detection, trust filter.
static LevelSmoother::State levelSmooth;

// Median of the last 30 s of stick readings (one every 2 s); -1 when none.
int waterLevelSmoothedPercent() {
  return LevelSmoother::median(levelSmooth, millis());
}

// Highest minus lowest stick reading over the last 60 s.
int waterLevelSpreadPercent() {
  return LevelSmoother::spread(levelSmooth, millis());
}

void applyWaterLevelReading(int rawWaterLevelPercent, bool verbose) {
  #if ENABLE_WATER_LEVEL
  waterLevelRawPercent = rawWaterLevelPercent;
  LevelSmoother::add(levelSmooth, rawWaterLevelPercent, millis());
  updateWaterLevelSensorFault(rawWaterLevelPercent);
  waterLevelPercent = SensorDataStore::filterWaterLevelReading(
      waterLevelTrust, rawWaterLevelPercent);
  if (verbose) {
    Serial.print("[DEBUG] Water level - raw=");
    Serial.print(rawWaterLevelPercent);
    Serial.print(" %, trusted=");
    Serial.print(waterLevelPercent);
    if (waterLevelSensorFault) {
      Serial.print(" % [SENSOR FAULT — missing or stuck 0%]");
    }
    Serial.println(" %");
  } else if (waterLevelPercent > waterCeilingPercent()) {
    Serial.print("[DEBUG] Priority water poll - trusted level ");
    Serial.print(waterLevelPercent);
    Serial.print("% is above net-pot touch ");
    Serial.print(waterCeilingPercent());
    Serial.println("%");
  }
  #else
  waterLevelPercent = 0;
  waterLevelRawPercent = 0;
  #endif
}

bool isWaterLevelSensorFault() {
  return waterLevelSensorFault;
}

bool isWaterLevelInitRequired() {
  #if ENABLE_WATER_LEVEL
  return !waterLevelTrust.established;
  #else
  return false;
  #endif
}

// Control acts on the smoothed level (median of 30 s): single stick readings
// jump by several 5 % steps with ripple or a touched stick.
int waterLevelPercentForControl() {
  if (waterLevelSensorFault || isWaterLevelInitRequired()) {
    return -1;
  }
  const int smoothed = waterLevelSmoothedPercent();
  return smoothed >= 0 ? smoothed : waterLevelPercent;
}

void updateWaterLevelSensorFault(int rawPercent) {
  // Disconnected / no I2C response: always inhibit auto water control.
  if (rawPercent < 0) {
    waterLevelZeroStreak = 0;
    if (!waterLevelSensorFault) {
      waterLevelSensorFault = true;
      Serial.println("[ALERT] Water level sensor not responding (disconnected / I2C).");
      Serial.println("[ALERT] Auto water top-up inhibited until the Grove stick answers again.");
    }
    return;
  }

  // After boot the stick reads 0% until lifted and lowered back in. That is
  // "init required" (readings already ignored), not a stuck-sensor fault.
  if (!waterLevelTrust.established && rawPercent == 0) {
    waterLevelZeroStreak = 0;
    return;
  }

  // FILL mode can legitimately see 0% while the tank is empty. After CONFIRM /
  // leave FILL with fill_0 saved, 0% means the Grove stick failed to re-sense.
  const bool pastFill =
      !isWaterFillMode() && StageController::ctx().fill0Percent >= 0;

  if (!pastFill) {
    waterLevelZeroStreak = 0;
    if (waterLevelSensorFault) {
      waterLevelSensorFault = false;
      Serial.println("[ALERT] Water level fault cleared (back in FILL / no fill_0).");
    }
    return;
  }

  if (rawPercent == 0) {
    if (waterLevelZeroStreak < 255) {
      waterLevelZeroStreak++;
    }
    if (waterLevelZeroStreak >= WATER_LEVEL_FAULT_ZERO_STREAK &&
        !waterLevelSensorFault) {
      waterLevelSensorFault = true;
      Serial.println("[ALERT] Water level stuck at 0% after FILL — Grove sensor fault.");
      Serial.println("[ALERT] Auto water top-up inhibited. Reset: lift stick above water,");
      Serial.println("        dry it, power-cycle ESP (or sensor VCC) while dry, reinsert.");
      Serial.println("[ALERT] No reliable software reset — physical dry power-on is required.");
    }
    return;
  }

  if (rawPercent > 0) {
    waterLevelZeroStreak = 0;
    if (waterLevelSensorFault) {
      waterLevelSensorFault = false;
      Serial.print("[ALERT] Water level sensor recovered at ");
      Serial.print(rawPercent);
      Serial.println("% — auto water control re-enabled.");
    }
  }
}

int readAnalogAverage(int pin) {
  const int average = AnalogFilter::readAdcAverage(pin, AnalogFilter::DEFAULT_SAMPLES, 5);
  Serial.print("[DEBUG] Raw ADC on pin ");
  Serial.print(pin);
  Serial.print(" -> ");
  Serial.println(average);
  return average;
}

float readPHBoardVoltageAverage() {
  const float gpioVoltage =
      AnalogFilter::readGpioVoltsAverage(PH_PIN, AnalogFilter::DEFAULT_SAMPLES, 5);
  const float boardVoltage = gpioVoltage * PH_SIGNAL_LEVEL_SHIFT_GAIN;
  Serial.print("[DEBUG] pH GPIO voltage - ");
  Serial.print(gpioVoltage, 3);
  Serial.print(" V; board voltage - ");
  Serial.print(boardVoltage, 3);
  Serial.println(" V");
  return boardVoltage;
}

float convertVoltageToPH(float voltage) {
  // Nernst compensation from DS18B20 water temp (25 °C fallback if invalid).
  return PhProbeCal::voltageToPh(voltage, waterTempC);
}

float readDS18B20Average() {
  float total = 0.0;
  int validCount = 0;

  for (int i = 0; i < 3; i++) {
    ds18b20.requestTemperatures();
    float temp = ds18b20.getTempCByIndex(0);

    if (temp > -50 && temp < 100) {
      total += temp;
      validCount++;
    }

    delay(100);
  }

  if (validCount == 0) {
    Serial.println("[WARN] DS18B20 returned no valid readings");
    return -127.0;
  }

  return total / validCount;
}

int readWaterLevelAverage() {
  int total = 0;
  int validCount = 0;

  for (int i = 0; i < WATER_LEVEL_SAMPLE_COUNT; i++) {
    int level = readWaterLevelPercent();

    if (level >= 0 && level <= 100) {
      total += level;
      validCount++;
    }

    delay(100);
  }

  if (validCount == 0) {
    Serial.println("[WARN] Water level sensor returned no valid readings");
    return -1;
  }

  return total / validCount;
}

static const char* co2ErrorName(Error_t err) {
  switch (err) {
    case XENSIV_PASCO2_OK: return "OK";
    case XENSIV_PASCO2_ERR_COMM: return "COMM (I2C)";
    case XENSIV_PASCO2_ERR_WRITE_TOO_LARGE: return "WRITE_TOO_LARGE";
    case XENSIV_PASCO2_ERR_NOT_READY: return "NOT_READY";
    case XENSIV_PASCO2_ICCERR: return "ICCERR";
    case XENSIV_PASCO2_ORVS: return "ORVS (emitter voltage)";
    case XENSIV_PASCO2_ORTMP: return "ORTMP (temperature)";
    case XENSIV_PASCO2_READ_NRDY: return "READ_NRDY";
    default: return "UNKNOWN";
  }
}

// CO2 single-shot, split into non-blocking pieces for the sensor cycle:
//   co2StartSingleShot()  start a measurement
//   co2PollReady()        one MEAS_STS read; the cycle calls it every
//                         CO2_SINGLE_SHOT_POLL_MS (150 ms after a bus error)
//   co2ReadPpm()          read CO2PPM once DRDY is set
// Registers: CO2PPM_H=0x05, CO2PPM_L=0x06 (NOT 0x02/0x03 — those are MEAS_RATE).
static bool co2Ready = false;
static unsigned long co2LastProbeMs = 0;
static I2cFailLog co2I2cLog;

bool co2Init() {
  co2Ready = co2Sensor.begin() == XENSIV_PASCO2_OK;
  return co2Ready;
}

bool co2SensorReady() {
  return co2Ready;
}

// Once the CO2 drops off the bus, probe 0x28 every CO2_REPROBE_MS and run
// begin() when it answers, so a sensor that lost power comes back without a
// reboot. begin() blocks ~2 s, so it waits while a pump runs (that would
// stretch a dose). SDA held low gets a bus recovery first.
static void co2TryReinit() {
  const unsigned long now = millis();
  if (co2LastProbeMs != 0 && now - co2LastProbeMs < CO2_REPROBE_MS) {
    return;
  }
  if (sharedRailPumpDrawing()) {
    return;
  }
  co2LastProbeMs = now;
  if (digitalRead(I2C_SDA) == LOW) {
    i2cRecoverBus();
  }
  Wire.beginTransmission(CO2_I2C_ADDR);
  const uint8_t code = Wire.endTransmission();
  if (code != 0) {
    logI2cFailure(co2I2cLog, "CO2", CO2_I2C_ADDR, code);
    return;
  }
  logI2cRecovered(co2I2cLog, "CO2", CO2_I2C_ADDR);
  Serial.println("[I2C] CO2 re-initialising");
  if (co2Init()) {
    Serial.println("[I2C] CO2 sensor ready again");
  } else {
    Serial.println("[WARN] CO2 re-init failed - 0x28 answers; check VDD5 and 3.3 V before 5 V");
  }
}

bool co2StartSingleShot() {
  if (!co2Ready) {
    co2TryReinit();
    if (!co2Ready) {
      return false;
    }
  }
  const Error_t co2Error = co2Sensor.startMeasure();
  if (co2Error == XENSIV_PASCO2_ERR_COMM) {
    // Off the bus: probe and re-init from the next cycle on.
    co2Ready = false;
    co2LastProbeMs = 0;
  }
  if (co2Error != XENSIV_PASCO2_OK) {
    Serial.print("[WARN] CO2 single-shot start failed (error ");
    Serial.print(co2Error);
    Serial.print(" ");
    Serial.print(co2ErrorName(co2Error));
    Serial.println(")");
    return false;
  }
  return true;
}

Co2Poll co2PollReady() {
  uint8_t measSts = 0;
  const Error_t co2Error = co2Sensor.getRegister(XENSIV_PASCO2_REG_MEAS_STS, &measSts, 1);
  if (co2Error == XENSIV_PASCO2_ERR_COMM) {
    // Bus noise during the measurement is common on a shared I2C line.
    return CO2_POLL_COMM_RETRY;
  }
  if (co2Error != XENSIV_PASCO2_OK) {
    Serial.print("[WARN] CO2 MEAS_STS read failed (error ");
    Serial.print(co2Error);
    Serial.print(" ");
    Serial.print(co2ErrorName(co2Error));
    Serial.println(")");
    return CO2_POLL_ERROR;
  }
  return (measSts & XENSIV_PASCO2_REG_MEAS_STS_DRDY_MSK) != 0 ? CO2_POLL_READY : CO2_POLL_WAIT;
}

// Returns ppm, -1 on a bad value or error, or -2 for an I2C error worth one
// retry (the cycle retries after 150 ms).
int co2ReadPpm() {
  uint8_t ppmBytes[2] = {0, 0};
  const Error_t co2Error = co2Sensor.getRegister(XENSIV_PASCO2_REG_CO2PPM_H, ppmBytes, 2);
  if (co2Error == XENSIV_PASCO2_ERR_COMM) {
    return -2;
  }
  if (co2Error != XENSIV_PASCO2_OK) {
    Serial.print("[WARN] CO2PPM register read failed (error ");
    Serial.print(co2Error);
    Serial.print(" ");
    Serial.print(co2ErrorName(co2Error));
    Serial.println(")");
    return -1;
  }

  // Big-endian unsigned combine (same as datasheet / Infineon htons path).
  const uint16_t ppmU =
      (static_cast<uint16_t>(ppmBytes[0]) << 8) | static_cast<uint16_t>(ppmBytes[1]);
  const int16_t ppmSigned = static_cast<int16_t>(ppmU);

  Serial.print("[DEBUG] CO2 raw bytes H=0x");
  Serial.print(ppmBytes[0], HEX);
  Serial.print(" L=0x");
  Serial.print(ppmBytes[1], HEX);
  Serial.print(" → uint16=");
  Serial.print(ppmU);
  Serial.print(" int16=");
  Serial.println(ppmSigned);

  if (ppmU == 0 || ppmU > CO2_MAX_PPM) {
    Serial.print("[WARN] CO2 invalid unsigned ppm - ");
    Serial.println(ppmU);
    Serial.println("[WARN] If uint16 is huge (~60k) and int16 ~-2800, data is still "
                   "garbage (often missing emitter power), not a real CO2 value.");
    return -1;
  }

  Serial.print("[DEBUG] CO2 -> ");
  Serial.print(ppmU);
  Serial.println(" ppm (single-shot, uint16)");
  return static_cast<int>(ppmU);
}

void printCo2Timeout(int commFails) {
  Serial.print("[WARN] CO2 timed out waiting for MEAS_STS.DRDY");
  if (commFails > 0) {
    Serial.print(" (");
    Serial.print(commFails);
    Serial.print(" I2C COMM retries)");
  }
  Serial.println();
  Serial.println("[WARN] Check VDD3.3 + VDD5/VDD12, PSEL→GND, I2C 0x28, "
                 "pull-ups; emitter current can disrupt the bus.");
}

int readWaterLevelPercent() {
  // Probe both Grove high/low I2C addresses. A fully disconnected stick
  // NACKs both — never pretend that is a real 0% reading (that would look
  // like an empty tank and could trigger top-up once fill_0 exists).
  static I2cFailLog levelI2cLog;
  Wire.beginTransmission(WATER_LOW_ADDR);
  const uint8_t lowAck = Wire.endTransmission();
  Wire.beginTransmission(WATER_HIGH_ADDR);
  const uint8_t highAck = Wire.endTransmission();
  if (lowAck != 0 && highAck != 0) {
    logI2cFailure(levelI2cLog, "Water level", WATER_LOW_ADDR, lowAck);
    return -1;
  }
  logI2cRecovered(levelI2cLog, "Water level", WATER_LOW_ADDR);

  memset(low_data, 0, sizeof(low_data));
  memset(high_data, 0, sizeof(high_data));

  Wire.requestFrom(WATER_LOW_ADDR, 8);

  int lowGot = 0;
  while (Wire.available() && lowGot < 8) {
    low_data[lowGot++] = Wire.read();
  }

  Wire.requestFrom(WATER_HIGH_ADDR, 12);

  int highGot = 0;
  while (Wire.available() && highGot < 12) {
    high_data[highGot++] = Wire.read();
  }

  if (lowGot == 0 && highGot == 0) {
    return -1;
  }

  int touched = 0;

  for (int j = 0; j < 8; j++) {
    if (low_data[j] > WATER_TOUCH_THRESHOLD && low_data[j] < NO_TOUCH) {
      touched++;
    }
  }

  for (int j = 0; j < 12; j++) {
    if (high_data[j] > WATER_TOUCH_THRESHOLD && high_data[j] < NO_TOUCH) {
      touched++;
    }
  }

  int percent = map(touched, 0, 20, 0, 100);

  if (percent < 0)   percent = 0;
  if (percent > 100) percent = 100;

  return percent;
}

/************************************************************
 * Conversion Functions
 ************************************************************/

int convertTDSToPPM(int adc) {
  // Grove / DFRobot: divide probe volts by (1 + 0.02*(T−25)) using DS18B20
  // water temperature, then apply the cubic and kValue.
  return TdsProbeCal::adcToPpm(adc, waterTempC);
}

/************************************************************
 * Serial Output
 ************************************************************/

void printSensorData() {
  const GrowPreset& preset = activePreset();

  Serial.println();
  Serial.println("========== SENSOR DATA ==========");

  Serial.print("Timestamp (ms) - ");
  Serial.println(millis());
  Serial.print("Uptime - ");
  Serial.print(millis() / 1000UL);
  Serial.print(" s; boots=");
  Serial.print(PowerMonitor::bootCountValue());
  Serial.print("; brownouts=");
  Serial.print(PowerMonitor::brownoutCountValue());
  Serial.print("; last_reset=");
  Serial.println(PowerMonitor::lastResetReasonName());
  {
    const int railMv = PowerMonitor::readRailMv();
    Serial.print("Supply - free_heap=");
    Serial.print(PowerMonitor::freeHeapBytes());
    Serial.print(" B");
    if (PowerMonitor::wifiRssiValid()) {
      Serial.print("; wifi_rssi=");
      Serial.print(PowerMonitor::wifiRssiDbm());
      Serial.print(" dBm");
    }
    if (railMv >= 0) {
      Serial.print("; rail=");
      Serial.print(railMv);
      Serial.print(" mV");
    }
    Serial.println();
  }

  Serial.print("System Mode - ");
  Serial.println(systemModeName());

  Serial.print("Grow Preset - ");
  Serial.println(preset.name);

  Serial.print("Nutrients Allowed - ");
  Serial.println(nutrientsAllowedByMode() ? "yes" : "no (need MODE SEEDLING+)");

  Serial.print("Hydroponic Mode - ");
  Serial.print(hydroponicModeName());
  Serial.print(" (pH re-dose wait ");
  printDurationMs(phSettleMs());
  Serial.println(")");

  Serial.print("Chemistry Control - ");
  Serial.println(CHEMISTRY_CONTROL_LAW);

  Serial.print("Water Temperature - ");
  Serial.print(waterTempC);
  Serial.println(" C");

  Serial.print("LDR ADC - ");
  Serial.println(ldrADC);

  Serial.print("Water Level - ");
  Serial.print(waterLevelPercent);
  Serial.print(" % (stage air-gap line ");
  Serial.print(waterStageTargetPercent());
  Serial.print(" %, raw ");
  Serial.print(waterLevelRawPercent);
  Serial.print(" %");
  if (isWaterLevelSensorFault()) {
    Serial.print(", FAULT: sensor missing or stuck 0% — auto water off");
  }
  Serial.print("; touch ");
  Serial.print(waterCeilingPercent());
  Serial.print(" %, safety floor ");
  Serial.print(waterSafetyFloorPercent());
  Serial.println(" %)");

  Serial.print("TDS ADC - ");
  Serial.println(tdsADC);

  Serial.print("TDS Approx - ");
  Serial.print(tdsPPM);
  Serial.print(" ppm, ema ");
  Serial.print(tdsPpmEma, 0);
  Serial.print(" ppm @ ");
  Serial.print(TdsProbeCal::effectiveTempC(waterTempC), 1);
  Serial.print(" C water (comp ");
  Serial.print(TdsProbeCal::compensationCoefficient(waterTempC), 3);
  Serial.print("; target ");
  Serial.print(preset.nutrientTdsMinPpm);
  Serial.print("-");
  Serial.print(preset.nutrientTdsMaxPpm);
  Serial.println(" ppm)");

  Serial.print("Nutrient Dose - ");
  Serial.println(nutrientDoseLabel());

  Serial.print("pH Voltage - ");
  Serial.print(phVoltage, 3);
  Serial.println(" V");

  Serial.print("pH Approx (raw) - ");
  Serial.print(phValue, 2);
  Serial.print(" @ ");
  Serial.print(PhProbeCal::effectiveTempC(waterTempC), 1);
  Serial.print(" C water (Nernst ");
  Serial.print(PhProbeCal::compensationFactor(waterTempC), 4);
  Serial.println(")");

  Serial.print("pH Approx (filtered) - ");
  Serial.print(phValueFiltered, 2);
  Serial.print(", ema ");
  Serial.println(phValueEma, 2);

  Serial.print("pH Target - ");
  if (phControlAllowedByMode()) {
    Serial.print(preset.phMin, 1);
    Serial.print(" to ");
    Serial.println(preset.phMax, 1);
  } else {
    Serial.println("disabled");
  }

  Serial.print("pH Dose - ");
  Serial.println(phDoseLabel());

  Serial.print("Ambient Temp - ");
  Serial.print(ambientTempC);
  Serial.println(" C");

  Serial.print("Ambient Humidity - ");
  Serial.print(ambientHumidityPct);
  Serial.println(" %");

  Serial.print("CO2 - ");
  Serial.print(co2PPM);
  Serial.println(" ppm");

  #if ENABLE_GROW_LIGHT
  {
    const GrowLight::Status gl = GrowLight::status();
    Serial.print("Grow Light - ");
    Serial.print(gl.mode);
    Serial.print(gl.on ? " (on)" : " (off)");
    Serial.print(", brightness ");
    Serial.print(gl.brightnessPct);
    Serial.print("%, photoperiod ");
    Serial.print(gl.photoperiodOnHours);
    if (gl.ntpSchedule) {
      Serial.print(" h/day (ON ");
      Serial.print(gl.scheduleStartHourUtc);
      Serial.print(":00 -> OFF ");
      Serial.print(gl.scheduleOffHourUtc);
      Serial.print(":00 UTC), ");
    } else {
      Serial.print(" h/day (counted from boot, no NTP), ");
    }
    Serial.print(gl.wantOn ? "off in " : "back on in ");
    Serial.print(gl.nextChangeS / 60);
    Serial.println(" min");
  }
  #endif

  printPlotterData();

  Serial.println("=================================");
}

// Keep ':' exclusively for labelled numeric data, so Arduino Serial Plotter
// can distinguish this line from the human-readable diagnostics above.
void printPlotterData() {
  const GrowPreset& preset = activePreset();
  unsigned long pumpRunMs = waterRefillPumpRunning
                             ? millis() - waterRefillStartedAt
                             : waterRefillLastRunMs;

  Serial.print("WaterLevelPct:");
  Serial.print(waterLevelPercent);
  Serial.print(" WaterTargetPct:");
  Serial.print(waterStageTargetPercent());
  Serial.print(" WaterMaxPct:");
  Serial.print(waterCeilingPercent());
  Serial.print(" WaterFloorPct:");
  Serial.print(waterSafetyFloorPercent());
  Serial.print(" PumpOn:");
  Serial.print(waterRefillPumpRunning ? 1 : 0);
  Serial.print(" DrainOn:");
  Serial.print(waterDrainPumpRunning ? 1 : 0);
  Serial.print(" PumpRunMs:");
  Serial.print(pumpRunMs);
  Serial.print(" PumpStartedMs:");
  Serial.print(waterRefillStartedAt);
  Serial.print(" TDSppm:");
  Serial.print(tdsPPM);
  Serial.print(" NutrientTdsMin:");
  Serial.print(preset.nutrientTdsMinPpm);
  Serial.print(" NutrientTdsMax:");
  Serial.print(preset.nutrientTdsMaxPpm);
  Serial.print(" NutrientDose:");
  Serial.print(nutrientControlState == NUTRIENT_DOSING ? 1 : 0);
  Serial.print(" NutrientDilute:");
  Serial.print(nutrientControlState == NUTRIENT_DILUTING ? 1 : 0);
  Serial.print(" pH:");
  Serial.print(phValue, 2);
  Serial.print(" PhEma:");
  Serial.print(phValueEma, 2);
  Serial.print(" TdsEma:");
  Serial.print(tdsPpmEma, 1);
  Serial.print(" PhTargetMin:");
  Serial.print(phControlAllowedByMode() ? preset.phMin : 0.0, 1);
  Serial.print(" PhTargetMax:");
  Serial.print(phControlAllowedByMode() ? preset.phMax : 0.0, 1);
  Serial.print(" PhDoseUp:");
  Serial.print(phControlState == PH_DOSING_UP ? 1 : 0);
  Serial.print(" PhDoseDown:");
  Serial.print(phControlState == PH_DOSING_DOWN ? 1 : 0);
  Serial.print(" CO2ppm:");
  Serial.println(co2PPM);
}
