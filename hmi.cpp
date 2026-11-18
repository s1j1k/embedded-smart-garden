/* Front-panel HMI: ON/OFF soft switch and the three status LEDs.
 *
 *   ON  button  GPIO13   momentary to GND (internal pull-up)
 *   OFF button  GPIO14   momentary to GND (internal pull-up)
 *   POWER   LED GPIO25   green  — firmware running (switched ON)
 *   RUNNING LED GPIO26   yellow — a pump is drawing
 *   ERROR   LED GPIO33   red    — green + red: running with a fault
 *                                 red alone:   critical fault, ON refused
 *
 * The same switch works over MQTT ({"cmd":"power","action":"on|off|status"},
 * acked on the ack topic) for when the panel is out of reach.
 *
 * The .ino owns the power state (what starts and stops); this file reads the
 * buttons and MQTT power requests, keeps the critical-fault list and drives
 * the LEDs. See README "Front panel (HMI)".
 */
#include "garden.h"

namespace {

constexpr unsigned long DEBOUNCE_MS = 30;
// Health scan for the red LED (builds the dashboard health JSON).
constexpr unsigned long ERROR_SCAN_MS = 2000;
// A button held at power-up must read released this long to clear the fault.
constexpr unsigned long STUCK_CLEAR_MS = 2000;

struct Button {
  int pin;
  bool pressed;  // debounced
  bool lastRaw;
  unsigned long changedAt;
};

Button btnOn = {HMI_BTN_ON_PIN, false, false, 0};
Button btnOff = {HMI_BTN_OFF_PIN, false, false, 0};

bool systemOn = false;
bool onRequest = false;
bool offRequest = false;
const char* requestVia = "panel_button";

// Set by the network task (Hmi::requestRemote), taken by service().
volatile bool remoteOnPending = false;
volatile bool remoteOffPending = false;
volatile bool remoteStatusPending = false;

bool buttonStuck = false;
unsigned long buttonsReleasedSince = 0;
const char* criticalReason = nullptr;  // other than a stuck button

bool errorActive = false;
unsigned long lastErrorScanMs = 0;
String lastFaultList;

bool rawPressed(int pin) {
  return digitalRead(pin) == LOW;
}

// True on the pass the button becomes pressed (debounced).
bool pressEdge(Button& b, unsigned long now) {
  const bool raw = rawPressed(b.pin);
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.changedAt = now;
  }
  if (raw != b.pressed && now - b.changedAt >= DEBOUNCE_MS) {
    b.pressed = raw;
    return raw;
  }
  return false;
}

bool takeFlag(volatile bool& f) {
  if (!f) return false;
  f = false;
  return true;
}

void ackPower(bool ok, const char* message) {
#if ENABLE_MQTT
  publishPowerAck(ok, message);
#else
  (void)ok;
  (void)message;
#endif
}

bool pumpActive() {
  return sharedRailPumpDrawing() || waterFillManualPumpRunning || manualPrimeActive;
}

// Red LED while running: any device the dashboard shows as "fault".
void scanHealthFaults() {
  JsonDocument doc;
  addHealthJson(doc);
  String faults;
  for (JsonPair kv : doc["health"].as<JsonObject>()) {
    if (strcmp(kv.value().as<const char*>(), "fault") == 0) {
      if (faults.length() > 0) faults += ", ";
      faults += kv.key().c_str();
    }
  }
  errorActive = faults.length() > 0;
  if (faults != lastFaultList) {
    Serial.print("[HMI] Error LED ");
    if (errorActive) {
      Serial.print("on — fault: ");
      Serial.println(faults);
    } else {
      Serial.println("off — no device faults");
    }
    lastFaultList = faults;
  }
}

void writeLeds(unsigned long now) {
  const bool critical = Hmi::criticalFault() != nullptr;
  if (systemOn && now - lastErrorScanMs >= ERROR_SCAN_MS) {
    lastErrorScanMs = now;
    scanHealthFaults();
  }
  digitalWrite(HMI_LED_POWER_PIN, systemOn ? HIGH : LOW);
  digitalWrite(HMI_LED_RUNNING_PIN, systemOn && pumpActive() ? HIGH : LOW);
  digitalWrite(HMI_LED_ERROR_PIN, critical || (systemOn && errorActive) ? HIGH : LOW);
}

}  // namespace

