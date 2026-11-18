/************************************************************
 * Analog ADC helpers — mean / median for noisy ESP32 pins
 ************************************************************/

#ifndef SMART_GARDEN_ANALOG_FILTER_H
#define SMART_GARDEN_ANALOG_FILTER_H

#include <Arduino.h>
#include <math.h>

namespace AnalogFilter {

constexpr int DEFAULT_SAMPLES = 16;
constexpr int CAL_SAMPLES = 48;  // denser average for probe calibration capture

inline void sortAscending(float* values, int n) {
  for (int i = 1; i < n; i++) {
    float key = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
}

inline float medianOf(float* values, int n) {
  if (n <= 0) {
    return NAN;
  }
  sortAscending(values, n);
  if (n % 2 == 1) {
    return values[n / 2];
  }
  return 0.5f * (values[n / 2 - 1] + values[n / 2]);
}

inline float meanOf(const float* values, int n) {
  if (n <= 0) {
    return NAN;
  }
  float sum = 0.0f;
  for (int i = 0; i < n; i++) {
    sum += values[i];
  }
  return sum / static_cast<float>(n);
}

// Drop samples farther than maxDev from the median, then mean of keepers.
inline float trimmedMean(float* values, int n, float maxDev) {
  if (n <= 0) {
    return NAN;
  }
  const float med = medianOf(values, n);
  float sum = 0.0f;
  int kept = 0;
  for (int i = 0; i < n; i++) {
    if (fabsf(values[i] - med) <= maxDev) {
      sum += values[i];
      kept++;
    }
  }
  if (kept == 0) {
    return med;
  }
  return sum / static_cast<float>(kept);
}

// Average of analogRead() over sampleCount readings with inter-sample delay.
inline int readAdcAverage(int pin, int sampleCount = DEFAULT_SAMPLES,
                          unsigned long delayMs = 5) {
  if (sampleCount < 1) {
    sampleCount = 1;
  }
  long total = 0;
  for (int i = 0; i < sampleCount; i++) {
    total += analogRead(pin);
    if (delayMs > 0) {
      delay(delayMs);
    }
  }
  return static_cast<int>(total / sampleCount);
}

// Average of analogReadMilliVolts() → volts at the GPIO pin.
inline float readGpioVoltsAverage(int pin, int sampleCount = DEFAULT_SAMPLES,
                                  unsigned long delayMs = 5) {
  if (sampleCount < 1) {
    sampleCount = 1;
  }
  float samples[64];
  const int n = sampleCount > 64 ? 64 : sampleCount;
  for (int i = 0; i < n; i++) {
    samples[i] = analogReadMilliVolts(pin) / 1000.0f;
    if (delayMs > 0) {
      delay(delayMs);
    }
  }
  // Trim outliers ±30 mV around the median before averaging.
  return trimmedMean(samples, n, 0.030f);
}

}  // namespace AnalogFilter

#endif  // SMART_GARDEN_ANALOG_FILTER_H
