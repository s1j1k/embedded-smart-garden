/* Grow-light make-up runtime: how much scheduled light the plants missed
 * while the garden was off, and repaying it after the scheduled OFF.
 * Rules in shared/grow_light.h ("Make-up light"); this file dates the time
 * off, saves the owed time to LittleFS and takes the operator commands. */
#include "garden.h"
#include <time.h>

#if ENABLE_GROW_LIGHT

static const char* MAKEUP_PATH = "/light_makeup.txt";
// While the garden runs the clock is saved this often, so a power cut is
// dated to within this much.
static const unsigned long ALIVE_SAVE_MS = 5UL * MS_PER_MINUTE;

static uint32_t aliveUtc = 0;        // last time the garden was known to be running
static bool countPending = false;    // garden turned on: count the gap once the clock is set
static unsigned long onSinceMs = 0;  // when it was turned on
static unsigned long lastSaveMs = 0;

static uint32_t clockEpoch() {
  const time_t now = time(nullptr);
  return now > 1700000000 ? static_cast<uint32_t>(now) : 0;  // 0 until NTP has set it
}

static void saveMakeup() {
  File f = LittleFS.open(MAKEUP_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[LIGHT] WARNING: could not save make-up state to LittleFS");
    return;
  }
  f.printf("%lu %ld %d\n", static_cast<unsigned long>(aliveUtc), GrowLight::makeupOwedS(),
           GrowLight::makeupArmed() ? 1 : 0);
  f.close();
}

static void printHours(long s) {
  Serial.print(s / 3600);
  Serial.print("h");
  const long m = (s % 3600) / 60;
  if (m < 10) Serial.print("0");
  Serial.print(m);
  Serial.print("m");
}

static void logMakeupEvent(const char* event, const char* detail) {
  StageController::logEvent(event, buildSensorSnapshot(), "", NAN, NAN, NAN, NAN, detail,
                            currentIsoTimestamp().c_str());
}

void loadLightMakeup() {
  File f = LittleFS.open(MAKEUP_PATH, FILE_READ);
  if (!f) {
    return;  // never saved: nothing owed, and the first gap cannot be dated
  }
  aliveUtc = static_cast<uint32_t>(f.parseInt());
  const long owedS = f.parseInt();
  const bool armed = f.parseInt() == 1;
  f.close();
  GrowLight::setMakeup(armed, owedS);
  if (owedS > 0) {
    Serial.print("[LIGHT] Make-up light owed: ");
    printHours(GrowLight::makeupOwedS());
    Serial.println(armed ? " (make-up on)" : " (LIGHT MAKEUP to repay it)");
  }
}

void lightMakeupGardenOn() {
  countPending = true;
  onSinceMs = millis();
}

void lightMakeupGardenOff() {
  const uint32_t utc = clockEpoch();
  if (utc != 0) {
    aliveUtc = utc;
    saveMakeup();
  }
  countPending = false;
}

void serviceLightMakeup(unsigned long now) {
  const uint32_t utc = clockEpoch();
  if (utc == 0) {
    return;  // without the clock the time off cannot be dated
  }

  if (countPending) {
    countPending = false;
    // The light has been following the schedule since the panel ON, not
    // since the clock was set.
    const uint32_t onUtc = utc - (now - onSinceMs) / 1000UL;
    const long lostS = aliveUtc != 0 ? GrowLight::missedOnSeconds(aliveUtc, onUtc) : 0;
    if (lostS > 0) {
      GrowLight::setMakeup(GrowLight::makeupArmed(), GrowLight::makeupOwedS() + lostS);
      Serial.print("[LIGHT] Garden was off for ");
      printHours(static_cast<long>(onUtc - aliveUtc));
      Serial.print(" — missed ");
      printHours(lostS);
      Serial.print(" of scheduled light; now owed ");
      printHours(GrowLight::makeupOwedS());
      Serial.println(GrowLight::makeupArmed() ? " (make-up on)" : " (LIGHT MAKEUP to repay it)");
      char detail[48];
      snprintf(detail, sizeof(detail), "missed_s=%ld owed_s=%ld", lostS,
               GrowLight::makeupOwedS());
      logMakeupEvent("light_missed", detail);
    }
    aliveUtc = utc;
    saveMakeup();
    lastSaveMs = now;
    return;
  }

  if (GrowLight::makeupArmed() && GrowLight::makeupOwedS() == 0) {
    GrowLight::setMakeup(false, 0);
    Serial.println("[LIGHT] Make-up light done — missed light repaid");
    logMakeupEvent("light_makeup_done", "");
    aliveUtc = utc;
    saveMakeup();
    lastSaveMs = now;
    return;
  }

  if (now - lastSaveMs >= ALIVE_SAVE_MS) {
    aliveUtc = utc;
    saveMakeup();
    lastSaveMs = now;
  }
}