namespace Hmi {

void begin() {
  pinMode(HMI_BTN_ON_PIN, INPUT_PULLUP);
  pinMode(HMI_BTN_OFF_PIN, INPUT_PULLUP);
  pinMode(HMI_LED_POWER_PIN, OUTPUT);
  pinMode(HMI_LED_RUNNING_PIN, OUTPUT);
  pinMode(HMI_LED_ERROR_PIN, OUTPUT);
  digitalWrite(HMI_LED_POWER_PIN, LOW);
  digitalWrite(HMI_LED_RUNNING_PIN, LOW);
  digitalWrite(HMI_LED_ERROR_PIN, LOW);
  delay(5);  // pull-ups settle

  btnOn.lastRaw = btnOn.pressed = rawPressed(HMI_BTN_ON_PIN);
  btnOff.lastRaw = btnOff.pressed = rawPressed(HMI_BTN_OFF_PIN);
  buttonStuck = btnOn.pressed || btnOff.pressed;
  if (buttonStuck) {
    Serial.println("[HMI] CRITICAL: a panel button reads pressed at power-up "
                   "(stuck or shorted) — ON refused until both read released");
  }
  writeLeds(millis());
}

void service(unsigned long now) {
  const bool onEdge = pressEdge(btnOn, now);
  const bool offEdge = pressEdge(btnOff, now);

  if (buttonStuck) {
    if (btnOn.pressed || btnOff.pressed) {
      buttonsReleasedSince = 0;
    } else if (buttonsReleasedSince == 0) {
      buttonsReleasedSince = now;
    } else if (now - buttonsReleasedSince >= STUCK_CLEAR_MS) {
      buttonStuck = false;
      Serial.println("[HMI] Panel buttons released — stuck-button fault cleared");
    }
  }

  // OFF wins if both are pressed together.
  if (offEdge) {
    offRequest = true;
    onRequest = false;
    requestVia = "panel_button";
  } else if (onEdge) {
    const char* why = criticalFault();
    if (why != nullptr) {
      Serial.print("[HMI] ON refused — critical fault: ");
      Serial.println(why);
    } else if (!systemOn) {
      onRequest = true;
      requestVia = "panel_button";
    }
  }

  // MQTT power requests, after the buttons so a panel press is not overridden.
  if (takeFlag(remoteStatusPending)) {
    const char* why = criticalFault();
    ackPower(why == nullptr, why != nullptr ? why : (systemOn ? "on" : "standby"));
  }
  const bool remoteOff = takeFlag(remoteOffPending);
  const bool remoteOn = takeFlag(remoteOnPending);
  // OFF wins here too.
  if (remoteOff) {
    onRequest = false;
    if (systemOn) {
      Serial.println("[HMI] OFF requested over MQTT");
      offRequest = true;
      requestVia = "mqtt";
    } else {
      ackPower(true, "already in standby");
    }
  } else if (remoteOn && !offRequest) {
    const char* why = criticalFault();
    if (why != nullptr) {
      Serial.print("[HMI] MQTT ON refused — critical fault: ");
      Serial.println(why);
      ackPower(false, why);
    } else if (systemOn) {
      ackPower(true, "already on");
    } else if (!onRequest) {
      Serial.println("[HMI] ON requested over MQTT");
      onRequest = true;
      requestVia = "mqtt";
    }
  }
  writeLeds(now);
}

bool takeOnRequest() {
  const bool r = onRequest;
  onRequest = false;
  return r;
}

bool takeOffRequest() {
  const bool r = offRequest;
  offRequest = false;
  return r;
}

void requestRemote(const char* action) {
  if (strcmp(action, "on") == 0) {
    remoteOnPending = true;
  } else if (strcmp(action, "off") == 0) {
    remoteOffPending = true;
  } else {
    remoteStatusPending = true;
  }
}

const char* requestSource() {
  return requestVia;
}

void setSystemOn(bool on) {
  systemOn = on;
  if (!on) {
    errorActive = false;
    lastFaultList = "";
  } else {
    lastErrorScanMs = 0;  // first scan on the next pass
  }
  writeLeds(millis());
}

bool isSystemOn() {
  return systemOn;
}

void setCriticalFault(const char* reason) {
  criticalReason = reason;
  if (reason != nullptr) {
    Serial.print("[HMI] CRITICAL: ");
    Serial.println(reason);
  }
  writeLeds(millis());
}

const char* criticalFault() {
  if (buttonStuck) return "panel button stuck or shorted at power-up";
  return criticalReason;
}

}  // namespace Hmi
