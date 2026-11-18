/* Non-blocking sensor acquisition.
 *
 * The sensor cycle used to block the loop for several seconds (three DS18B20
 * conversions, DHT11 retries, up to 15 s for a CO2 single-shot), and the loop
 * also times the pumps. It now runs as a state machine: serviceSensorCycle()
 * does at most one short step per loop pass and waits by millis(), never by
 * delay(). The longest single step is a 16-sample analog burst (~80 ms).
 *
 * Pumps and sensor reads never act on each other:
 *  - a chemistry dose is stopped by a hardware timer (staged_control.cpp), so
 *    no sensor step can lengthen it;
 *  - the TDS and pH bursts are not taken while a pump draws on the shared 5 V
 *    rail; the cycle waits for the pump (at most SENSOR_READ_MAX_DEFER_MS)
 *    and otherwise keeps the last value;
 *  - a pump cannot start during a burst, because pumps are started from the
 *    same loop, between steps.
 * The TDS probe is powered only for its read (TDS_POWER_PIN): on, wait
 * TDS_POWER_WARMUP_MS, read, off, then wait TDS_POWER_QUIET_MS before the pH
 * read, so the TDS excitation is not in the water while pH is sampled.
 * The water-level safety poll runs the same way and is paused while the CO2
 * emitter is measuring, which can disturb the shared I2C bus.
 */
#include "garden.h"

namespace {

enum Step {
  ST_IDLE,
  ST_LEVEL,
  ST_DS_START,
  ST_DS_WAIT,
  ST_LDR,
  ST_TDS_POWER,
  ST_TDS,
  ST_PH,
  ST_DHT,
  ST_CO2_START,
  ST_CO2_POLL,
  ST_CO2_READ,
  ST_DONE
};

constexpr unsigned long LEVEL_SPACING_MS = 100;
constexpr unsigned long DS18B20_CONVERSION_MS = 750;  // 12-bit
constexpr int DS18B20_CONVERSIONS = 3;
constexpr int DHT_ATTEMPTS = 3;
constexpr unsigned long DHT_RETRY_MS = 1100;          // DHT11 needs > 1 s between reads
constexpr unsigned long CO2_FIRST_POLL_MS = 200;      // emitter ramp before the first poll
constexpr unsigned long I2C_RETRY_MS = 150;

struct LevelSampler {
  bool active = false;
  int count = 0;
  int valid = 0;
  int total = 0;
  unsigned long nextMs = 0;
};

struct Cycle {
  Step step = ST_IDLE;
  unsigned long nextMs = 0;
  LevelSampler level;
  int dsCount = 0;
  int dsValid = 0;
  float dsTotal = 0.0f;
  unsigned long analogWaitStartMs = 0;
  int dhtAttempt = 0;
  unsigned long co2DeadlineMs = 0;
  int co2CommFails = 0;
  bool co2Retried = false;
};

Cycle cyc;
LevelSampler priorityLevel;

bool reached(unsigned long now, unsigned long at) {
  return static_cast<long>(now - at) >= 0;
}

void startLevel(LevelSampler& s, unsigned long now) {
  s = LevelSampler{};
  s.active = true;
  s.nextMs = now;
}

// One Grove level sample when due. Returns true once WATER_LEVEL_SAMPLE_COUNT
// samples are in; out is their average, or -1 if none was valid.
bool stepLevel(LevelSampler& s, unsigned long now, int& out) {
  if (!s.active || !reached(now, s.nextMs)) {
    return false;
  }
  const int level = readWaterLevelPercent();
  if (level >= 0 && level <= 100) {
    s.total += level;
    s.valid++;
  }
  s.count++;
  s.nextMs = now + LEVEL_SPACING_MS;
  if (s.count < WATER_LEVEL_SAMPLE_COUNT) {
    return false;
  }
  s.active = false;
  if (s.valid == 0) {
    Serial.println("[WARN] Water level sensor returned no valid readings");
    out = -1;
  } else {
    out = s.total / s.valid;
  }
  return true;
}

void afterLevel(int raw, bool verbose, unsigned long now) {
  applyWaterLevelReading(raw, verbose);
  lastWaterLevelPriorityPollMs = now;
  updateWaterDrainPump(now);
  enforceWaterLevelCeiling();
  controlWaterDrain(now);
}

// True while the analog burst should wait for a pump to stop.
bool waitForRail(unsigned long now) {
  if (!sharedRailPumpDrawing()) {
    cyc.analogWaitStartMs = 0;
    return false;
  }
  if (cyc.analogWaitStartMs == 0) {
    cyc.analogWaitStartMs = now;
  }
  if (now - cyc.analogWaitStartMs < SENSOR_READ_MAX_DEFER_MS) {
    return true;
  }
  // Pump ran longer than expected: keep the last values this cycle.
  return false;
}

}  // namespace

bool sensorCycleActive() {
  return cyc.step != ST_IDLE;
}

void startSensorCycle(unsigned long now) {
  cyc = Cycle{};
  #if ENABLE_WATER_LEVEL
  Serial.println("[DEBUG] Reading water level...");
  priorityLevel.active = false;  // the cycle's own level read replaces it
  startLevel(cyc.level, now);
  cyc.step = ST_LEVEL;
  #else
  cyc.step = ST_DS_START;
  #endif
}

