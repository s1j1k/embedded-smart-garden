/************************************************************
 * HMI bring-up: ON/OFF buttons and the three panel LEDs.
 *
 *   GPIO13 (D13) = ON button   (green)
 *   GPIO14 (D14) = OFF button  (red)
 *   GPIO25 (D25) = POWER LED   (green)
 *   GPIO26 (D26) = RUNNING LED (yellow)
 *   GPIO33 (D33) = ERROR LED   (red)
 *
 * Buttons: momentary, between the GPIO and GND, internal pull-up
 * (pressed = LOW). If yours go to 3.3 V instead, set BUTTON_ACTIVE_LOW 0
 * and the sketch switches to the internal pull-down.
 *
 * What it does:
 *   1. Boot: lamp test — green, yellow, red for 0.5 s each, then all three.
 *   2. While a button is held its LED lights: ON held -> yellow,
 *      OFF held -> red. Every press and release is printed with its
 *      debounced level, so a bouncing or stuck button shows up on Serial.
 *   3. A short ON press latches green on; a short OFF press turns all off.
 *   Serial (115200): G / Y / R toggle one LED, T reruns the lamp test,
 *   S prints the raw pin levels.
 ************************************************************/

#define BUTTON_ACTIVE_LOW 1

const int BTN_ON_PIN = 13;
const int BTN_OFF_PIN = 14;
const int LED_POWER_PIN = 25;
const int LED_RUNNING_PIN = 26;
const int LED_ERROR_PIN = 33;

const unsigned long DEBOUNCE_MS = 30;

struct Button {
  int pin;
  const char* name;
  bool pressed;           // debounced
  bool lastRaw;
  unsigned long changedAt;
  unsigned long pressedAt;
  unsigned long presses;
};

Button btnOn = {BTN_ON_PIN, "ON", false, false, 0, 0, 0};
Button btnOff = {BTN_OFF_PIN, "OFF", false, false, 0, 0, 0};

bool powerLatched = false;
bool manualG = false, manualY = false, manualR = false;

bool rawPressed(int pin) {
  const int level = digitalRead(pin);
  return BUTTON_ACTIVE_LOW ? level == LOW : level == HIGH;
}

// True on the pass the debounced state changes.
bool updateButton(Button& b, unsigned long now) {
  const bool raw = rawPressed(b.pin);
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.changedAt = now;
  }
  if (raw != b.pressed && now - b.changedAt >= DEBOUNCE_MS) {
    b.pressed = raw;
    if (raw) {
      b.pressedAt = now;
      b.presses++;
    }
    Serial.print("[HMI] ");
    Serial.print(b.name);
    Serial.print(raw ? " pressed" : " released");
    if (!raw) {
      Serial.print(" after ");
      Serial.print(now - b.pressedAt);
      Serial.print(" ms");
    }
    Serial.print(" (count ");
    Serial.print(b.presses);
    Serial.println(")");
    return true;
  }
  return false;
}

void setLeds(bool g, bool y, bool r) {
  digitalWrite(LED_POWER_PIN, g ? HIGH : LOW);
  digitalWrite(LED_RUNNING_PIN, y ? HIGH : LOW);
  digitalWrite(LED_ERROR_PIN, r ? HIGH : LOW);
}

void lampTest() {
  Serial.println("[HMI] Lamp test: green, yellow, red, all");
  setLeds(true, false, false);
  delay(500);
  setLeds(false, true, false);
  delay(500);
  setLeds(false, false, true);
  delay(500);
  setLeds(true, true, true);
  delay(1000);
  setLeds(false, false, false);
}

void printLevels() {
  Serial.print("[HMI] Raw levels: ON(GPIO13)=");
  Serial.print(digitalRead(BTN_ON_PIN) ? "HIGH" : "LOW");
  Serial.print(" OFF(GPIO14)=");
  Serial.print(digitalRead(BTN_OFF_PIN) ? "HIGH" : "LOW");
  Serial.println(BUTTON_ACTIVE_LOW ? "  (idle should be HIGH)" : "  (idle should be LOW)");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(BTN_ON_PIN, BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  pinMode(BTN_OFF_PIN, BUTTON_ACTIVE_LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  pinMode(LED_POWER_PIN, OUTPUT);
  pinMode(LED_RUNNING_PIN, OUTPUT);
  pinMode(LED_ERROR_PIN, OUTPUT);
  setLeds(false, false, false);

  Serial.println();
  Serial.println("[HMI] HMI test: buttons GPIO13 (ON) / GPIO14 (OFF); "
                 "LEDs GPIO25 green / 26 yellow / 33 red");
  Serial.println("[HMI] Serial: G/Y/R toggle LED, T lamp test, S raw levels");
  printLevels();
  lampTest();
  btnOn.lastRaw = btnOn.pressed = rawPressed(BTN_ON_PIN);
  btnOff.lastRaw = btnOff.pressed = rawPressed(BTN_OFF_PIN);
  if (btnOn.pressed || btnOff.pressed) {
    Serial.println("[HMI] WARNING: a button reads pressed at boot — check wiring "
                   "or BUTTON_ACTIVE_LOW");
  }
}

void loop() {
  const unsigned long now = millis();

  if (updateButton(btnOn, now) && !btnOn.pressed) {
    powerLatched = true;
    Serial.println("[HMI] -> latched ON (green)");
  }
  if (updateButton(btnOff, now) && !btnOff.pressed) {
    powerLatched = false;
    manualG = manualY = manualR = false;
    Serial.println("[HMI] -> OFF (all LEDs off)");
  }

  while (Serial.available()) {
    const char c = toupper(Serial.read());
    switch (c) {
      case 'G': manualG = !manualG; Serial.printf("[HMI] green %s\n", manualG ? "on" : "off"); break;
      case 'Y': manualY = !manualY; Serial.printf("[HMI] yellow %s\n", manualY ? "on" : "off"); break;
      case 'R': manualR = !manualR; Serial.printf("[HMI] red %s\n", manualR ? "on" : "off"); break;
      case 'T': lampTest(); break;
      case 'S': printLevels(); break;
      default: break;
    }
  }

  setLeds(powerLatched || manualG,
          btnOn.pressed || manualY,
          btnOff.pressed || manualR);
}
