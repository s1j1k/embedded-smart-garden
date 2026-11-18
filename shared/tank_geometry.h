/************************************************************
 * Reservoir inner dimensions and Grove-level → volume helpers
 *
 * Approximate inner box (mm):
 *   width  280
 *   length 400
 *   height 120
 *
 * Nominal initial fill: water surface 55 mm below the rim
 * (just touching the bottom of the net pots) → 65 mm water
 * column → geometric volume 7.28 L.
 *
 * Dose sizing uses a 90 % derate of that fill (conservative
 * working volume ≈ 6.55 L) so pots / walls do not cause
 * overshoot. See grow_stage.h for label-based mL clamps.
 ************************************************************/

#ifndef SMART_GARDEN_TANK_GEOMETRY_H
#define SMART_GARDEN_TANK_GEOMETRY_H

namespace Tank {

constexpr float WIDTH_MM = 280.0f;
constexpr float LENGTH_MM = 400.0f;
constexpr float HEIGHT_MM = 120.0f;

// Freeboard from rim to net-pot touch / initial fill line.
constexpr float FREEBOARD_FROM_TOP_MM = 55.0f;
constexpr float INITIAL_FILL_HEIGHT_MM = HEIGHT_MM - FREEBOARD_FROM_TOP_MM;  // 65 mm

// Grove stick (100 mm) hangs from the underside of the 3 mm lid, so its tip
// is 20 mm above the floor: 0 % = 20 mm of water, 100 % = 120 mm. Water below
// 20 mm cannot be seen. (Measured 2026-10-06.)
// Control uses the stick only relative to the net-pot touch reading (fill_0):
// drawdown_mm = (fill_0 % - level %) x 1 mm/%, so this offset cancels; it only
// sets the default touch reading below and absolute volume estimates.
constexpr float SENSOR_SPAN_MM = 100.0f;
constexpr float SENSOR_OFFSET_MM = HEIGHT_MM - SENSOR_SPAN_MM;  // 20 mm

// Empirically: 5 mm of water height ≈ 5 % on this Grove stick.
constexpr float GROVE_MM_PER_PERCENT = 1.0f;

// DWC air gaps relative to net-pot touch (water surface below pots). The tote
// holds only 65 mm of water at touch (net pots reach 55 mm into the 120 mm box),
// so gaps stay small: 20 mm leaves ~5.0 L, 30 mm ~3.9 L. Doses assume 6.5 L
// (ControlDesign::V_ASSUMED_L) and stay safe while the real volume is above
// about half of that.
constexpr int VEG_AIR_GAP_MM = 20;
constexpr int FLOWER_AIR_GAP_MM = 30;
// Absolute lowest surface before a safety top-up: 25 mm of water, just above
// the stick tip (5 %).
constexpr int SAFETY_FLOOR_AIR_GAP_MM = 35;
// Extra mm below the floor line so refill-to == floor never chatters.
constexpr int SAFETY_HYSTERESIS_MM = 5;

// Fallback until water_fill_cal saves /net_pot_touch.txt.
// 65 mm of water = 45 mm up the stick = 45 %.
constexpr int NET_POT_TOUCH_DEFAULT_PERCENT = 45;
// Never treat the safety floor as below this Grove reading.
constexpr int WATER_LEVEL_MIN_PERCENT = 5;

// Derate geometric fill so chemistry doses assume less buffer (avoid overshoot).
constexpr float CONSERVATIVE_VOLUME_DERATE = 0.90f;

inline float mlPerMm() {
  return (WIDTH_MM * LENGTH_MM) / 1000.0f;  // 112 mL/mm
}

inline float fullMl() {
  return mlPerMm() * HEIGHT_MM;
}

inline float initialFillMl() {
  return mlPerMm() * INITIAL_FILL_HEIGHT_MM;  // 7280 mL
}

inline float initialFillL() {
  return initialFillMl() / 1000.0f;  // 7.28 L
}

inline float conservativeWorkingMl() {
  return initialFillMl() * CONSERVATIVE_VOLUME_DERATE;  // ≈ 6552 mL
}

inline float conservativeWorkingL() {
  return conservativeWorkingMl() / 1000.0f;  // ≈ 6.55 L
}

inline int mmToPercentOffset(int mm) {
  if (mm <= 0) {
    return 0;
  }
  return static_cast<int>(mm / GROVE_MM_PER_PERCENT + 0.5f);
}

inline int clampPercent(int pct) {
  if (pct < WATER_LEVEL_MIN_PERCENT) {
    return WATER_LEVEL_MIN_PERCENT;
  }
  if (pct > 100) {
    return 100;
  }
  return pct;
}

// Grove % for a surface that is `airGapMm` below the net-pot touch line.
inline int percentForAirGapMm(int netPotTouchPercent, int airGapMm) {
  return clampPercent(netPotTouchPercent - mmToPercentOffset(airGapMm));
}

inline float heightMmFromGrovePct(float pct) {
  if (pct < 0.0f) {
    return 0.0f;
  }
  if (pct > 100.0f) {
    pct = 100.0f;
  }
  return SENSOR_OFFSET_MM + SENSOR_SPAN_MM * (pct / 100.0f);
}

inline float geomMlFromGrovePct(float pct) {
  float h = heightMmFromGrovePct(pct);
  if (h < 0.0f) {
    h = 0.0f;
  }
  if (h > HEIGHT_MM) {
    h = HEIGHT_MM;
  }
  return mlPerMm() * h;
}

inline float geomMlFromHeightMm(float heightMm) {
  if (heightMm < 0.0f) {
    heightMm = 0.0f;
  }
  if (heightMm > HEIGHT_MM) {
    heightMm = HEIGHT_MM;
  }
  return mlPerMm() * heightMm;
}

}  // namespace Tank

#endif  // SMART_GARDEN_TANK_GEOMETRY_H