bool serviceSensorCycle(unsigned long now) {
  switch (cyc.step) {
    case ST_IDLE:
      return false;

    case ST_LEVEL: {
      int raw = -1;
      if (stepLevel(cyc.level, now, raw)) {
        afterLevel(raw, true, now);
        cyc.step = ST_DS_START;
      }
      return false;
    }

    case ST_DS_START:
      #if ENABLE_DS18B20
      ds18b20StartConversion();
      cyc.nextMs = now + DS18B20_CONVERSION_MS;
      cyc.step = ST_DS_WAIT;
      #else
      waterTempC = -127.0;
      cyc.step = ST_LDR;
      #endif
      return false;

    case ST_DS_WAIT: {
      if (!reached(now, cyc.nextMs)) {
        return false;
      }
      const float temp = ds18b20ReadConversion();
      if (!isnan(temp)) {
        cyc.dsTotal += temp;
        cyc.dsValid++;
      }
      if (++cyc.dsCount < DS18B20_CONVERSIONS) {
        cyc.step = ST_DS_START;
        return false;
      }
      if (cyc.dsValid == 0) {
        Serial.println("[WARN] DS18B20 returned no valid readings");
        waterTempC = -127.0;
      } else {
        waterTempC = cyc.dsTotal / cyc.dsValid;
      }
      Serial.print("[DEBUG] DS18B20 result - ");
      Serial.print(waterTempC);
      Serial.println(" C");
      cyc.step = ST_LDR;
      return false;
    }

    case ST_LDR:
      readLdrSensor();
      cyc.step = ST_TDS_POWER;
      return false;

    case ST_TDS_POWER:
      setTdsPower(true);
      cyc.nextMs = now + TDS_POWER_WARMUP_MS;
      cyc.step = ST_TDS;
      return false;

    case ST_TDS:
      if (!tdsPowerOn()) {
        cyc.step = ST_TDS_POWER;  // a pH capture switched it off mid warm-up
        return false;
      }
      if (!reached(now, cyc.nextMs) || waitForRail(now)) {
        return false;
      }
      readTdsSensor();
      setTdsPower(false);
      cyc.nextMs = now + TDS_POWER_QUIET_MS;
      cyc.step = ST_PH;
      return false;

    case ST_PH:
      if (!reached(now, cyc.nextMs) || waitForRail(now)) {
        return false;
      }
      readPhSensorValues();
      printAnalogSensors();
      cyc.step = ST_DHT;
      cyc.nextMs = now;
      return false;

    case ST_DHT:
      #if ENABLE_AMBIENT_SENSORS
      if (!reached(now, cyc.nextMs)) {
        return false;
      }
      cyc.dhtAttempt++;
      if (readAmbientDHT11Once(cyc.dhtAttempt)) {
        cyc.step = ST_CO2_START;
      } else if (cyc.dhtAttempt < DHT_ATTEMPTS) {
        cyc.nextMs = now + DHT_RETRY_MS;
      } else {
        ambientTempC = -127.0;
        ambientHumidityPct = -1.0;
        Serial.println("[WARN] DHT11 returned no valid readings");
        cyc.step = ST_CO2_START;
      }
      #else
      ambientTempC = -127.0;
      ambientHumidityPct = -1.0;
      cyc.step = ST_CO2_START;
      #endif
      return false;

    case ST_CO2_START:
      #if ENABLE_CO2
      if (co2StartSingleShot()) {
        cyc.nextMs = now + CO2_FIRST_POLL_MS;
        cyc.co2DeadlineMs = cyc.nextMs + CO2_SINGLE_SHOT_TIMEOUT_MS;
        cyc.step = ST_CO2_POLL;
      } else {
        co2PPM = -1;
        cyc.step = ST_DONE;
      }
      #else
      co2PPM = -1;
      cyc.step = ST_DONE;
      #endif
      return false;

    case ST_CO2_POLL: {
      if (!reached(now, cyc.nextMs)) {
        return false;
      }
      if (reached(now, cyc.co2DeadlineMs)) {
        printCo2Timeout(cyc.co2CommFails);
        co2PPM = -1;
        cyc.step = ST_DONE;
        return false;
      }
      switch (co2PollReady()) {
        case CO2_POLL_READY:
          cyc.step = ST_CO2_READ;
          cyc.nextMs = now;
          break;
        case CO2_POLL_WAIT:
          cyc.nextMs = now + CO2_SINGLE_SHOT_POLL_MS;
          break;
        case CO2_POLL_COMM_RETRY:
          cyc.co2CommFails++;
          cyc.nextMs = now + I2C_RETRY_MS;
          break;
        case CO2_POLL_ERROR:
          co2PPM = -1;
          cyc.step = ST_DONE;
          break;
      }
      return false;
    }

    case ST_CO2_READ: {
      if (!reached(now, cyc.nextMs)) {
        return false;
      }
      const int ppm = co2ReadPpm();
      if (ppm == -2 && !cyc.co2Retried) {
        cyc.co2Retried = true;
        cyc.nextMs = now + I2C_RETRY_MS;
        return false;
      }
      co2PPM = ppm < 0 ? -1 : ppm;
      Serial.print("[DEBUG] CO2 result - ");
      Serial.print(co2PPM);
      Serial.println(" ppm");
      cyc.step = ST_DONE;
      return false;
    }

    case ST_DONE:
      cyc.step = ST_IDLE;
      return true;
  }
  return false;
}

void serviceWaterLevelPriorityPoll(unsigned long now) {
  #if ENABLE_WATER_LEVEL
  // Not during the cycle's own level read, and not while the CO2 emitter is
  // measuring (its current ramp can disturb the shared I2C bus).
  const bool busy = cyc.step == ST_LEVEL || cyc.step == ST_CO2_POLL || cyc.step == ST_CO2_READ;
  if (!priorityLevel.active) {
    if (busy || now - lastWaterLevelPriorityPollMs < WATER_LEVEL_PRIORITY_POLL_MS) {
      return;
    }
    startLevel(priorityLevel, now);
  }
  if (busy) {
    return;
  }
  int raw = -1;
  if (stepLevel(priorityLevel, now, raw)) {
    afterLevel(raw, false, now);
  }
  #else
  (void)now;
  #endif
}
