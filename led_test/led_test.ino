#include <FastLED.h>

// Bring-up of the grow-light panel on GPIO 27.
// Layout: 6 rows × 18 LEDs (108), serpentine.
// Data starts on the end row. After that row the wire wraps and continues
// forward onto the next row (odd rows run back the other way):
//   even rows (0, 2, 4): along the row, starting at the end position
//   odd rows  (1, 3, 5): back the other way, toward the next wrap
// Hue follows wire order 0..(LED_COUNT-1), so the rainbow walks that path.
#define LED_PIN        27
#define ROW_COUNT      6
#define LEDS_PER_ROW   18
constexpr int LED_COUNT = ROW_COUNT * LEDS_PER_ROW;

#define FRAME_MS          10
#define MAX_VOLTS         5
#define MAX_CURRENT_MA    2200  // soft cap — leave headroom on the 3 A LED buck

CRGB leds[LED_COUNT];

void setup() {
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, LED_COUNT);
  FastLED.setMaxPowerInVoltsAndMilliamps(MAX_VOLTS, MAX_CURRENT_MA);
  FastLED.setBrightness(255);
  FastLED.clear(true);
}

void loop() {
  // Original full-strip rainbow: one hue cycle across the wire, scrolling.
  // Every pixel stays lit. A dead LED or a bad join shows up as a gap in
  // the progression where the strip wraps to the next row.
  static uint16_t firstHue = 0;
  for (int i = 0; i < LED_COUNT; i++) {
    const uint8_t hue = (firstHue + (i * 65536L / LED_COUNT)) >> 8;
    leds[i] = CHSV(hue, 255, 255);
  }
  FastLED.show();
  firstHue += 256;
  delay(FRAME_MS);
}
