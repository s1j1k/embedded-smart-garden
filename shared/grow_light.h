/************************************************************
 * WS2812B grow light — blurple photoperiod for leafy greens
 *
 * Basil & lettuce (vegetative): strong blue (450 nm) for compact
 * growth + red (660 nm) for photosynthesis; minimal green.
 *
 * Layout: 6 rows × 18 LEDs, serpentine (row ends connect to next row).
 * Even rows run left → right; odd rows run right → left.
 * Photoperiod: one 16 h ON window and one 8 h OFF window per day (see
 * "Photoperiod" below). Uses the NTP clock; without it, runs the same 16/8
 * split counted from boot. Light missed while the garden was off can be
 * repaid after the scheduled OFF ("Make-up light" below; light_makeup.cpp).
 *
 * Power: FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_CURRENT_MA) hard-caps
 * draw so the LED buck is never overloaded. Cap is set below the buck's
 * 3 A rating for thermal headroom; FastLED scales pixel values each frame.
 *
 * Include from embedded-smart-garden.ino:
 *   #define ENABLE_GROW_LIGHT 1
 *   #define GROW_LIGHT_DATA_PIN 27
 *   #include "shared/grow_light.h"
 ************************************************************/

#ifndef SMART_GARDEN_GROW_LIGHT_H
#define SMART_GARDEN_GROW_LIGHT_H

#include <Arduino.h>

#ifndef ENABLE_GROW_LIGHT
#define ENABLE_GROW_LIGHT 1
#endif

#ifndef GROW_LIGHT_DATA_PIN
#define GROW_LIGHT_DATA_PIN 27
#endif

// Must be outside GrowLight — including inside a namespace mangles FastLED
// types into GrowLight::… and breaks linking against the library.
#if ENABLE_GROW_LIGHT
#include <FastLED.h>
#include "flash_led_guard.h"
#endif