bool lightMakeupCommand(const char* action, const char* via, char* msg, size_t msgLen) {
  const long owedS = GrowLight::makeupOwedS();
  const long nightH = GrowLight::MAKEUP_MAX_PER_NIGHT_S / 3600;
  const int fromUtc = GrowLight::PHOTOPERIOD_OFF_HOUR_UTC;
  const int toUtc = (fromUtc + nightH) % 24;
  bool ok = true;

  if (strcmp(action, "on") == 0) {
    if (owedS <= 0) {
      snprintf(msg, msgLen, "No missed light to make up");
      ok = false;
    } else if (GrowLight::MAKEUP_MAX_PER_NIGHT_S <= 0) {
      snprintf(msg, msgLen, "Make-up off: the photoperiod already uses the maximum hours");
      ok = false;
    } else {
      GrowLight::setMakeup(true, owedS);
      snprintf(msg, msgLen,
               "Make-up on: %ldh%02ldm owed; light stays on up to %ldh after the scheduled "
               "OFF (%02d:00-%02d:00 UTC) until repaid",
               owedS / 3600, (owedS % 3600) / 60, nightH, fromUtc, toUtc);
      logMakeupEvent("light_makeup_on", via);
    }
  } else if (strcmp(action, "off") == 0) {
    GrowLight::setMakeup(false, owedS);
    snprintf(msg, msgLen, "Make-up off: %ldh%02ldm still owed", owedS / 3600,
             (owedS % 3600) / 60);
    logMakeupEvent("light_makeup_off", via);
  } else if (strcmp(action, "clear") == 0) {
    GrowLight::setMakeup(false, 0);
    snprintf(msg, msgLen, "Make-up cleared: nothing owed");
    logMakeupEvent("light_makeup_clear", via);
  } else if (strcmp(action, "status") == 0) {
    snprintf(msg, msgLen, "Make-up %s: %ldh%02ldm owed (%ldh max per night, %02d:00-%02d:00 UTC)",
             GrowLight::makeupArmed() ? "on" : "off", owedS / 3600, (owedS % 3600) / 60, nightH,
             fromUtc, toUtc);
    return true;
  } else {
    snprintf(msg, msgLen, "Unknown action (on | off | clear | status)");
    return false;
  }
  if (ok) {
    saveMakeup();
  }
  return ok;
}

// LIGHT ON <hours> / MQTT light_on: switch the light on now for that long,
// whatever the schedule. hours == 0 uses the owed make-up light; hours < 0
// switches the manual light off (back to the schedule). The light given is
// taken off the owed make-up light.
bool lightOnCommand(float hours, const char* via, char* msg, size_t msgLen) {
  if (hours < 0.0f) {
    const bool was = GrowLight::manualLeftS() > 0;
    GrowLight::setManual(0);
    snprintf(msg, msgLen, was ? "Manual light off: back to the schedule"
                              : "Manual light was not on");
    if (was) {
      logMakeupEvent("light_manual_off", via);
    }
    return true;
  }
  long seconds = hours > 0.0f ? static_cast<long>(hours * 3600.0f + 0.5f)
                              : GrowLight::makeupOwedS();
  if (seconds <= 0) {
    snprintf(msg, msgLen, "No missed light recorded: give the hours (LIGHT ON 4)");
    return false;
  }
  if (seconds > GrowLight::MANUAL_MAX_S) {
    seconds = GrowLight::MANUAL_MAX_S;
  }
  GrowLight::setManual(seconds);
  const long owedS = GrowLight::makeupOwedS();
  if (owedS > 0) {
    GrowLight::setMakeup(GrowLight::makeupArmed(), owedS > seconds ? owedS - seconds : 0);
    saveMakeup();
  }
  snprintf(msg, msgLen, "Grow light on for %ldh%02ldm, then back to the schedule",
           seconds / 3600, (seconds % 3600) / 60);
  char detail[48];
  snprintf(detail, sizeof(detail), "s=%ld via=%s", seconds, via);
  logMakeupEvent("light_manual_on", detail);
  return true;
}

#else  // ENABLE_GROW_LIGHT

void loadLightMakeup() {}
void lightMakeupGardenOn() {}
void lightMakeupGardenOff() {}
void serviceLightMakeup(unsigned long) {}
bool lightMakeupCommand(const char*, const char*, char* msg, size_t msgLen) {
  snprintf(msg, msgLen, "Grow light disabled in firmware");
  return false;
}
bool lightOnCommand(float, const char*, char* msg, size_t msgLen) {
  snprintf(msg, msgLen, "Grow light disabled in firmware");
  return false;
}

#endif  // ENABLE_GROW_LIGHT
