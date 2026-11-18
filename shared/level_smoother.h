/************************************************************
 * Water-level smoothing and noise check (1.9.24)
 *
 * The Grove 10 cm stick reads in 5 % steps (5 mm per pad), and on 18 Oct its
 * single readings swung between 21 and 45 % while about 3 mm of water went in
 * (surface ripple, a touched or reseated stick). Level decisions therefore use
 * the median of the last MEDIAN_WINDOW_MS of readings (one every 2 s), and the
 * stick counts as noisy when its readings span NOISY_SPAN_PCT (two steps) or
 * more within NOISE_WINDOW_MS. A refill that would start, or continue, on a
 * noisy stick holds until the operator confirms it (stage_controller.h).
 ************************************************************/

#ifndef SMART_GARDEN_LEVEL_SMOOTHER_H
#define SMART_GARDEN_LEVEL_SMOOTHER_H

namespace LevelSmoother {

constexpr int CAPACITY = 48;                      // > 60 s of readings at one per 2 s
constexpr unsigned long MEDIAN_WINDOW_MS = 30000;
constexpr unsigned long NOISE_WINDOW_MS = 60000;
constexpr int NOISY_SPAN_PCT = 10;                // two 5 % steps

struct State {
  int pct[CAPACITY] = {};
  unsigned long ms[CAPACITY] = {};
  int count = 0;   // stored readings (at most CAPACITY)
  int next = 0;    // slot for the next reading
};

inline void add(State& s, int pct, unsigned long nowMs) {
  if (pct < 0 || pct > 100) {
    return;
  }
  s.pct[s.next] = pct;
  s.ms[s.next] = nowMs;
  s.next = (s.next + 1) % CAPACITY;
  if (s.count < CAPACITY) {
    s.count++;
  }
}

inline void reset(State& s) {
  s = State{};
}

// Readings no older than windowMs, copied into out (up to CAPACITY). Returns how many.
inline int recent(const State& s, unsigned long nowMs, unsigned long windowMs, int* out) {
  int n = 0;
  for (int i = 0; i < s.count; i++) {
    if (nowMs - s.ms[i] <= windowMs) {
      out[n++] = s.pct[i];
    }
  }
  return n;
}

// Median of the last windowMs of readings; -1 when there are none.
inline int median(const State& s, unsigned long nowMs, unsigned long windowMs = MEDIAN_WINDOW_MS) {
  int v[CAPACITY];
  const int n = recent(s, nowMs, windowMs, v);
  if (n == 0) {
    return -1;
  }
  for (int i = 1; i < n; i++) {
    const int key = v[i];
    int j = i - 1;
    while (j >= 0 && v[j] > key) {
      v[j + 1] = v[j];
      j--;
    }
    v[j + 1] = key;
  }
  return v[n / 2];  // upper median: a reading the stick actually gave
}

// Highest minus lowest reading over the last windowMs; 0 with fewer than two.
inline int spread(const State& s, unsigned long nowMs, unsigned long windowMs = NOISE_WINDOW_MS) {
  int v[CAPACITY];
  const int n = recent(s, nowMs, windowMs, v);
  if (n < 2) {
    return 0;
  }
  int lo = v[0];
  int hi = v[0];
  for (int i = 1; i < n; i++) {
    lo = v[i] < lo ? v[i] : lo;
    hi = v[i] > hi ? v[i] : hi;
  }
  return hi - lo;
}

inline bool noisy(const State& s, unsigned long nowMs) {
  return spread(s, nowMs) >= NOISY_SPAN_PCT;
}

}  // namespace LevelSmoother

#endif
