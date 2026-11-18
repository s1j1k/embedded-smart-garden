/* Dashboard status: per-device health and control schedule for MQTT telemetry.
 *
 * "health":     {device: "ok" | "fault" | "init" | "warming" | "hold" | "off"}
 * "health_msg": {device: "why"}               (only for devices that are not ok)
 * "sched":      next sensor read, next level/EC/pH check, dose thresholds, and
 *               the reason a loop will not dose ("hold") when one applies.
 */
#include "garden.h"
#include "shared/sensor_ema.h"

namespace {

const char* const OK = "ok";
const char* const FAULT = "fault";
const char* const INIT = "init";
const char* const WARMING = "warming";
const char* const HOLD = "hold";
const char* const OFF = "off";

void put(JsonObject health, JsonObject msgs, const char* key, const char* state,
         const char* msg = nullptr) {
  health[key] = state;
  if (msg != nullptr && msg[0] != '\0') {
    msgs[key] = String(msg);
  }
}

// Seconds until an interval-driven check is due (0 = due now, -1 = never).
long dueInS(unsigned long lastMs, unsigned long intervalMs, unsigned long now) {
  if (intervalMs == 0) {
    return -1;
  }
  if (lastMs == 0) {
    return 0;
  }
  const unsigned long elapsed = now - lastMs;
  return elapsed >= intervalMs ? 0 : static_cast<long>((intervalMs - elapsed) / 1000UL);
}

const char* activityName(StageController::Activity a) {
  switch (a) {
    case StageController::WAIT_WATER_SETTLE: return "wait_water_settle";
    case StageController::WAIT_EC_SETTLE: return "wait_ec_settle";
    case StageController::WAIT_PH_SETTLE: return "wait_ph_settle";
    case StageController::AWAIT_RECHECK: return "await_recheck";
    case StageController::WAIT_EC_PH_MIX: return "wait_ph_after_nutrient";
    default: return "idle";
  }
}

// Reason every auto loop is held, or nullptr when auto control may run.
const char* autoHoldReason() {
  if (autoControlPaused) return "auto control paused (AUTO RESUME)";
  if (isWaterFillMode()) return "FILL mode — auto control off";
  if (pumpCalibrationActive()) return "pump CAL session running";
  if (softPrimeActive()) return "soft-prime running";
  if (isControlIdleMode()) return "stage has no auto control";
  return nullptr;
}

const char* pumpHold(PumpCal::Id id) {
  if (PumpCal::isEmpty(id)) return "bottle empty (FILL after refilling)";
  if (!PumpCal::isCalibrated(id)) return "pump not calibrated (CAL)";
  return nullptr;
}

// Influx upload: fault once nothing has been written for a few sensor cycles.
constexpr long INFLUX_STALE_S = 90;

void addInfluxHealth(JsonObject h, JsonObject m) {
  #if ENABLE_INFLUXDB
  char buf[112];
  const unsigned long now = millis();
  const InfluxCloud::Stats& st = InfluxCloud::stats();
  const long ago = InfluxCloud::secondsSinceOk(now);
  const int queued = InfluxCloud::queuedLines();
  if (!InfluxCloud::configured()) {
    put(h, m, "influx", OFF, "not configured in secrets.h");
  } else if (WiFi.status() != WL_CONNECTED) {
    snprintf(buf, sizeof(buf), "WiFi down — %d line(s) queued", queued);
    put(h, m, "influx", FAULT, buf);
  } else if (InfluxCloud::controlHoldFlag() && st.holdSinceMs != 0 &&
             now - st.holdSinceMs < 600000UL) {
    put(h, m, "influx", HOLD, "paused while a pump runs");
  } else if (ago < 0 && now < 120000UL) {
    put(h, m, "influx", WARMING, "no upload yet since boot");
  } else if (ago < 0 || ago > INFLUX_STALE_S) {
    snprintf(buf, sizeof(buf), "last upload %s ago, HTTP %d, queue %d/%d, dropped %lu",
             ago < 0 ? "never" : (String(ago) + " s").c_str(), st.lastCode, queued,
             InfluxCloud::kBacklogSlots, st.droppedLines);
    put(h, m, "influx", FAULT, buf);
  } else {
    snprintf(buf, sizeof(buf), "last upload %ld s ago, %lu dropped since boot", ago,
             st.droppedLines);
    put(h, m, "influx", OK, buf);
  }
  #else
  put(h, m, "influx", OFF, "disabled in firmware");
  #endif
}

void addSensorHealth(JsonObject h, JsonObject m, unsigned long now) {
  char buf[112];

  #if ENABLE_PH_TDS
  if (phSensorFault) {
    snprintf(buf, sizeof(buf),
             "no valid pH (raw %.2f @ %.3f V) — probe unpowered? check 5 V rail",
             phValue, phVoltage);
    put(h, m, "ph", FAULT, buf);
  } else if (phWarmupElapsedS() < SensorEma::WARMUP_MS / 1000UL) {
    snprintf(buf, sizeof(buf), "EMA warm-up %lu/%lu s — no pH dosing yet",
             phWarmupElapsedS(), SensorEma::WARMUP_MS / 1000UL);
    put(h, m, "ph", WARMING, buf);
  } else {
    put(h, m, "ph", OK);
  }

  if (tdsSensorFault) {
    snprintf(buf, sizeof(buf), "TDS reads %d ppm — probe out of the water or unplugged",
             static_cast<int>(tdsPPM));
    put(h, m, "tds", FAULT, buf);
  } else if (tdsWarmupElapsedS() < SensorEma::WARMUP_MS / 1000UL) {
    snprintf(buf, sizeof(buf), "EMA warm-up %lu/%lu s — no EC dosing yet",
             tdsWarmupElapsedS(), SensorEma::WARMUP_MS / 1000UL);
    put(h, m, "tds", WARMING, buf);
  } else {
    put(h, m, "tds", OK);
  }
  put(h, m, "light", OK);
  #else
  put(h, m, "ph", OFF, "disabled in firmware");
  put(h, m, "tds", OFF, "disabled in firmware");
  put(h, m, "light", OFF, "disabled in firmware");
  #endif

  #if ENABLE_WATER_LEVEL
  if (isWaterLevelSensorFault()) {
    put(h, m, "water_level", FAULT,
        "not responding or stuck at 0% — lift out, dry, power-cycle, reinsert");
  } else if (isWaterLevelInitRequired()) {
    if (waterLevelTrust.lastTrustedPercent > 0) {
      snprintf(buf, sizeof(buf),
               "init required: lift the stick out and lower it slowly into the "
               "water (last saved %d%%)",
               waterLevelTrust.lastTrustedPercent);
    } else {
      snprintf(buf, sizeof(buf),
               "init required: lift the stick out and lower it slowly into the water");
    }
    put(h, m, "water_level", INIT, buf);
  } else {
    put(h, m, "water_level", OK);
  }
  #else
  put(h, m, "water_level", OFF, "disabled in firmware");
  #endif

  #if ENABLE_DS18B20
  if (waterTempC <= -100.0f) {
    put(h, m, "water_temp", FAULT, "DS18B20 no valid reading (pH/TDS use 25 C)");
  } else {
    put(h, m, "water_temp", OK);
  }
  #else
  put(h, m, "water_temp", OFF, "disabled in firmware");
  #endif

  #if ENABLE_AMBIENT_SENSORS
  if (ambientTempC <= -100.0f) {
    put(h, m, "ambient", FAULT, "DHT11 no valid reading");
  } else {
    put(h, m, "ambient", OK);
  }
  #else
  put(h, m, "ambient", OFF, "disabled in firmware");
  #endif

  #if ENABLE_CO2
  if (!co2SensorReady()) {
    snprintf(buf, sizeof(buf),
             "no I2C answer at 0x28 (%s) — check harness, 3.3 V, GND; retried every 30 s",
             i2cLineState());
    put(h, m, "co2", FAULT, buf);
  } else if (co2PPM < 0) {
    put(h, m, "co2", FAULT, "PAS CO2 invalid reading — 5 V rail off? (emitter needs 5 V)");
  } else {
    put(h, m, "co2", OK);
  }
  #else
  put(h, m, "co2", OFF, "disabled in firmware");
  #endif
  (void)now;
}

void addActuatorHealth(JsonObject h, JsonObject m) {
  #if ENABLE_PUMPS
  const bool ecUsed = GrowStage::allowsEc(systemMode);
  struct {
    const char* key;
    PumpCal::Id id;
    bool used;
  } pumps[] = {
    {"water_pump", PumpCal::WATER, true},
    {"nutrient_pump", PumpCal::NUTRIENT, ecUsed},
    {"ph_up_pump", PumpCal::PH_UP, true},
    {"ph_down_pump", PumpCal::PH_DOWN, true},
  };
  const PhControl::State& phs = StageController::ctx().ph;
  for (const auto& p : pumps) {
    const bool phPump = p.id == PumpCal::PH_UP || p.id == PumpCal::PH_DOWN;
    const bool up = p.id == PumpCal::PH_UP;
    if (PumpCal::isEmpty(p.id)) {
      put(h, m, p.key, FAULT, "bottle empty — dosing stopped (FILL after refilling)");
    } else if (p.id == PumpCal::NUTRIENT && StageController::ctx().ec.noResponseFault) {
      put(h, m, p.key, FAULT,
          "no response: nutrient doses did not raise TDS — pump not running or line not "
          "delivering? PRIME NUTRIENT, then SESSION RESET EC");
    } else if (phPump && (up ? phs.noResponseFaultUp : phs.noResponseFaultDown)) {
      char buf[160];
      const int n = up ? phs.noResponseUp : phs.noResponseDown;
      if (n > 0) {
        snprintf(buf, sizeof(buf),
                 "no response: %d doses (%.1f mL) did not move pH — pump not running or line "
                 "not delivering? (or a heavily buffered tank). PRIME, then SESSION RESET PH",
                 n, up ? phs.noResponseMlUp : phs.noResponseMlDown);
      } else {
        snprintf(buf, sizeof(buf),
                 "no response (saved before reboot) — doses did not move pH. PRIME to check "
                 "the line, then SESSION RESET PH");
      }
      put(h, m, p.key, FAULT, buf);
    } else if (p.used && !PumpCal::isCalibrated(p.id)) {
      put(h, m, p.key, HOLD, "not calibrated — run CAL");
    } else if (p.id == PumpCal::WATER && isWaterLevelInitRequired()) {
      put(h, m, p.key, HOLD, "auto top-up held until water level init");
    } else if (p.id == PumpCal::WATER && isWaterLevelSensorFault()) {
      put(h, m, p.key, HOLD, "auto top-up held — water level sensor fault");
    } else {
      put(h, m, p.key, OK);
    }
  }
  #else
  put(h, m, "water_pump", OFF, "pumps disabled in firmware");
  put(h, m, "nutrient_pump", OFF, "pumps disabled in firmware");
  put(h, m, "ph_up_pump", OFF, "pumps disabled in firmware");
  put(h, m, "ph_down_pump", OFF, "pumps disabled in firmware");
  #endif

  #if ENABLE_GROW_LIGHT
  {
    // Say why the light is on or off and when it next switches, so an off
    // light at night reads as the schedule rather than a fault.
    const GrowLight::Status gl = GrowLight::status();
    const long mins = (gl.nextChangeS + 59) / 60;
    char buf[144];
    if (gl.ntpSchedule) {
      snprintf(buf, sizeof(buf), "%s: ON %02d:00-%02d:00 UTC; %s at %02d:00 UTC, in %ldh%02ldm",
               gl.wantOn ? "day" : "night", gl.scheduleStartHourUtc, gl.scheduleOffHourUtc,
               gl.wantOn ? "off" : "on",
               gl.wantOn ? gl.scheduleOffHourUtc : gl.scheduleStartHourUtc,
               mins / 60, mins % 60);
    } else {
      snprintf(buf, sizeof(buf), "no NTP: %d h ON / %d h OFF from boot; %s in %ldh%02ldm",
               gl.photoperiodOnHours, 24 - gl.photoperiodOnHours,
               gl.wantOn ? "off" : "on", mins / 60, mins % 60);
    }
    if (gl.makeupOwedS > 0) {
      const size_t used = strlen(buf);
      snprintf(buf + used, sizeof(buf) - used, "; make-up %s, %ldh%02ldm owed",
               gl.makeup ? "lit" : (gl.makeupArmed ? "on" : "off"), gl.makeupOwedS / 3600,
               (gl.makeupOwedS % 3600) / 60);
    }
    put(h, m, "grow_light", OK, buf);
  }
  #else
  put(h, m, "grow_light", OFF, "disabled in firmware");
  #endif

  const char* hold = autoHoldReason();
  put(h, m, "auto_control", hold ? HOLD : OK, hold);

  addInfluxHealth(h, m);
}

void addSchedule(JsonObject s, unsigned long now) {
  const StageController::Context& c = StageController::ctx();
  const GrowStage::Config& cfg = GrowStage::config(c.stage);
  const char* globalHold = autoHoldReason();

  const unsigned long readMs =
      (PhProbeCal::isActive() || TdsProbeCal::isActive())
          ? SENSOR_READ_INTERVAL_PH_CAL_MS
          : SENSOR_READ_INTERVAL_MS;
  s["read_s"] = readMs / 1000UL;
  s["read_in_s"] = dueInS(lastSensorReadTime, readMs, now);
  s["ema_warmup_s"] = SensorEma::WARMUP_MS / 1000UL;
  s["activity"] = activityName(c.activity);
  s["wait_s"] = (c.activity != StageController::IDLE &&
                 c.activity != StageController::AWAIT_RECHECK &&
                 static_cast<long>(c.waitUntilMs - now) > 0)
                    ? static_cast<long>((c.waitUntilMs - now) / 1000UL)
                    : 0L;
  // Control cycle order is level → EC → pH. cycle_requested: a manual
  // CYCLE is queued; limit_cycle: a reading outside its dead limits started
  // one; retry_in_s: such a cycle could not dose and the limits start the next
  // one after this; ph_after_ec: nutrient dose mixed, pH check runs next.
  s["cycle_requested"] = c.manualCyclePending;
  s["limit_cycle"] = c.limitCyclePending;
  {
    const unsigned long since = now - c.limitRetryFromMs;
    s["retry_in_s"] = (c.limitRetryHold && since < ControlDesign::LIMIT_CYCLE_RETRY_MS)
                          ? static_cast<long>((ControlDesign::LIMIT_CYCLE_RETRY_MS - since) / 1000UL)
                          : 0L;
  }
  s["ph_after_ec"] = c.runPhAfterEcSettle;
  s["water_settle_s"] = StageController::WATER_SETTLE_MS / 1000UL;
  s["ec_settle_s"] = cfg.ecSettleMs / 1000UL;
  s["ph_settle_s"] = cfg.phSettleMs / 1000UL;

  // Level: top-up when drawdown from fill_0 exceeds target + tolerance.
  {
    JsonObject o = s["level"].to<JsonObject>();
    const bool on = GrowStage::allowsLevelAuto(c.stage);
    o["on"] = on;
    o["every_s"] = cfg.levelCheckIntervalMs / 1000UL;
    o["in_s"] = dueInS(c.lastLevelCheckMs, cfg.levelCheckIntervalMs, now);
    if (c.fill0Percent >= 0) {
      const float upperMm =
          static_cast<float>(cfg.targetDrawdownMm + cfg.drawdownToleranceMm);
      int topUpAt = -1;
      for (int pct = c.fill0Percent; pct >= 0; pct--) {
        if (LevelControl::drawdownMm(c.fill0Percent, pct) > upperMm) {
          topUpAt = pct;
          break;
        }
      }
      o["topup_at_pct"] = topUpAt;
      o["refill_to_pct"] = LevelControl::levelPercentForDrawdownMm(
          c.fill0Percent, static_cast<float>(cfg.targetDrawdownMm));
    }
    const char* hold = globalHold;
    if (!hold && !on) hold = "off in this stage";
    if (!hold && c.fill0Percent < 0) hold = "fill_0 not set (FILL, then CONFIRM)";
    if (!hold && isWaterLevelInitRequired()) hold = "water level init required";
    if (!hold && isWaterLevelSensorFault()) hold = "water level sensor fault";
    if (!hold) hold = pumpHold(PumpCal::WATER);
    if (hold) o["hold"] = hold;
  }

  // EC: dose nutrient when EC < dead-low; above dead-high is flagged only.
  {
    JsonObject o = s["ec"].to<JsonObject>();
    const bool on = GrowStage::allowsEc(c.stage) && cfg.ecControlEnabled;
    o["on"] = on;
    o["every_s"] = cfg.ecCheckIntervalMs / 1000UL;
    o["in_s"] = dueInS(c.lastEcCheckMs, cfg.ecCheckIntervalMs, now);
    if (on) {
      o["dose_below"] = cfg.ecDeadLow;
      o["min"] = cfg.ecTargetMin;
      o["max"] = cfg.ecTargetMax;
      o["high_above"] = cfg.ecDeadHigh;
    }
    const char* hold = globalHold;
    if (!hold && !on) hold = "off in this stage";
    if (!hold && tdsSensorFault) hold = "TDS sensor fault (probe out of the water?)";
    if (!hold && isnan(tdsPpmEma)) hold = "TDS EMA warming up";
    if (!hold && c.ec.sessionCapAlert) hold = "EC session dose cap reached";
    if (!hold && c.ec.noResponseFault)
      hold = "nutrient not responding (pump/line?) — SESSION RESET EC after checking";
    if (!hold) hold = pumpHold(PumpCal::NUTRIENT);
    if (hold) o["hold"] = hold;
  }

  // pH: dose up below dead-low, down above dead-high.
  {
    JsonObject o = s["ph"].to<JsonObject>();
    const bool on = GrowStage::allowsPh(c.stage);
    o["on"] = on;
    o["every_s"] = cfg.phCheckIntervalMs / 1000UL;
    o["in_s"] = dueInS(c.lastPhCheckMs, cfg.phCheckIntervalMs, now);
    if (on) {
      o["up_below"] = cfg.phDeadLow;
      o["down_above"] = cfg.phDeadHigh;
      o["min"] = cfg.phTargetMin;
      o["max"] = cfg.phTargetMax;
    }
    const char* hold = globalHold;
    if (!hold && !on) hold = "off in this stage";
    if (!hold && phSensorFault) hold = "pH sensor fault";
    if (!hold && GrowStage::allowsEc(c.stage) && isnan(tdsPpmEma))
      hold = "TDS warming up: pH waits for nutrient";
    if (!hold && StageController::phWaitsForNutrient(tdsPpmEma))
      hold = "nutrient loop running: pH waits until TDS is in range";
    if (!hold && isnan(phValueEma)) hold = "pH EMA warming up";
    if (!hold && c.ph.sessionCapAlert) hold = "pH session dose cap reached";
    if (!hold && c.ph.noResponseFaultDown && phValueEma > cfg.phDeadHigh)
      hold = "pH-down not responding (pump/line?) — SESSION RESET PH after checking";
    if (!hold && c.ph.noResponseFaultUp && phValueEma < cfg.phDeadLow)
      hold = "pH-up not responding (pump/line?) — SESSION RESET PH after checking";
    if (!hold && PumpCal::isEmpty(PumpCal::PH_UP) && PumpCal::isEmpty(PumpCal::PH_DOWN)) {
      hold = "pH-up and pH-down bottles empty";
    }
    if (hold) o["hold"] = hold;
  }
}

}  // namespace

void addHealthJson(JsonDocument& doc) {
  const unsigned long now = millis();
  JsonObject h = doc["health"].to<JsonObject>();
  JsonObject m = doc["health_msg"].to<JsonObject>();
  addSensorHealth(h, m, now);
  addActuatorHealth(h, m);
  addSchedule(doc["sched"].to<JsonObject>(), now);
}
