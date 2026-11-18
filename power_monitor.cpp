/* Power / brownout monitor — RTC counters live in this TU only. */
#include "garden.h"

namespace PowerMonitor {

namespace {

constexpr uint32_t RTC_MAGIC = 0xB0A7C0DEul;

RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcBootCount;
RTC_NOINIT_ATTR uint32_t rtcBrownoutCount;
RTC_NOINIT_ATTR uint32_t rtcLastResetReason;

esp_reset_reason_t bootReason = ESP_RST_UNKNOWN;
uint32_t bootCount = 0;
uint32_t brownoutCount = 0;

const char* reasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "ext_pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

}  // namespace

void begin() {
  bootReason = esp_reset_reason();

  if (rtcMagic != RTC_MAGIC) {
    rtcMagic = RTC_MAGIC;
    rtcBootCount = 0;
    rtcBrownoutCount = 0;
    rtcLastResetReason = 0;
  }

  rtcBootCount++;
  rtcLastResetReason = static_cast<uint32_t>(bootReason);
  if (bootReason == ESP_RST_BROWNOUT) {
    rtcBrownoutCount++;
  }

  bootCount = rtcBootCount;
  brownoutCount = rtcBrownoutCount;

#if POWER_SENSE_PIN >= 0
  pinMode(POWER_SENSE_PIN, INPUT);
  analogSetPinAttenuation(POWER_SENSE_PIN, ADC_11db);
#endif

  Serial.print("[POWER] Reset reason - ");
  Serial.print(lastResetReasonName());
  Serial.print(" (");
  Serial.print(static_cast<int>(bootReason));
  Serial.print("); boot_count=");
  Serial.print(bootCount);
  Serial.print("; brownout_count=");
  Serial.print(brownoutCount);
  Serial.print("; free_heap=");
  Serial.print(ESP.getFreeHeap());
  Serial.println(" B");
#if POWER_SENSE_PIN < 0
  Serial.println("[POWER] Rail ADC sense disabled (POWER_SENSE_PIN=-1). "
                 "Wire VIN÷2 to a free ADC GPIO to monitor supply voltage.");
#endif
}

const char* lastResetReasonName() {
  return reasonName(bootReason);
}

bool lastResetWasBrownout() {
  return bootReason == ESP_RST_BROWNOUT;
}

uint32_t bootCountValue() {
  return bootCount;
}

uint32_t brownoutCountValue() {
  return brownoutCount;
}

int readRailMv() {
#if POWER_SENSE_PIN < 0
  return -1;
#else
  uint32_t sum = 0;
  const int n = 8;
  for (int i = 0; i < n; i++) {
    sum += analogReadMilliVolts(POWER_SENSE_PIN);
    delay(2);
  }
  const float gpioMv = static_cast<float>(sum) / static_cast<float>(n);
  const int railMv =
      static_cast<int>(gpioMv * POWER_SENSE_DIVIDER_GAIN + 0.5f);
  if (railMv < 500 || railMv > 6000) {
    return -1;
  }
  return railMv;
#endif
}

int freeHeapBytes() {
  return static_cast<int>(ESP.getFreeHeap());
}

int wifiRssiDbm() {
  if (WiFi.status() != WL_CONNECTED) {
    return 0;
  }
  return WiFi.RSSI();
}

bool wifiRssiValid() {
  return WiFi.status() == WL_CONNECTED;
}

void printStatus() {
  Serial.println("[POWER] --- supply / brownout ---");
  Serial.print("  last_reset=");
  Serial.println(lastResetReasonName());
  Serial.print("  boot_count=");
  Serial.println(bootCount);
  Serial.print("  brownout_count=");
  Serial.println(brownoutCount);
  Serial.print("  uptime_ms=");
  Serial.println(millis());
  Serial.print("  free_heap_b=");
  Serial.println(freeHeapBytes());
  if (wifiRssiValid()) {
    Serial.print("  wifi_rssi_dbm=");
    Serial.println(wifiRssiDbm());
  } else {
    Serial.println("  wifi_rssi_dbm=(not connected)");
  }
  const int mv = readRailMv();
  if (mv >= 0) {
    Serial.print("  rail_mv=");
    Serial.println(mv);
  } else {
    Serial.println("  rail_mv=(no sense pin — set POWER_SENSE_PIN)");
  }
}

}  // namespace PowerMonitor
