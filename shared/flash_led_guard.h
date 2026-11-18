/************************************************************
 * FlashLedGuard — keep flash access and WS2812B frames apart
 *
 * FastLED drives the grow light through the RMT peripheral with a small
 * ping-pong buffer that an ISR on the loop core refills every ~120 us. This
 * ESP32 core is built without CONFIG_RMT_TX_ISR_CACHE_SAFE, so while any
 * flash read, write or erase runs (cache disabled on both cores) that ISR
 * cannot run: the buffer underruns mid-frame and the rest of the bits land on
 * the wrong pixels — a brief flash of random colours. A LittleFS append or
 * cursor write measured 8–350 ms on this board (sector erase + metadata).
 *
 * One mutex, two roles:
 *   LED side    tryBeginFrame() — never waits; a frame is skipped while a
 *               file operation runs and retried on the next light update
 *   flash side  lockFlash()     — waits at most one frame (~4 ms, 108 LEDs)
 *
 * Only file access that can run on the other core (network / Influx tasks)
 * needs lockFlash(). Loop-task file access cannot overlap a frame because
 * GrowLight waits for the frame to finish before returning to the loop.
 ************************************************************/

#ifndef SMART_GARDEN_FLASH_LED_GUARD_H
#define SMART_GARDEN_FLASH_LED_GUARD_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace FlashLedGuard {

inline SemaphoreHandle_t handle() {
  static SemaphoreHandle_t mutex = nullptr;
  if (mutex == nullptr) {
    static portMUX_TYPE initMux = portMUX_INITIALIZER_UNLOCKED;
    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    portENTER_CRITICAL(&initMux);
    if (mutex == nullptr) {
      mutex = created;
      created = nullptr;
    }
    portEXIT_CRITICAL(&initMux);
    if (created != nullptr) {
      vSemaphoreDelete(created);  // another task won the race
    }
  }
  return mutex;
}

// LED side: true when the frame may go out now; pair with endFrame().
inline bool tryBeginFrame() {
  return xSemaphoreTake(handle(), 0) == pdTRUE;
}

inline void endFrame() {
  xSemaphoreGive(handle());
}

// Flash side: hold across a whole LittleFS operation.
inline void lockFlash() {
  xSemaphoreTake(handle(), portMAX_DELAY);
}

inline void unlockFlash() {
  xSemaphoreGive(handle());
}

}  // namespace FlashLedGuard

#endif  // SMART_GARDEN_FLASH_LED_GUARD_H
