/************************************************************
 * Ambient daughterboard sensors — DHT11, LDR, PAS CO2 (I2C)
 *
 * Used by embedded-smart-garden.ino and ambient_sensor_test.ino.
 * CO2 shares I2C with the Grove water-level stick on the main board.
 ************************************************************/

#ifndef SMART_GARDEN_AMBIENT_SENSORS_H
#define SMART_GARDEN_AMBIENT_SENSORS_H

#include <Arduino.h>
#include <Wire.h>
#include <DHT.h>
#include <pas-co2-ino.hpp>

namespace AmbientSensors {

constexpr int DHT11_PIN = 16;   // RX2 — ambient harness wire 3
constexpr int LDR_PIN = 34;     // ambient harness wire 4
constexpr int I2C_SDA = 21;
constexpr int I2C_SCL = 22;

constexpr int ANALOG_SAMPLE_COUNT = 10;
constexpr unsigned long CO2_SINGLE_SHOT_TIMEOUT_MS = 15000;
constexpr unsigned long CO2_SINGLE_SHOT_POLL_MS = 100;
constexpr uint16_t CO2_MAX_PPM = 32000;

struct Reading {
  float ambientTempC = NAN;
  float ambientHumidityPct = NAN;
  int lightAdc = -1;
  int co2Ppm = -1;
  bool dhtOk = false;
  bool ldrOk = false;
  bool co2Ok = false;
};

inline const char* co2ErrorName(Error_t err) {
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

inline void scanI2C(TwoWire& wire) {
  byte error;
  int deviceCount = 0;
  Serial.println("[AMBIENT] Scanning I2C bus...");
  for (byte address = 1; address < 127; address++) {
    wire.beginTransmission(address);
    error = wire.endTransmission();
    if (error == 0) {
      deviceCount++;
      Serial.print("[AMBIENT] Device at 0x");
      if (address < 16) {
        Serial.print('0');
      }
      Serial.println(address, HEX);
    }
  }
  if (deviceCount == 0) {
    Serial.println("[AMBIENT] No I2C devices — check harness, pull-ups, CO2 power");
  }
}

inline bool beginDht(DHT& dht) {
  dht.begin();
  pinMode(DHT11_PIN, INPUT);
  Serial.println("[AMBIENT] DHT11 on GPIO 16 (external 10k pull-up)");
  return true;
}

inline bool beginCo2(PASCO2Ino& co2) {
  Serial.println("[AMBIENT] Initialising PAS CO2...");
  if (co2.begin() == XENSIV_PASCO2_OK) {
    Serial.println("[AMBIENT] CO2 ready (single-shot per read)");
    return true;
  }
  Serial.println("[AMBIENT] CO2 init failed — check 3.3 V, 5 V emitter, PSEL→GND");
  return false;
}

inline void begin(TwoWire& wire, DHT& dht, PASCO2Ino& co2) {
  wire.begin(I2C_SDA, I2C_SCL);
  Serial.print("[AMBIENT] I2C SDA=");
  Serial.print(I2C_SDA);
  Serial.print(" SCL=");
  Serial.println(I2C_SCL);
  scanI2C(wire);
  beginDht(dht);
  beginCo2(co2);
}

inline int readLdrAdc() {
  long total = 0;
  for (int i = 0; i < ANALOG_SAMPLE_COUNT; i++) {
    total += analogRead(LDR_PIN);
    delay(10);
  }
  return static_cast<int>(total / ANALOG_SAMPLE_COUNT);
}

inline bool readDhtAverage(DHT& dht, float& tempC, float& humidityPct) {
  float tempTotal = 0.0f;
  float humidityTotal = 0.0f;
  int validCount = 0;

  for (int i = 0; i < 3; i++) {
    const float temp = dht.readTemperature();
    const float humidity = dht.readHumidity();
    if (!isnan(temp) && !isnan(humidity)) {
      tempTotal += temp;
      humidityTotal += humidity;
      validCount++;
      Serial.print("[AMBIENT] DHT attempt ");
      Serial.print(i + 1);
      Serial.print(" → ");
      Serial.print(temp, 1);
      Serial.print(" °C, ");
      Serial.print(humidity, 1);
      Serial.println(" %RH");
    } else {
      Serial.print("[AMBIENT] DHT attempt ");
      Serial.print(i + 1);
      Serial.println(" failed");
    }
    delay(2000);
  }

  if (validCount == 0) {
    return false;
  }
  tempC = tempTotal / static_cast<float>(validCount);
  humidityPct = humidityTotal / static_cast<float>(validCount);
  return true;
}

inline int readCo2Ppm(PASCO2Ino& co2) {
  Error_t co2Error = co2.startMeasure();
  if (co2Error != XENSIV_PASCO2_OK) {
    Serial.print("[AMBIENT] CO2 start failed ");
    Serial.print(co2Error);
    Serial.print(" ");
    Serial.println(co2ErrorName(co2Error));
    return -1;
  }

  delay(200);

  const unsigned long deadline = millis() + CO2_SINGLE_SHOT_TIMEOUT_MS;
  bool dataReady = false;
  while (static_cast<long>(millis() - deadline) < 0) {
    uint8_t measSts = 0;
    co2Error = co2.getRegister(XENSIV_PASCO2_REG_MEAS_STS, &measSts, 1);
    if (co2Error == XENSIV_PASCO2_ERR_COMM) {
      delay(150);
      continue;
    }
    if (co2Error != XENSIV_PASCO2_OK) {
      Serial.print("[AMBIENT] CO2 status read failed ");
      Serial.println(co2ErrorName(co2Error));
      return -1;
    }
    if ((measSts & XENSIV_PASCO2_REG_MEAS_STS_DRDY_MSK) != 0) {
      dataReady = true;
      break;
    }
    delay(CO2_SINGLE_SHOT_POLL_MS);
  }

  if (!dataReady) {
    Serial.println("[AMBIENT] CO2 timed out waiting for DRDY");
    return -1;
  }

  uint8_t ppmBytes[2] = {0, 0};
  co2Error = co2.getRegister(XENSIV_PASCO2_REG_CO2PPM_H, ppmBytes, 2);
  if (co2Error == XENSIV_PASCO2_ERR_COMM) {
    delay(150);
    co2Error = co2.getRegister(XENSIV_PASCO2_REG_CO2PPM_H, ppmBytes, 2);
  }
  if (co2Error != XENSIV_PASCO2_OK) {
    Serial.println("[AMBIENT] CO2 ppm register read failed");
    return -1;
  }

  const uint16_t ppmU =
      (static_cast<uint16_t>(ppmBytes[0]) << 8) | static_cast<uint16_t>(ppmBytes[1]);
  if (ppmU == 0 || ppmU > CO2_MAX_PPM) {
    Serial.print("[AMBIENT] CO2 invalid ppm ");
    Serial.println(ppmU);
    return -1;
  }
  return static_cast<int>(ppmU);
}

inline Reading readAll(DHT& dht, PASCO2Ino& co2) {
  Reading r;

  r.lightAdc = readLdrAdc();
  r.ldrOk = r.lightAdc >= 0;
  Serial.print("[AMBIENT] LDR ADC ");
  Serial.println(r.lightAdc);

  r.dhtOk = readDhtAverage(dht, r.ambientTempC, r.ambientHumidityPct);

  const int ppm = readCo2Ppm(co2);
  if (ppm >= 0) {
    r.co2Ppm = ppm;
    r.co2Ok = true;
    Serial.print("[AMBIENT] CO2 ");
    Serial.print(ppm);
    Serial.println(" ppm");
  }

  return r;
}

}  // namespace AmbientSensors

#endif  // SMART_GARDEN_AMBIENT_SENSORS_H
