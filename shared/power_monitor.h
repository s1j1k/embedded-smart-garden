/************************************************************
 * Power / brownout health (ESP32)
 *
 * Classic ESP32 has no built-in VDD ADC (unlike ESP8266 getVcc).
 * Without new wiring we publish:
 *   - uptime_ms (existing)
 *   - reset reason (brownout when BOD trips)
 *   - RTC_NOINIT boot_count / brownout_count
 *   - free_heap_b, wifi_rssi_dbm
 *
 * Optional rail sense: VIN/5V → 100 kΩ → POWER_SENSE_PIN → 100 kΩ → GND,
 * then #define POWER_SENSE_PIN 33 (and POWER_SENSE_DIVIDER_GAIN 2.0f).
 ************************************************************/

#ifndef SMART_GARDEN_POWER_MONITOR_H
#define SMART_GARDEN_POWER_MONITOR_H

#include <Arduino.h>
#include <esp_system.h>

#ifndef POWER_SENSE_PIN
#define POWER_SENSE_PIN (-1)
#endif
#ifndef POWER_SENSE_DIVIDER_GAIN
#define POWER_SENSE_DIVIDER_GAIN (2.0f)
#endif

namespace PowerMonitor {

void begin();
void printStatus();

const char* lastResetReasonName();
bool lastResetWasBrownout();
uint32_t bootCountValue();
uint32_t brownoutCountValue();
int readRailMv();
int freeHeapBytes();
int wifiRssiDbm();  // WiFi.RSSI() or 0 if disconnected
bool wifiRssiValid();

}  // namespace PowerMonitor

#endif  // SMART_GARDEN_POWER_MONITOR_H