namespace GrowLight {

constexpr int LED_COUNT = 108;
constexpr int LEDS_PER_ROW = 18;
constexpr int ROW_COUNT = 6;

// ---- Photoperiod: when the light is on ----------------------------------
// Each day has one ON window then one OFF window, fixed in UTC (the ESP32
// clock is UTC: configTime(0, 0, …)):
//
//   ON   22:00 → 14:00 UTC (16 h)  = 08:00 → 00:00 AEST (+10)
//                                  = 09:00 → 01:00 AEDT (+11, daylight saving)
//   OFF  14:00 → 22:00 UTC  (8 h)  = 00:00 → 08:00 AEST / 01:00 → 09:00 AEDT
//
// So the light being off overnight and early morning is the schedule, not
// a fault. Without NTP time the same 16/8 split runs from boot instead (ON
// for the first 16 h after power-up), and the clock times above do not apply.
constexpr int PHOTOPERIOD_START_HOUR_UTC = 22;  // ON starts; 22:00 UTC = 08:00 AEST
constexpr int PHOTOPERIOD_ON_HOURS = 16;        // higher DLI for a weak strip
constexpr int PHOTOPERIOD_MAX_ON_HOURS = 18;    // plants need a dark period
constexpr int PHOTOPERIOD_OFF_HOUR_UTC =
    (PHOTOPERIOD_START_HOUR_UTC + PHOTOPERIOD_ON_HOURS) % 24;
static_assert(PHOTOPERIOD_ON_HOURS > 0 && PHOTOPERIOD_ON_HOURS <= PHOTOPERIOD_MAX_ON_HOURS,
              "photoperiod must leave a dark period");

constexpr long DAY_S = 24L * 3600L;
constexpr long ON_S = PHOTOPERIOD_ON_HOURS * 3600L;

struct Photoperiod {
  bool wantOn = false;
  bool fromClock = false;     // true: NTP wall clock; false: counted from boot
  long secondsToChange = 0;   // until the next ON <-> OFF switch
  bool makeup = false;        // on only to repay missed light (withMakeup)
  bool manual = false;        // switched on by an operator for a set time (withManual)
};

// cycleS: seconds since the ON window started (any value; wrapped to a day).
inline Photoperiod photoperiodAt(long cycleS, bool fromClock) {
  cycleS %= DAY_S;
  if (cycleS < 0) {
    cycleS += DAY_S;
  }
  Photoperiod p;
  p.fromClock = fromClock;
  p.wantOn = cycleS < ON_S;
  p.secondsToChange = p.wantOn ? ON_S - cycleS : DAY_S - cycleS;
  return p;
}

// ---- Make-up light ---------------------------------------------------------
// Scheduled ON time missed while the garden was off (power cut, or panel
// standby) is owed to the plants. Once an operator starts make-up (Serial
// LIGHT MAKEUP, MQTT light_makeup), the light stays on past the scheduled
// OFF until the owed time is repaid. Each night it may run at most
// MAKEUP_MAX_PER_NIGHT_S, so the day never exceeds PHOTOPERIOD_MAX_ON_HOURS
// of light and the dark period is kept; a larger debt is repaid over several
// nights. Make-up needs the NTP clock: lost time cannot be dated without it.
//
//   ON 22:00 -> 14:00 UTC, make-up 14:00 -> 16:00 UTC (00:00 -> 02:00 AEST)
constexpr long MAKEUP_MAX_PER_NIGHT_S =
    (PHOTOPERIOD_MAX_ON_HOURS - PHOTOPERIOD_ON_HOURS) * 3600L;
// A longer outage still owes at most one day's light.
constexpr long MAKEUP_MAX_OWED_S = ON_S;

// Scheduled ON seconds between the clock epoch and utc (grows with utc).
inline long long scheduledOnSecondsUntil(long long utc) {
  const long long s = utc - PHOTOPERIOD_START_HOUR_UTC * 3600LL;
  long long days = s / DAY_S;
  long long rem = s % DAY_S;
  if (rem < 0) {
    rem += DAY_S;
    days--;
  }
  return days * ON_S + (rem < ON_S ? rem : ON_S);
}

// Scheduled ON seconds in [fromUtc, toUtc): the light the plants missed if
// the garden was off for that span.
inline long missedOnSeconds(long long fromUtc, long long toUtc) {
  if (toUtc <= fromUtc) {
    return 0;
  }
  return static_cast<long>(scheduledOnSecondsUntil(toUtc) - scheduledOnSecondsUntil(fromUtc));
}

// Clock schedule p at cycleS (seconds since the ON window started), with
// make-up applied: during the dark period, on for up to owedS within the
// make-up slot right after the scheduled OFF.
inline Photoperiod withMakeup(Photoperiod p, long cycleS, bool armed, long owedS) {
  if (p.wantOn || !p.fromClock || !armed || owedS <= 0) {
    return p;
  }
  cycleS %= DAY_S;
  if (cycleS < 0) {
    cycleS += DAY_S;
  }
  const long slotLeft = ON_S + MAKEUP_MAX_PER_NIGHT_S - cycleS;
  if (cycleS < ON_S || slotLeft <= 0) {
    return p;
  }
  p.wantOn = true;
  p.makeup = true;
  p.secondsToChange = slotLeft < owedS ? slotLeft : owedS;
  return p;
}

// ---- Manual on ------------------------------------------------------------
// An operator can switch the light on now for a set time (Serial LIGHT ON
// <hours>, MQTT light_on), whatever the schedule says: to give the plants the
// light they missed while the garden was off (the dashboard works it out from
// the gaps in its data), or for work on the garden. It ends by itself and the
// schedule takes over again. At most one day's light at a time.
constexpr long MANUAL_MAX_S = ON_S;

// p with a manual-on override that has leftS seconds to run. The next change
// is when the light next goes off: the end of the override, or the end of a
// scheduled ON window it runs into.
inline Photoperiod withManual(Photoperiod p, long leftS) {
  if (leftS <= 0) {
    return p;
  }
  long offIn = leftS;
  if (p.wantOn) {
    if (p.secondsToChange > offIn) offIn = p.secondsToChange;
  } else if (p.fromClock && p.secondsToChange < offIn) {
    const long scheduledOff = p.secondsToChange + ON_S;
    if (scheduledOff > offIn) offIn = scheduledOff;
  }
  p.wantOn = true;
  p.manual = true;
  p.secondsToChange = offIn;
  return p;
}

// Blurple tuned for basil / lettuce vegetative growth (WS2812B GRB order).
constexpr uint8_t BLURPLE_R = 200;
constexpr uint8_t BLURPLE_G = 0;
constexpr uint8_t BLURPLE_B = 255;

constexpr unsigned long FRAME_INTERVAL_MS = 35;
constexpr unsigned long ANIMATION_DURATION_MS = 5200;
// Steady ON is one colour: re-send it this often rather than every frame.
// WS2812B pixels hold their colour, so this only repairs a stray bad frame;
// fewer frames also means fewer chances to meet a flash operation.
constexpr unsigned long ON_REFRESH_MS = 1000;
// 108 LEDs clock out in ~3.5 ms; this only bounds a stuck driver.
constexpr unsigned long FRAME_WAIT_TIMEOUT_MS = 20;

// Soft ceiling for the 5 V / 3 A LED buck. Uncapped blurple on 108 LEDs
// wants >3 A; FastLED scales each frame to stay under this budget.
// 2200 mA ≈ 27% less than the old 3 A pin — cooler buck, still ~full panel
// look (perceived brightness drops less than linear). Raise toward 3000
// only after a higher-rated buck or measured headroom.
constexpr int MAX_VOLTS = 5;
constexpr int MAX_CURRENT_MA = 2200;

// ---- Camera flash ----------------------------------------------------------
// For a photo the strip goes white for a few seconds, then back to whatever it
// was doing. White is sized from the WS2812B datasheet worst case (20 mA per
// colour channel at full, ~1 mA idle), not FastLED's power estimate (which
// assumes ~42 mA per white LED and would let real draw pass 3 A), so the flash
// stays under the 3 A absolute maximum of the LED buck even on hungry LEDs:
//   108 LEDs × (3 × 20 mA × level/255 + 1 mA) ≤ 2700 mA  →  level 102 (40 %).
constexpr int WORST_MA_PER_CHANNEL = 20;
constexpr int IDLE_MA_PER_LED = 1;
constexpr int FLASH_BUDGET_MA = 2700;  // 10 % under the 3 A buck rating
constexpr uint8_t FLASH_LEVEL = static_cast<uint8_t>(
    (FLASH_BUDGET_MA - LED_COUNT * IDLE_MA_PER_LED) * 255L /
    (LED_COUNT * 3L * WORST_MA_PER_CHANNEL));
static_assert(LED_COUNT * (3L * WORST_MA_PER_CHANNEL * FLASH_LEVEL / 255 + IDLE_MA_PER_LED) <
                  3000,
              "camera flash must stay under the 3 A LED buck");
// FastLED's own estimate of the flash must sit under MAX_CURRENT_MA too, or it
// would dim the flash (16 + 11 + 15 mA per white LED in its model).
static_assert(LED_COUNT * (42L * FLASH_LEVEL / 255 + 1) < MAX_CURRENT_MA,
              "FastLED would scale the flash down");
constexpr unsigned long FLASH_MIN_MS = 500;
constexpr unsigned long FLASH_MAX_MS = 30000;  // ends by itself if "off" never arrives

// Avoid "DISABLED" — ESP32 core defines that as a GPIO macro.
enum class RunState : uint8_t {
  INACTIVE,
  BOOT_ANIMATION,
  ON,
  OFF,
  FADE_IN,
  FADE_OUT
};

struct Status {
  bool enabled = false;
  bool on = false;
  bool animating = false;
  const char* mode = "disabled";
  int brightnessPct = 0;
  int photoperiodOnHours = PHOTOPERIOD_ON_HOURS;
  int scheduleStartHourUtc = PHOTOPERIOD_START_HOUR_UTC;
  int scheduleOffHourUtc = PHOTOPERIOD_OFF_HOUR_UTC;
  bool ntpSchedule = false;   // schedule follows the clock (else counted from boot)
  bool wantOn = false;        // what the photoperiod asks for right now
  long nextChangeS = -1;      // seconds until the next ON <-> OFF switch
  bool flash = false;         // white camera flash overriding the photoperiod
  bool makeup = false;        // lit after the scheduled OFF to repay missed light
  bool makeupArmed = false;   // operator started make-up; runs until owed reaches 0
  long makeupOwedS = 0;       // scheduled light missed while the garden was off
  bool manual = false;        // switched on by an operator (LIGHT ON / light_on)
  long manualLeftS = 0;       // seconds the manual on still runs
};

#if ENABLE_GROW_LIGHT

// Avoid "detail" — clashes with ArduinoJson's private namespace when both are visible.
namespace impl {

inline CRGB* leds() {
  static CRGB pixels[LED_COUNT];
  return pixels;
}

inline uint8_t& outputScale() {
  static uint8_t scale = 0;
  return scale;
}

inline bool& stripReady() {
  static bool ready = false;
  return ready;
}

inline void ensureStrip() {
  if (stripReady()) {
    return;
  }
  FastLED.addLeds<WS2812B, GROW_LIGHT_DATA_PIN, GRB>(leds(), LED_COUNT);
  // Current ceiling for the LED buck. FastLED scales each frame's pixel
  // values so instantaneous draw stays under this budget.
  FastLED.setMaxPowerInVoltsAndMilliamps(MAX_VOLTS, MAX_CURRENT_MA);
  FastLED.setBrightness(255);
  stripReady() = true;
}

// A frame the flash guard skipped; update() re-sends the pixel buffer.
inline bool& framePending() {
  static bool pending = false;
  return pending;
}

// Sends the pixel buffer unless a file operation holds the flash guard
// (shared/flash_led_guard.h); then the frame is retried on the next update.
// Waits for the frame to finish so loop-task file writes after it cannot
// stall the RMT refill ISR mid-frame either.
inline void show() {
  if (!FlashLedGuard::tryBeginFrame()) {
    framePending() = true;
    return;
  }
  FastLED.show();
  FastLED.wait(FRAME_WAIT_TIMEOUT_MS);
  FlashLedGuard::endFrame();
  framePending() = false;
}

inline int serpentineIndex(int row, int col) {
  if (row < 0 || row >= ROW_COUNT || col < 0 || col >= LEDS_PER_ROW) {
    return -1;
  }
  if ((row & 1) == 0) {
    return row * LEDS_PER_ROW + col;
  }
  return row * LEDS_PER_ROW + (LEDS_PER_ROW - 1 - col);
}

inline void setAll(const CRGB& color) {
  fill_solid(leds(), LED_COUNT, color);
}

inline void setBlurple(uint8_t scale = 255) {
  outputScale() = scale;
  CRGB c(BLURPLE_R, BLURPLE_G, BLURPLE_B);
  c.nscale8_video(scale);
  setAll(c);
}

inline void blackout() {
  outputScale() = 0;
  setAll(CRGB::Black);
  show();
}

struct Makeup {
  bool armed = false;
  long owedS = 0;
  unsigned long tickMs = 0;   // last update() while lit for make-up; 0 = not counting
  unsigned long litMs = 0;    // make-up light not yet taken off owedS
};

inline Makeup& makeup() {
  static Makeup m;
  return m;
}

struct Manual {
  bool active = false;
  unsigned long untilMs = 0;
};

inline Manual& manual() {
  static Manual m;
  return m;
}

// Seconds the manual on still runs (0 when off); ends it once the time is up.
inline long manualLeftS() {
  Manual& m = manual();
  if (!m.active) {
    return 0;
  }
  const long leftMs = static_cast<long>(m.untilMs - millis());
  if (leftMs <= 0) {
    m.active = false;
    return 0;
  }
  return (leftMs + 999) / 1000;
}

// Seconds since today's ON window started, or -1 without the NTP clock.
inline long clockCycleS(bool ntpValid) {
  if (ntpValid) {
    const time_t nowUtc = time(nullptr);
    if (nowUtc > 1700000000) {  // clock set (not 1970)
      const long secOfDay = static_cast<long>(nowUtc % DAY_S);
      long c = secOfDay - PHOTOPERIOD_START_HOUR_UTC * 3600L;
      return c < 0 ? c + DAY_S : c;
    }
  }
  return -1;
}

// The schedule alone (telemetry want_on / next_change_s).
inline Photoperiod photoperiodNow(bool ntpValid) {
  const long cycleS = clockCycleS(ntpValid);
  if (cycleS >= 0) {
    return photoperiodAt(cycleS, true);
  }
  return photoperiodAt(static_cast<long>(millis() / 1000UL), false);
}

// What the strip should do: the schedule plus any make-up or manual light.
inline Photoperiod lightWanted(bool ntpValid) {
  const long cycleS = clockCycleS(ntpValid);
  if (cycleS < 0) {
    return withManual(photoperiodAt(static_cast<long>(millis() / 1000UL), false),
                      manualLeftS());
  }
  return withManual(
      withMakeup(photoperiodAt(cycleS, true), cycleS, makeup().armed, makeup().owedS),
      manualLeftS());
}

// Takes the time the strip was lit for make-up off the owed time.
inline void countMakeup(unsigned long nowMs, bool litForMakeup) {
  Makeup& m = makeup();
  if (!litForMakeup) {
    m.tickMs = 0;
    return;
  }
  if (m.tickMs != 0) {
    m.litMs += nowMs - m.tickMs;
  }
  m.tickMs = nowMs;
  if (m.litMs >= 1000) {
    const long s = static_cast<long>(m.litMs / 1000);
    m.litMs %= 1000;
    m.owedS = m.owedS > s ? m.owedS - s : 0;
  }
}

inline void renderComet(int headIndex, int tailLength) {
  outputScale() = 255;
  for (int i = 0; i < LED_COUNT; i++) {
    int dist = headIndex - i;
    if (dist < 0 || dist > tailLength) {
      leds()[i] = CRGB::Black;
      continue;
    }
    const uint8_t fade = static_cast<uint8_t>(255 - (dist * 255 / tailLength));
    leds()[i] = CRGB(fade / 6, 0, fade);
  }
}

inline void renderRowWave(int litRows) {
  outputScale() = 255;
  for (int row = 0; row < ROW_COUNT; row++) {
    const uint8_t scale =
        row < litRows ? 255 : (row == litRows ? 128 : 0);
    CRGB c(BLURPLE_R, BLURPLE_G, BLURPLE_B);
    c.nscale8_video(scale);
    for (int col = 0; col < LEDS_PER_ROW; col++) {
      const int idx = serpentineIndex(row, col);
      if (idx < 0) {
        continue;
      }
      leds()[idx] = c;
    }
  }
}

struct State {
  RunState runState = RunState::BOOT_ANIMATION;
  unsigned long lastFrameMs = 0;
  unsigned long animStartedMs = 0;
  unsigned long fadeStartedMs = 0;
  unsigned long lastRefreshMs = 0;  // last steady-ON re-send
  bool initialized = false;
  bool ntpValid = false;
  // Camera flash overlay: the photoperiod state is kept and resumes after.
  bool flashActive = false;
  unsigned long flashStartedMs = 0;
  unsigned long flashMs = 0;
  Status status;
};

inline State& state() {
  static State s;
  return s;
}

inline const char* modeLabel(RunState rs) {
  switch (rs) {
    case RunState::BOOT_ANIMATION: return "animating";
    case RunState::ON: return "on";
    case RunState::OFF: return "off";
    case RunState::FADE_IN: return "fade_in";
    case RunState::FADE_OUT: return "fade_out";
    default: return "disabled";
  }
}

inline void refreshStatus() {
  State& st = state();
  st.status.enabled = true;
  st.status.animating = st.runState == RunState::BOOT_ANIMATION;
  st.status.on = st.runState == RunState::ON ||
                 st.runState == RunState::BOOT_ANIMATION ||
                 st.runState == RunState::FADE_IN;
  st.status.mode = modeLabel(st.runState);
  st.status.brightnessPct = (static_cast<int>(outputScale()) * 100) / 255;
  st.status.photoperiodOnHours = PHOTOPERIOD_ON_HOURS;
  st.status.scheduleStartHourUtc = PHOTOPERIOD_START_HOUR_UTC;
  st.status.scheduleOffHourUtc = PHOTOPERIOD_OFF_HOUR_UTC;
  const Photoperiod p = photoperiodNow(st.ntpValid);
  st.status.ntpSchedule = p.fromClock;
  st.status.wantOn = p.wantOn;
  st.status.nextChangeS = p.secondsToChange;
  st.status.flash = st.flashActive;
  st.status.makeup = st.status.on && lightWanted(st.ntpValid).makeup;
  st.status.makeupArmed = makeup().armed;
  st.status.makeupOwedS = makeup().owedS;
  st.status.manualLeftS = manualLeftS();
  st.status.manual = st.status.manualLeftS > 0;
  if (st.flashActive) {
    st.status.on = true;
    st.status.mode = "flash";
  }
}

inline void renderFlash() {
  outputScale() = FLASH_LEVEL;
  setAll(CRGB(FLASH_LEVEL, FLASH_LEVEL, FLASH_LEVEL));
  show();
}

// Put the strip back to what the photoperiod state was showing before the flash.
// Boot animation and fades redraw on their next frame by themselves.
inline void restoreAfterFlash() {
  switch (state().runState) {
    case RunState::ON:
      setBlurple();
      show();
      break;
    case RunState::OFF:
      blackout();
      break;
    default:
      break;
  }
}

inline void stepAnimation(unsigned long nowMs) {
  State& st = state();
  const unsigned long elapsed = nowMs - st.animStartedMs;

  if (elapsed < 2200) {
    const int head = static_cast<int>((elapsed * (LED_COUNT - 1)) / 2200);
    renderComet(head, 18);
    show();
    return;
  }

  if (elapsed < 3600) {
    const int phase = static_cast<int>(elapsed - 2200);
    const int litRows = 1 + (phase * (ROW_COUNT + 1)) / 1400;
    renderRowWave(litRows > ROW_COUNT ? ROW_COUNT : litRows);
    show();
    return;
  }

  if (elapsed < ANIMATION_DURATION_MS) {
    const int phase = static_cast<int>(elapsed - 3600);
    const uint8_t pulse = static_cast<uint8_t>(
        180 + (75 * phase) / static_cast<int>(ANIMATION_DURATION_MS - 3600));
    setBlurple(pulse);
    show();
    return;
  }

  if (lightWanted(st.ntpValid).wantOn) {
    st.runState = RunState::ON;
    setBlurple();
    show();
  } else {
    st.runState = RunState::OFF;
    blackout();
  }
}

inline void stepFade(unsigned long nowMs, bool fadeIn) {
  State& st = state();
  constexpr unsigned long FADE_MS = 1200;
  const unsigned long elapsed = nowMs - st.fadeStartedMs;
  if (elapsed >= FADE_MS) {
    if (fadeIn) {
      st.runState = RunState::ON;
      setBlurple();
    } else {
      st.runState = RunState::OFF;
      blackout();
    }
    show();
    return;
  }

  const uint8_t scale =
      fadeIn ? static_cast<uint8_t>((elapsed * 255UL) / FADE_MS)
             : static_cast<uint8_t>(255 - (elapsed * 255UL) / FADE_MS);
  setBlurple(scale);
  show();
}

}  // namespace impl

inline void init() {
  impl::State& st = impl::state();
  impl::ensureStrip();
  impl::blackout();
  st.runState = RunState::BOOT_ANIMATION;
  st.animStartedMs = millis();
  st.lastFrameMs = st.animStartedMs;
  st.initialized = true;
  impl::refreshStatus();

  Serial.print("[DEBUG] Grow light init — ");
  Serial.print(LED_COUNT);
  Serial.print(" WS2812B on GPIO ");
  Serial.print(GROW_LIGHT_DATA_PIN);
  Serial.print(", FastLED power cap ");
  Serial.print(MAX_VOLTS);
  Serial.print(" V / ");
  Serial.print(MAX_CURRENT_MA);
  Serial.print(" mA (");
  Serial.print(PHOTOPERIOD_ON_HOURS);
  Serial.print(" h/day photoperiod: ON ");
  Serial.print(PHOTOPERIOD_START_HOUR_UTC);
  Serial.print(":00 -> OFF ");
  Serial.print(PHOTOPERIOD_OFF_HOUR_UTC);
  Serial.println(":00 UTC)");
}

inline void setNtpValid(bool valid) {
  impl::state().ntpValid = valid;
}

// White camera flash for durationMs (clamped to FLASH_MIN_MS..FLASH_MAX_MS),
// then the light returns to its photoperiod. Calling it again extends the flash.
inline bool startFlash(unsigned long durationMs) {
  impl::State& st = impl::state();
  if (!st.initialized) {
    return false;
  }
  if (durationMs < FLASH_MIN_MS) durationMs = FLASH_MIN_MS;
  if (durationMs > FLASH_MAX_MS) durationMs = FLASH_MAX_MS;
  st.flashActive = true;
  st.flashStartedMs = millis();
  st.flashMs = durationMs;
  impl::renderFlash();
  impl::refreshStatus();
  Serial.print("[LIGHT] Camera flash on (white ");
  Serial.print((FLASH_LEVEL * 100) / 255);
  Serial.print("%, max ");
  Serial.print(durationMs);
  Serial.println(" ms)");
  return true;
}

inline void endFlash() {
  impl::State& st = impl::state();
  if (!st.flashActive) {
    return;
  }
  st.flashActive = false;
  impl::restoreAfterFlash();
  impl::refreshStatus();
  Serial.print("[LIGHT] Camera flash off - back to ");
  Serial.println(st.status.mode);
}

inline bool flashActive() {
  return impl::state().flashActive;
}

// Panel OFF: strip dark now. The photoperiod redraws it within
// ON_REFRESH_MS once update() runs again.
inline void switchOff() {
  if (!impl::state().initialized) {
    return;
  }
  if (impl::state().flashActive) {
    impl::state().flashActive = false;
  }
  impl::makeup().tickMs = 0;  // dark from here: standby is not make-up light
  impl::blackout();
  impl::refreshStatus();
}

// Make-up light (see "Make-up light" above). owedS is capped at
// MAKEUP_MAX_OWED_S; armed with nothing owed does nothing.
inline void setMakeup(bool armed, long owedS) {
  impl::Makeup& m = impl::makeup();
  m.armed = armed;
  m.owedS = owedS < 0 ? 0 : (owedS > MAKEUP_MAX_OWED_S ? MAKEUP_MAX_OWED_S : owedS);
  m.litMs = 0;
  if (impl::state().initialized) {
    impl::refreshStatus();
  }
}

inline bool makeupArmed() {
  return impl::makeup().armed;
}

inline long makeupOwedS() {
  return impl::makeup().owedS;
}

// Manual on (see "Manual on" above): on now for seconds (clamped to
// 1..MANUAL_MAX_S), then back to the schedule. 0 or less switches it off.
inline void setManual(long seconds) {
  impl::Manual& m = impl::manual();
  if (seconds <= 0) {
    m.active = false;
  } else {
    if (seconds > MANUAL_MAX_S) seconds = MANUAL_MAX_S;
    m.active = true;
    m.untilMs = millis() + static_cast<unsigned long>(seconds) * 1000UL;
  }
  if (impl::state().initialized) {
    impl::refreshStatus();
  }
}

inline long manualLeftS() {
  return impl::manualLeftS();
}

inline void update(unsigned long nowMs) {
  if (!impl::state().initialized) {
    return;
  }

  impl::State& st = impl::state();
  if (nowMs - st.lastFrameMs < FRAME_INTERVAL_MS) {
    return;
  }
  st.lastFrameMs = nowMs;

  // A frame skipped while flash was busy (incl. flash / blackout transitions
  // outside this function): send the buffer it left behind.
  if (impl::framePending()) {
    impl::show();
  }

  if (st.flashActive) {
    if (nowMs - st.flashStartedMs < st.flashMs) {
      return;  // hold white; the photoperiod resumes when the flash ends
    }
    Serial.println("[LIGHT] Camera flash timed out");
    endFlash();
  }

  const Photoperiod want = impl::lightWanted(st.ntpValid);
  const bool wantOn = want.wantOn;
  // Steady ON and the fade in both count as make-up light (the camera
  // flash is over by here: a held flash returned above).
  impl::countMakeup(nowMs, want.makeup && (st.runState == RunState::ON ||
                                           st.runState == RunState::FADE_IN));

  switch (st.runState) {
    case RunState::BOOT_ANIMATION:
      impl::stepAnimation(nowMs);
      break;

    case RunState::ON:
      if (!wantOn) {
        st.runState = RunState::FADE_OUT;
        st.fadeStartedMs = nowMs;
        impl::stepFade(nowMs, false);
      } else if (nowMs - st.lastRefreshMs >= ON_REFRESH_MS) {
        st.lastRefreshMs = nowMs;
        impl::setBlurple();
        impl::show();
      }
      break;

    case RunState::OFF:
      if (wantOn) {
        st.runState = RunState::FADE_IN;
        st.fadeStartedMs = nowMs;
        impl::stepFade(nowMs, true);
      }
      break;

    case RunState::FADE_IN:
      if (!wantOn) {
        st.runState = RunState::FADE_OUT;
        st.fadeStartedMs = nowMs;
        impl::stepFade(nowMs, false);
      } else {
        impl::stepFade(nowMs, true);
      }
      break;

    case RunState::FADE_OUT:
      if (wantOn) {
        st.runState = RunState::FADE_IN;
        st.fadeStartedMs = nowMs;
        impl::stepFade(nowMs, true);
      } else {
        impl::stepFade(nowMs, false);
      }
      break;

    default:
      break;
  }

  impl::refreshStatus();
}

inline Status status() {
  impl::refreshStatus();
  return impl::state().status;
}

inline bool isOn() {
  return status().on;
}

#else  // ENABLE_GROW_LIGHT

inline void init() {
  Serial.println("[DEBUG] Grow light disabled (ENABLE_GROW_LIGHT=0)");
}

inline void setNtpValid(bool) {}

inline void update(unsigned long) {}

inline bool startFlash(unsigned long) { return false; }

inline void endFlash() {}

inline bool flashActive() { return false; }

inline void switchOff() {}

inline void setMakeup(bool, long) {}

inline bool makeupArmed() { return false; }

inline long makeupOwedS() { return 0; }

inline void setManual(long) {}

inline long manualLeftS() { return 0; }

inline Status status() {
  return Status{};
}

inline bool isOn() {
  return false;
}

#endif  // ENABLE_GROW_LIGHT

}  // namespace GrowLight

#endif  // SMART_GARDEN_GROW_LIGHT_H
