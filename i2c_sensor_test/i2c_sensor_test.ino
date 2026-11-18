#include <Wire.h>
#include <LittleFS.h>
#include "sensor_data_store.h"

#define I2C_SDA 21
#define I2C_SCL 22

const byte WATER_LEVEL_ADDR = 0x77;
const byte WATER_LEVEL_ADDR_2 = 0x78;
const byte TDS_ADDR = 0x76; // common Grove-compatible address placeholder
const byte CO2_ADDR = 0x28; // XENSIV PAS CO2 I2C address

bool sensorStoreReady = false;

bool deviceExists(byte address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

void scanI2CDevices() {
  byte error;
  byte address;
  int deviceCount = 0;

  Serial.println("[TEST] Scanning I2C bus...");

  for (address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    error = Wire.endTransmission();

    if (error == 0) {
      deviceCount++;
      Serial.print("[TEST] Device found at 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
    }
  }

  if (deviceCount == 0) {
    Serial.println("[TEST] No I2C devices detected. Check pull-ups, GND, VCC, and wiring.");
  } else {
    Serial.print("[TEST] Found ");
    Serial.print(deviceCount);
    Serial.println(" device(s)");
  }
}

void testWaterLevel() {
  byte data[8] = {0};
  byte data2[12] = {0};

  Wire.requestFrom(WATER_LEVEL_ADDR, 8);
  for (int i = 0; i < 8 && Wire.available(); i++) {
    data[i] = Wire.read();
  }

  Wire.requestFrom(WATER_LEVEL_ADDR_2, 12);
  for (int i = 0; i < 12 && Wire.available(); i++) {
    data2[i] = Wire.read();
  }

  int touched = 0;
  for (int i = 0; i < 8; i++) {
    if (data[i] > 100 && data[i] < 0xFE) touched++;
  }
  for (int i = 0; i < 12; i++) {
    if (data2[i] > 100 && data2[i] < 0xFE) touched++;
  }

  int percent = map(touched, 0, 20, 0, 100);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;

  Serial.print("[TEST] Water level raw touch count: ");
  Serial.print(touched);
  Serial.print(" -> percent: ");
  Serial.println(percent);

  // Keep the shared trust file current; CSV only on change.
  if (sensorStoreReady && percent > 0) {
    static int lastLoggedPercent = -1;
    SensorDataStore::recordWaterLevel(percent);
    if (percent != lastLoggedPercent) {
      lastLoggedPercent = percent;
      SensorDataStore::appendCsvRow("i2c_sensor_test", percent);
    }
  }
}

void testTDS() {
  Serial.print("[TEST] TDS/EC expected address: 0x");
  Serial.println(TDS_ADDR, HEX);
  if (deviceExists(TDS_ADDR)) {
    Serial.println("[TEST] TDS/EC sensor responded on the expected address");
  } else {
    Serial.println("[TEST] TDS/EC sensor did not respond on the expected address");
  }
}

void testCO2() {
  Serial.print("[TEST] CO2 expected address: 0x");
  Serial.println(CO2_ADDR, HEX);
  if (deviceExists(CO2_ADDR)) {
    Serial.println("[TEST] CO2 sensor responded on the expected address");
  } else {
    Serial.println("[TEST] CO2 sensor did not respond on the expected address");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println("[TEST] I2C sensor test sketch starting");
  sensorStoreReady = SensorDataStore::begin(true);
  scanI2CDevices();
  delay(1000);
}

void loop() {
  Serial.println();
  Serial.println("[TEST] --- Running I2C tests ---");
  testWaterLevel();
  testTDS();
  testCO2();
  delay(5000);
}
