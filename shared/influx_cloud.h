/************************************************************
 * InfluxDB Cloud v2 writer (Line Protocol over HTTPS)
 *
 * Configure in secrets.h:
 *   INFLUX_URL, INFLUX_ORG, INFLUX_BUCKET, INFLUX_TOKEN
 *
 * Data API → Load Data → API Token (read/write on bucket).
 * URL is the cluster host, e.g. https://us-east-1-1.aws.cloud2.influxdata.com
 *
 * Writes never run on the control loop. Callers enqueue a line and return.
 * A low-priority task posts the queue only while actuators are idle.
 * Starting a dose, prime, calibration, or a manual cycle calls
 * preemptForControl(), which closes an in-flight TLS socket so the
 * handshake cannot sit on the radio while a pump is commanded on.
 * The line stays queued and is sent after control releases the hold.
 *
 * Overflow: when the 8-line RAM queue is full a new line is appended to
 * /influx_outbox.lp on LittleFS instead of being dropped, and the uploader
 * drains that file whenever the RAM queue is empty. Every line carries its
 * own millisecond timestamp, so a line sent twice overwrites the same point
 * (Influx keys points on measurement + tags + time): no duplicates.
 ************************************************************/

#ifndef SMART_GARDEN_INFLUX_CLOUD_H
#define SMART_GARDEN_INFLUX_CLOUD_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <new>
#include <HTTPClient.h>
#include <math.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include "control_event_log.h"
#include "durable_queue.h"

// INFLUX_* come from secrets.h (inline const char* const). Include secrets.h
// before this header (garden.h does). Do not #define INFLUX_URL here.

namespace InfluxCloud {

struct SensorPoint {
  const char* stage = "";
  const char* recordedAtUtc = "";
  unsigned long uptimeMs = 0;
  float waterTempC = NAN;
  float ambientTempC = NAN;
  float ambientHumidityPct = NAN;
  int lightAdc = -1;
  int waterLevelPct = -1;
  int waterLevelRawPct = -1;
  int waterTargetPct = -1;
  int fill0Pct = -1;
  float currentDrawdownMm = NAN;
  int targetDrawdownMm = -1;
  int tdsPpm = -1;
  float tdsPpmEma = NAN;
  float tdsPpmNutrient = NAN;
  float tdsPhSaltPpm = NAN;
  float tdsKappaUp = NAN;
  float tdsKappaDown = NAN;
  float tdsPhUpMl = NAN;
  float tdsPhDownMl = NAN;
  int tdsSaltCorrectionLive = -1;
  int tdsKappaProvisional = -1;
  float phRaw = NAN;
  float phFiltered = NAN;
  float phEma = NAN;
  float ph = NAN;
  float phVoltage = NAN;
  float phTargetMin = NAN;
  float phTargetMax = NAN;
  const char* phDoseState = "";
  float phSessionMl = NAN;
  float ecSessionMl = NAN;
  float gainPhUp = NAN;
  float gainPhDown = NAN;
  float gainEc = NAN;
  bool readyToSeed = false;
  bool baselineCapture = false;
  bool waterLevelSensorFault = false;
  int co2Ppm = -1;
  int waterPumpOn = 0;
  int nutrientPumpOn = 0;
  int phUpPumpOn = 0;
  int phDownPumpOn = 0;
  int growLightOn = -1;
  int growLightBrightnessPct = -1;
  int growLightAnimating = -1;
  const char* growLightMode = "";
  int growLightPhotoperiodH = -1;
  // Power / brownout health (see shared/power_monitor.h).
  const char* resetReason = "";
  int bootCount = -1;
  int brownoutCount = -1;
  int lastResetBrownout = -1;  // 1 if this boot was BOD, else 0
  int railMv = -1;             // optional ADC divider; -1 = not wired
  int freeHeapB = -1;
  int wifiRssiDbm = 0;
  bool wifiRssiValid = false;
  uint32_t bootId = 0;  // with seq: same reading identity as the CSV journal and MQTT
  uint32_t seq = 0;
};

inline bool configured() {
  if (INFLUX_URL == nullptr || INFLUX_ORG == nullptr ||
      INFLUX_BUCKET == nullptr || INFLUX_TOKEN == nullptr) {
    return false;
  }
  return INFLUX_URL[0] != '\0' && INFLUX_ORG[0] != '\0' &&
         INFLUX_BUCKET[0] != '\0' && INFLUX_TOKEN[0] != '\0' &&
         strcmp(INFLUX_TOKEN, "YOUR_INFLUX_API_TOKEN") != 0 &&
         strcmp(INFLUX_ORG, "YOUR_ORG_NAME_OR_ID") != 0;
}

inline void appendUrlEncoded(String& out, const char* value) {
  if (value == nullptr) {
    return;
  }
  for (const char* p = value; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else if (c == ' ') {
      out += "%20";
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
}

inline void appendUint64(String& out, unsigned long long value) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%llu", value);
  out += buf;
}

inline void appendEscaped(String& out, const char* value) {
  if (value == nullptr) {
    return;
  }
  for (const char* p = value; *p; ++p) {
    const char c = *p;
    if (c == ',' || c == ' ' || c == '=' || c == '"') {
      out += '\\';
    }
    out += c;
  }
}

inline void appendTag(String& line, const char* key, const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return;
  }
  line += ',';
  line += key;
  line += '=';
  appendEscaped(line, value);
}

inline void appendFieldInt(String& line, bool& first, const char* key, int value) {
  if (value < 0) {
    return;
  }
  if (!first) {
    line += ',';
  }
  first = false;
  line += key;
  line += '=';
  line += value;
  line += 'i';
}

// Like appendFieldInt but allows negatives (WiFi RSSI, etc.).
inline void appendFieldSignedInt(String& line, bool& first, const char* key,
                                 int value, bool valid = true) {
  if (!valid) {
    return;
  }
  if (!first) {
    line += ',';
  }
  first = false;
  line += key;
  line += '=';
  line += value;
  line += 'i';
}

inline void appendFieldFloat(String& line, bool& first, const char* key, float value,
                             int digits = 3) {
  if (isnan(value)) {
    return;
  }
  if (!first) {
    line += ',';
  }
  first = false;
  line += key;
  line += '=';
  line += String(value, digits);
}

inline void appendFieldBool(String& line, bool& first, const char* key, bool value) {
  if (!first) {
    line += ',';
  }
  first = false;
  line += key;
  line += '=';
  line += value ? "true" : "false";
}

inline void appendFieldStr(String& line, bool& first, const char* key, const char* value) {
  if (value == nullptr || value[0] == '\0') {
    return;
  }
  if (!first) {
    line += ',';
  }
  first = false;
  line += key;
  line += "=\"";
  for (const char* p = value; *p; ++p) {
    if (*p == '"' || *p == '\\') {
      line += '\\';
    }
    line += *p;
  }
  line += '"';
}

inline unsigned long long epochMs(
    bool ntpValid,
    uint32_t ntpEpochAtSync,
    unsigned long millisAtSync,
    unsigned long uptimeMs) {
  if (ntpValid) {
    // Unsigned subtraction then signed: correct across the 49.7-day millis()
    // wrap as long as the latch is less than ~24 days old (it is re-latched
    // every few hours).
    const long long delta = static_cast<long long>(
        static_cast<int32_t>(static_cast<uint32_t>(uptimeMs) -
                             static_cast<uint32_t>(millisAtSync)));
    return static_cast<unsigned long long>(ntpEpochAtSync) * 1000ULL +
           static_cast<unsigned long long>(delta);
  }
  return static_cast<unsigned long long>(uptimeMs);
}

// Wall-clock cap for one background attempt. The control loop does not wait
// on this. A shorter handshake than 15 s keeps a dead TLS session from
// occupying the radio until the next control action aborts it.
constexpr unsigned long kWriteBudgetMs = 8000;
// One attempt per session: a failed attempt retires the TLS session and the
// uploader retries after its backoff with a new one.
constexpr int kWriteAttempts = 1;
constexpr unsigned long kRetryGapMs = 250;
// Unsent line protocol. Head `inflight` lines are in the current POST and
// must not be dropped to make room for a newer sample.
constexpr int kBacklogSlots = 8;
constexpr int kBatchMax = 4;
constexpr size_t kBacklogLineMax = 1536;

struct PostOutcome {
  int code = -1;
  String error;
  String response;
};

inline bool writeErrorIsPermanent(int code) {
  return code == 400 || code == 401 || code == 403 || code == 404;
}

// One attempt. The client is owned by the uploader task so preemptForControl()
// can close it from the control loop without waiting for the handshake.
inline PostOutcome postOnce(WiFiClientSecure& client, HTTPClient& http, const String& body,
                            uint16_t timeoutMs) {
  PostOutcome out;
  String url = String(INFLUX_URL);
  url.trim();
  if (url.endsWith("/")) {
    url.remove(url.length() - 1);
  }
  url += "/api/v2/write?org=";
  appendUrlEncoded(url, INFLUX_ORG);
  url += "&bucket=";
  appendUrlEncoded(url, INFLUX_BUCKET);
  url += "&precision=ms";

  client.setInsecure();
  const int32_t connectMs = timeoutMs < 1000U ? 1000 : static_cast<int32_t>(timeoutMs);
  // setHandshakeTimeout() takes SECONDS (the core multiplies by 1000). Passing
  // ms here gave an 8000 s handshake limit, so one stalled TLS session held
  // the uploader for hours while lines were dropped.
  client.setHandshakeTimeout(static_cast<unsigned long>((connectMs + 999) / 1000));

  // Keep-alive: the TLS session stays open between posts, so the handshake
  // (which needs a large contiguous heap block) runs once, not every post.
  http.setReuse(true);
  http.setConnectTimeout(connectMs);
  if (!http.begin(client, url)) {
    out.code = -1;
    out.error = "HTTP begin failed";
    return out;
  }

  http.setTimeout(timeoutMs > 65535U ? 65535U : timeoutMs);
  http.addHeader("Authorization", String("Token ") + INFLUX_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  http.addHeader("Accept", "application/json");

  out.code = http.POST(body);
  if (out.code > 0) {
    out.response = http.getString();
  } else if (out.code < 0) {
    out.error = HTTPClient::errorToString(out.code);
    char sslErr[96];
    sslErr[0] = '\0';
    const int sslCode = client.lastError(sslErr, sizeof(sslErr));
    if (sslCode != 0 || sslErr[0] != '\0') {
      out.error += " ssl ";
      out.error += String(sslCode);
      if (sslErr[0] != '\0') {
        out.error += " ";
        out.error += sslErr;
      }
    }
    out.error += " heap ";
    out.error += String(ESP.getFreeHeap());
    out.error += " max_block ";
    out.error += String(ESP.getMaxAllocHeap());
  }
  http.end();
  return out;
}

inline void logWriteFailure(const PostOutcome& out, int attempt, bool hints) {
  Serial.print("[INFLUX] Write failed HTTP ");
  Serial.print(out.code);
  Serial.print(" (attempt ");
  Serial.print(attempt);
  Serial.print("/");
  Serial.print(kWriteAttempts);
  Serial.print(")");
  if (out.code < 0 && out.error.length() > 0) {
    Serial.print(" (");
    Serial.print(out.error);
    Serial.print(")");
  }
  if (out.response.length() > 0) {
    Serial.print(" — ");
    Serial.print(out.response.substring(0, 180));
  }
  Serial.println();
  if (!hints) {
    return;
  }
  if (out.code == 401 || out.code == 403) {
    Serial.println("[INFLUX] Token rejected — generate a new All Access or bucket R/W token");
  } else if (out.code == 404) {
    if (out.response.indexOf("bucket") >= 0 && out.response.indexOf("not found") >= 0) {
      Serial.print("[INFLUX] Bucket \"");
      Serial.print(INFLUX_BUCKET);
      Serial.println("\" not found — Influx UI → Load Data → Buckets → Create bucket");
    } else {
      Serial.println("[INFLUX] Org or bucket not found — use org ID from Settings → Organization Profile");
    }
  } else if (out.code == 400) {
    Serial.println("[INFLUX] Bad line protocol or timestamp — check Serial NTP sync");
  }
}

// One shared queue for every sketch that includes this header.
inline char (&unsentLines())[kBacklogSlots][kBacklogLineMax] {
  static char lines[kBacklogSlots][kBacklogLineMax];
  return lines;
}

inline uint8_t& unsentHead() {
  static uint8_t head = 0;
  return head;
}

inline uint8_t& unsentCount() {
  static uint8_t count = 0;
  return count;
}

inline void dropOldestUnsent() {
  if (unsentCount() == 0) {
    return;
  }
  unsentHead() = static_cast<uint8_t>((unsentHead() + 1) % kBacklogSlots);
  unsentCount()--;
}

inline void clearUnsent() {
  unsentHead() = 0;
  unsentCount() = 0;
}

inline int& inflightCount() {
  static int n = 0;
  return n;
}

inline volatile bool& controlHoldFlag() {
  static volatile bool hold = false;
  return hold;
}

// Uploader health for Serial INFLUX, MQTT telemetry and the dashboard.
// Written by the uploader task / control loop; read without locking (display only).
struct Stats {
  volatile unsigned long uploadedLines = 0;
  volatile unsigned long droppedLines = 0;   // outbox full too, or rejected by the server
  volatile unsigned long spilledLines = 0;   // RAM queue full: parked in the LittleFS outbox
  volatile unsigned long outboxUploaded = 0; // outbox lines delivered
  volatile unsigned long lastOkMs = 0;       // millis() of the last 204
  volatile unsigned long lastAttemptMs = 0;
  volatile int lastCode = 0;                 // last HTTP code (<0 = transport error)
  volatile unsigned long holdSinceMs = 0;    // 0 while not held
};

inline Stats& stats() {
  static Stats s;
  return s;
}

// Set once from setup() (FIRMWARE_VERSION is defined after this header).
inline const char*& firmwareVersion() {
  static const char* v = "";
  return v;
}

inline portMUX_TYPE& clientMux() {
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  return mux;
}

inline WiFiClientSecure*& activeClientSlot() {
  static WiFiClientSecure* client = nullptr;
  return client;
}

// Held by preemptForControl() while it stops the client, and by the uploader
// while it retires one, so a preempt never touches a deleted client.
inline SemaphoreHandle_t clientLifeMutex() {
  static SemaphoreHandle_t mu = xSemaphoreCreateMutex();
  return mu;
}

inline void publishActiveClient(WiFiClientSecure* client) {
  portENTER_CRITICAL(&clientMux());
  activeClientSlot() = client;
  portEXIT_CRITICAL(&clientMux());
}

inline SemaphoreHandle_t queueMutex() {
  static SemaphoreHandle_t mu = xSemaphoreCreateMutex();
  return mu;
}

inline void queueLock() {
  xSemaphoreTake(queueMutex(), portMAX_DELAY);
}

inline void queueUnlock() {
  xSemaphoreGive(queueMutex());
}

constexpr const char* kOutboxPath = "/influx_outbox.lp";
constexpr const char* kOutboxCursorPath = "/influx_outbox.cur";
constexpr size_t kOutboxMaxBytes = 192 * 1024;  // ~40 min of sensor lines

inline DurableQueue& outbox() {
  static DurableQueue q(kOutboxPath, kOutboxCursorPath, kOutboxMaxBytes);
  return q;
}

// Caller holds queueMutex().
inline bool rememberUnsent(const String& line) {
  if (line.length() == 0 || line.length() >= kBacklogLineMax) {
    return false;
  }
  if (unsentCount() == kBacklogSlots) {
    // RAM queue full (uplink slow or down). Park the new line on LittleFS;
    // the batch already inside HTTPS stays untouched.
    if (outbox().append(line.c_str())) {
      stats().spilledLines = stats().spilledLines + 1;
      return true;
    }
    stats().droppedLines = stats().droppedLines + 1;
    if (inflightCount() > 0) {
      return false;
    }
    dropOldestUnsent();
  }
  const uint8_t idx =
      static_cast<uint8_t>((unsentHead() + unsentCount()) % kBacklogSlots);
  strncpy(unsentLines()[idx], line.c_str(), kBacklogLineMax - 1);
  unsentLines()[idx][kBacklogLineMax - 1] = '\0';
  unsentCount()++;
  return true;
}

// One background attempt. 1 = written, 0 = retry later, -1 = rejected.
// Returns 0 immediately if control takes the hold mid-attempt.
inline int postLineProtocol(WiFiClientSecure& client, HTTPClient& http, const String& body) {
  if (!configured()) {
    Serial.println("[INFLUX] skip — secrets.h not configured");
    return 0;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return 0;
  }
  if (body.length() == 0) {
    return 0;
  }

  const unsigned long started = millis();
  for (int attempt = 1; attempt <= kWriteAttempts; attempt++) {
    if (controlHoldFlag()) {
      return 0;
    }
    const unsigned long elapsed = millis() - started;
    if (elapsed >= kWriteBudgetMs || kWriteBudgetMs - elapsed < 1000) {
      break;
    }
    const unsigned long remain = kWriteBudgetMs - elapsed;
    const uint16_t timeoutMs = remain > 65535UL ? 65535 : static_cast<uint16_t>(remain);
    const PostOutcome out = postOnce(client, http, body, timeoutMs);
    stats().lastAttemptMs = millis();
    stats().lastCode = out.code;
    if (out.code == 204) {
      stats().lastOkMs = millis();
      return 1;
    }
    if (controlHoldFlag()) {
      return 0;
    }
    const unsigned long after = millis() - started;
    const bool lastAttempt =
        attempt == kWriteAttempts || after + 1500 >= kWriteBudgetMs;
    logWriteFailure(out, attempt, writeErrorIsPermanent(out.code) || lastAttempt);
    if (writeErrorIsPermanent(out.code)) {
      return -1;
    }
    if (lastAttempt) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(kRetryGapMs));
  }
  return 0;
}

// Copy the oldest lines into one POST body and mark them in flight.
// Caller does not hold the mutex. Returns 0 when idle, held, or empty.
inline int beginUploadBatch(String& body) {
  queueLock();
  body = "";
  if (controlHoldFlag() || unsentCount() == 0 || inflightCount() > 0) {
    queueUnlock();
    return 0;
  }
  const int n = unsentCount() > kBatchMax ? kBatchMax : unsentCount();
  body.reserve(static_cast<unsigned int>(n) * 320);
  for (int i = 0; i < n; i++) {
    if (body.length() > 0) {
      body += '\n';
    }
    const uint8_t idx = static_cast<uint8_t>((unsentHead() + i) % kBacklogSlots);
    body += unsentLines()[idx];
  }
  inflightCount() = n;
  queueUnlock();
  return n;
}

inline void endUploadBatch(bool success, int n) {
  queueLock();
  if (success) {
    while (n > 0 && unsentCount() > 0) {
      dropOldestUnsent();
      n--;
    }
  }
  inflightCount() = 0;
  queueUnlock();
}

inline void preemptForControl();
inline void ensureUploader();

// Control loop only. Never opens a socket.
inline bool enqueueLine(const String& line) {
  ensureUploader();
  if (line.length() == 0 || line.length() >= kBacklogLineMax) {
    return false;
  }
  queueLock();
  const bool ok = rememberUnsent(line);
  const int waiting = unsentCount();
  queueUnlock();
  if (!ok) {
    static unsigned long lastDropMs = 0;
    const unsigned long now = millis();
    if (lastDropMs == 0 || now - lastDropMs > 15000) {
      lastDropMs = now;
      Serial.print("[INFLUX] queue full (");
      Serial.print(waiting);
      Serial.print("/");
      Serial.print(kBacklogSlots);
      Serial.println(") — kept the in-flight batch, dropped one new line");
    }
  }
  return ok;
}

inline void uploaderTask(void*);

inline void ensureUploader() {
  static bool started = false;
  if (started) {
    return;
  }
  const BaseType_t ok = xTaskCreatePinnedToCore(
      uploaderTask, "influx", 16384, nullptr, 1, nullptr, 0);
  if (ok == pdPASS) {
    started = true;
    Serial.println("[INFLUX] uploader task started (queue, preempted while pumps run)");
  } else {
    Serial.println("[INFLUX] uploader task failed to start");
  }
}

// Close an in-flight handshake and hold further posts until setControlHold(false).
inline void preemptForControl() {
  controlHoldFlag() = true;
  xSemaphoreTake(clientLifeMutex(), portMAX_DELAY);
  WiFiClientSecure* client = nullptr;
  portENTER_CRITICAL(&clientMux());
  client = activeClientSlot();
  portEXIT_CRITICAL(&clientMux());
  if (client != nullptr) {
    client->stop();
  }
  xSemaphoreGive(clientLifeMutex());
}

// Authoritative hold from the control loop. true aborts a post in progress.
inline void setControlHold(bool hold) {
  if (hold) {
    if (stats().holdSinceMs == 0) {
      stats().holdSinceMs = millis() | 1UL;
    }
    preemptForControl();
  } else {
    controlHoldFlag() = false;
    stats().holdSinceMs = 0;
  }
}

inline int queuedLines() {
  return unsentCount();
}

// Seconds since the last successful write, or -1 if none since boot.
inline long secondsSinceOk(unsigned long nowMs) {
  const unsigned long ok = stats().lastOkMs;
  return ok == 0 ? -1L : static_cast<long>((nowMs - ok) / 1000UL);
}

inline void printStats() {
  const unsigned long now = millis();
  const Stats& s = stats();
  Serial.print("[INFLUX] queue ");
  Serial.print(queuedLines());
  Serial.print("/");
  Serial.print(kBacklogSlots);
  Serial.print(" (in flight ");
  Serial.print(inflightCount());
  Serial.print("), hold ");
  Serial.print(controlHoldFlag() ? "ON" : "off");
  if (s.holdSinceMs != 0) {
    Serial.print(" for ");
    Serial.print((now - s.holdSinceMs) / 1000UL);
    Serial.print(" s");
  }
  Serial.print(", uploaded ");
  Serial.print(s.uploadedLines);
  Serial.print(", dropped ");
  Serial.print(s.droppedLines);
  Serial.print(", outbox ");
  Serial.print(outbox().pendingBytes());
  Serial.print(" B (parked ");
  Serial.print(s.spilledLines);
  Serial.print(", sent ");
  Serial.print(s.outboxUploaded);
  Serial.print(")");
  Serial.print(", last OK ");
  const long ago = secondsSinceOk(now);
  if (ago < 0) {
    Serial.print("never");
  } else {
    Serial.print(ago);
    Serial.print(" s ago");
  }
  Serial.print(", last HTTP ");
  Serial.println(s.lastCode);
}

// One keep-alive TLS session, reused while posts succeed. Any failure
// retires it (a WiFiClientSecure reused after a failed handshake kept failing
// with "Generic error"), and the next post builds a new one. A new client per
// post fragmented the heap until the RSA step could not allocate.
struct UploadSession {
  WiFiClientSecure* client = nullptr;
  HTTPClient* http = nullptr;
};

inline UploadSession& session() {
  static UploadSession s;
  return s;
}

inline void retireSession() {
  UploadSession& s = session();
  xSemaphoreTake(clientLifeMutex(), portMAX_DELAY);
  publishActiveClient(nullptr);
  xSemaphoreGive(clientLifeMutex());
  if (s.http != nullptr) {
    s.http->end();
    delete s.http;
    s.http = nullptr;
  }
  if (s.client != nullptr) {
    s.client->stop();
    delete s.client;
    s.client = nullptr;
  }
}

inline int postWithSession(const String& body) {
  UploadSession& s = session();
  if (s.client == nullptr) {
    s.client = new (std::nothrow) WiFiClientSecure();
    s.http = new (std::nothrow) HTTPClient();
    if (s.client == nullptr || s.http == nullptr) {
      retireSession();
      return 0;
    }
    s.client->setInsecure();
  }
  publishActiveClient(s.client);
  int result = 0;
  if (!controlHoldFlag()) {
    result = postLineProtocol(*s.client, *s.http, body);
  }
  xSemaphoreTake(clientLifeMutex(), portMAX_DELAY);
  publishActiveClient(nullptr);
  xSemaphoreGive(clientLifeMutex());
  // 1 sent, -1 rejected by the server: the connection is healthy. 0 is a
  // transport failure or a preempt: start the next post on a new session.
  if (result == 0) {
    retireSession();
  }
  return result;
}

// RAM queue empty: send up to kBatchMax lines from the LittleFS outbox.
// Returns 1 sent, 0 nothing to send or retry later, -1 rejected (skipped).
inline int drainOutboxOnce() {
  static char buf[kBatchMax * (kBacklogLineMax + 1)];
  const DurableQueue::Batch b = outbox().readBatch(buf, sizeof(buf), kBatchMax, kBacklogLineMax - 1);
  if (b.endOffset == 0) {
    return 0;
  }
  String body;
  const char* line = buf;
  for (int i = 0; i < b.lines; i++) {
    if (body.length() > 0) {
      body += '\n';
    }
    body += line;
    line += strlen(line) + 1;
  }
  if (b.lines == 0) {
    outbox().commit(b);  // only over-long lines: skip them
    return 0;
  }
  const int result = postWithSession(body);
  if (result > 0) {
    outbox().commit(b);
    stats().uploadedLines += static_cast<unsigned long>(b.lines);
    stats().outboxUploaded += static_cast<unsigned long>(b.lines);
    Serial.print("[INFLUX] uploaded ");
    Serial.print(b.lines);
    Serial.println(" outbox line(s)");
    return 1;
  }
  if (result < 0) {
    outbox().commit(b);
    stats().droppedLines += static_cast<unsigned long>(b.lines);
    Serial.println("[INFLUX] outbox batch rejected — skipped");
    return -1;
  }
  return 0;
}

inline void uploaderTask(void*) {
  unsigned long lastFailLogMs = 0;
  for (;;) {
    if (controlHoldFlag() || WiFi.status() != WL_CONNECTED || !configured()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    String body;
    const int n = beginUploadBatch(body);
    if (n <= 0) {
      if (drainOutboxOnce() != 0) {
        continue;
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (controlHoldFlag()) {
      endUploadBatch(false, n);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    const int result = postWithSession(body);

    const bool held = controlHoldFlag();
    if (result > 0) {
      stats().uploadedLines += static_cast<unsigned long>(n);
      endUploadBatch(true, n);
      Serial.print("[INFLUX] uploaded ");
      Serial.print(n);
      Serial.println(" queued line(s)");
      if (held) {
        vTaskDelay(pdMS_TO_TICKS(50));
      }
      continue;
    }

    endUploadBatch(false, n);
    if (held) {
      Serial.println("[INFLUX] upload preempted for control — queued for retry");
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (result < 0) {
      // Auth, bucket, or a bad line will not start working on retry.
      endUploadBatch(true, n);
      stats().droppedLines += static_cast<unsigned long>(n);
      Serial.println("[INFLUX] write rejected — dropped that batch");
      continue;
    }

    const unsigned long now = millis();
    if (lastFailLogMs == 0 || now - lastFailLogMs > 15000) {
      lastFailLogMs = now;
      Serial.println("[INFLUX] upload failed — lines stay queued");
    }
    // Slice the backoff so a new control hold is noticed promptly.
    for (int i = 0; i < 20 && !controlHoldFlag(); i++) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// Control-loop entry. Queues the line and returns. The uploader task posts it
// once actuators are idle. WiFi-down lines stay queued.
inline bool publishLine(const String& line) {
  if (!configured()) {
    return false;
  }
  return enqueueLine(line);
}

inline bool uploadSensor(
    const SensorPoint& p,
    bool ntpValid,
    uint32_t ntpEpochAtSync,
    unsigned long millisAtSync) {
  String line = "sensor_reading";
  appendTag(line, "device", INFLUX_DEVICE_ID);
  appendTag(line, "stage", p.stage);
  if (p.baselineCapture) {
    appendTag(line, "capture", "baseline");
  }

  line += ' ';
  bool first = true;
  appendFieldInt(line, first, "uptime_ms", static_cast<int>(p.uptimeMs));
  appendFieldStr(line, first, "recorded_at_utc", p.recordedAtUtc);
  appendFieldFloat(line, first, "water_temp_c", p.waterTempC, 2);
  appendFieldFloat(line, first, "ambient_temp_c", p.ambientTempC, 2);
  appendFieldFloat(line, first, "ambient_humidity_pct", p.ambientHumidityPct, 1);
  appendFieldInt(line, first, "light_adc", p.lightAdc);
  appendFieldInt(line, first, "water_level_pct", p.waterLevelPct);
  appendFieldInt(line, first, "water_level_raw_pct", p.waterLevelRawPct);
  appendFieldInt(line, first, "water_target_pct", p.waterTargetPct);
  appendFieldInt(line, first, "fill_0_pct", p.fill0Pct);
  appendFieldFloat(line, first, "current_drawdown_mm", p.currentDrawdownMm, 1);
  appendFieldInt(line, first, "target_drawdown_mm", p.targetDrawdownMm);
  appendFieldInt(line, first, "tds_ppm", p.tdsPpm);
  appendFieldFloat(line, first, "tds_ppm_ema", p.tdsPpmEma, 1);
  appendFieldFloat(line, first, "tds_ppm_nutrient", p.tdsPpmNutrient, 1);
  appendFieldFloat(line, first, "tds_ph_salt_ppm", p.tdsPhSaltPpm, 1);
  appendFieldFloat(line, first, "tds_kappa_up", p.tdsKappaUp, 2);
  appendFieldFloat(line, first, "tds_kappa_down", p.tdsKappaDown, 2);
  appendFieldFloat(line, first, "tds_ph_up_ml", p.tdsPhUpMl, 3);
  appendFieldFloat(line, first, "tds_ph_down_ml", p.tdsPhDownMl, 3);
  appendFieldInt(line, first, "tds_salt_correction_live", p.tdsSaltCorrectionLive);
  appendFieldInt(line, first, "tds_kappa_provisional", p.tdsKappaProvisional);
  appendFieldInt(line, first, "co2_ppm", p.co2Ppm);
  appendFieldFloat(line, first, "ph_voltage", p.phVoltage, 3);
  appendFieldFloat(line, first, "ph_raw", p.phRaw, 3);
  appendFieldFloat(line, first, "ph_filtered", p.phFiltered, 3);
  appendFieldFloat(line, first, "ph_ema", p.phEma, 3);
  appendFieldFloat(line, first, "ph", p.ph, 3);
  appendFieldFloat(line, first, "ph_target_min", p.phTargetMin, 2);
  appendFieldFloat(line, first, "ph_target_max", p.phTargetMax, 2);
  appendFieldStr(line, first, "ph_dose_state", p.phDoseState);
  appendFieldFloat(line, first, "ph_session_ml", p.phSessionMl, 2);
  appendFieldFloat(line, first, "ec_session_ml", p.ecSessionMl, 2);
  appendFieldFloat(line, first, "gain_ph_up", p.gainPhUp, 4);
  appendFieldFloat(line, first, "gain_ph_down", p.gainPhDown, 4);
  appendFieldFloat(line, first, "gain_ec", p.gainEc, 6);
  appendFieldBool(line, first, "ready_to_seed", p.readyToSeed);
  appendFieldBool(line, first, "baseline_capture", p.baselineCapture);
  appendFieldBool(line, first, "water_level_sensor_fault", p.waterLevelSensorFault);
  appendFieldInt(line, first, "water_pump_on", p.waterPumpOn);
  appendFieldInt(line, first, "nutrient_pump_on", p.nutrientPumpOn);
  appendFieldInt(line, first, "ph_up_pump_on", p.phUpPumpOn);
  appendFieldInt(line, first, "ph_down_pump_on", p.phDownPumpOn);
  appendFieldInt(line, first, "grow_light_on", p.growLightOn);
  appendFieldInt(line, first, "grow_light_brightness_pct", p.growLightBrightnessPct);
  appendFieldInt(line, first, "grow_light_animating", p.growLightAnimating);
  appendFieldStr(line, first, "grow_light_mode", p.growLightMode);
  appendFieldInt(line, first, "grow_light_photoperiod_h", p.growLightPhotoperiodH);
  appendFieldStr(line, first, "reset_reason", p.resetReason);
  appendFieldInt(line, first, "boot_count", p.bootCount);
  appendFieldInt(line, first, "brownout_count", p.brownoutCount);
  appendFieldInt(line, first, "last_reset_brownout", p.lastResetBrownout);
  appendFieldInt(line, first, "rail_mv", p.railMv);
  appendFieldInt(line, first, "free_heap_b", p.freeHeapB);
  appendFieldSignedInt(line, first, "wifi_rssi_dbm", p.wifiRssiDbm, p.wifiRssiValid);
  appendFieldStr(line, first, "firmware_version", firmwareVersion());
  if (p.bootId > 0) {
    appendFieldInt(line, first, "boot_id", static_cast<int>(p.bootId));
    appendFieldInt(line, first, "seq", static_cast<int>(p.seq));
  }

  line += ' ';
  appendUint64(line, epochMs(ntpValid, ntpEpochAtSync, millisAtSync, p.uptimeMs));

  const bool ok = publishLine(line);
  if (ok) {
    Serial.println("[INFLUX] sensor_reading queued");
  }
  return ok;
}

inline bool uploadControlEvent(
    const ControlEventLog::Row& row,
    bool ntpValid,
    uint32_t ntpEpochAtSync,
    unsigned long millisAtSync) {
  String line = "control_event";
  appendTag(line, "device", INFLUX_DEVICE_ID);
  appendTag(line, "stage", row.stage);
  appendTag(line, "event", row.event);
  if (row.phDoseDir != nullptr && row.phDoseDir[0] != '\0') {
    appendTag(line, "ph_dose_dir", row.phDoseDir);
  }

  line += ' ';
  bool first = true;
  appendFieldInt(line, first, "uptime_ms", static_cast<int>(row.uptimeMs));
  appendFieldStr(line, first, "recorded_at_utc", row.recordedAtUtc);
  appendFieldInt(line, first, "current_level_pct", row.currentLevelPct);
  appendFieldFloat(line, first, "current_drawdown_mm", row.currentDrawdownMm, 1);
  appendFieldInt(line, first, "target_depth_mm", row.targetDepthMm);
  appendFieldFloat(line, first, "ec_before", row.ecBefore, 0);
  appendFieldFloat(line, first, "ec_dose_ml", row.ecDoseMl, 2);
  appendFieldFloat(line, first, "ec_after", row.ecAfter, 0);
  appendFieldFloat(line, first, "ph_before", row.phBefore, 3);
  appendFieldFloat(line, first, "ph_dose_ml", row.phDoseMl, 2);
  appendFieldFloat(line, first, "ph_after", row.phAfter, 3);
  appendFieldFloat(line, first, "gain_up", row.gainUp, 4);
  appendFieldFloat(line, first, "gain_down", row.gainDown, 4);
  appendFieldFloat(line, first, "gain_ec", row.gainEc, 6);
  appendFieldFloat(line, first, "water_temp_c", row.waterTempC, 2);
  appendFieldFloat(line, first, "cumulative_session_ml", row.cumulativeSessionMl, 2);
  appendFieldStr(line, first, "alert_flags", row.alertFlags);
  appendFieldStr(line, first, "firmware_version", firmwareVersion());

  line += ' ';
  appendUint64(line, epochMs(ntpValid, ntpEpochAtSync, millisAtSync, row.uptimeMs));

  const bool ok = publishLine(line);
  if (ok) {
    Serial.print("[INFLUX] control_event queued: ");
    Serial.println(row.event != nullptr ? row.event : "");
  }
  return ok;
}

struct AmbientPoint {
  const char* recordedAtUtc = "";
  unsigned long uptimeMs = 0;
  float ambientTempC = NAN;
  float ambientHumidityPct = NAN;
  int lightAdc = -1;
  int co2Ppm = -1;
  bool dhtOk = false;
  bool ldrOk = false;
  bool co2Ok = false;
};

inline bool uploadAmbientReading(
    const AmbientPoint& p,
    bool ntpValid,
    uint32_t ntpEpochAtSync,
    unsigned long millisAtSync) {
  String line = "ambient_reading";
  appendTag(line, "device", INFLUX_DEVICE_ID);
  appendTag(line, "source", "ambient_test");

  line += ' ';
  bool first = true;
  appendFieldInt(line, first, "uptime_ms", static_cast<int>(p.uptimeMs));
  appendFieldStr(line, first, "recorded_at_utc", p.recordedAtUtc);
  appendFieldFloat(line, first, "ambient_temp_c", p.ambientTempC, 2);
  appendFieldFloat(line, first, "ambient_humidity_pct", p.ambientHumidityPct, 1);
  appendFieldInt(line, first, "light_adc", p.lightAdc);
  appendFieldInt(line, first, "co2_ppm", p.co2Ppm);
  appendFieldBool(line, first, "dht_ok", p.dhtOk);
  appendFieldBool(line, first, "ldr_ok", p.ldrOk);
  appendFieldBool(line, first, "co2_ok", p.co2Ok);

  line += ' ';
  appendUint64(line, epochMs(ntpValid, ntpEpochAtSync, millisAtSync, p.uptimeMs));

  const bool ok = publishLine(line);
  if (ok) {
    Serial.println("[INFLUX] ambient_reading queued");
  }
  return ok;
}

inline bool uploadTestPoint(
    bool ntpValid,
    uint32_t ntpEpochAtSync,
    unsigned long millisAtSync) {
  String line = "connectivity_test";
  appendTag(line, "device", INFLUX_DEVICE_ID);
  line += " value=1i ";
  appendUint64(line, epochMs(ntpValid, ntpEpochAtSync, millisAtSync, millis()));
  const bool ok = publishLine(line);
  if (ok) {
    Serial.println("[INFLUX] test point queued — uploader sends it when control is idle");
  }
  return ok;
}

inline void printSetupHint() {
  if (!configured()) {
    Serial.println("[INFLUX] Not configured — set INFLUX_URL / ORG / BUCKET / TOKEN in secrets.h");
    return;
  }
  Serial.print("[INFLUX] Cloud upload enabled → ");
  Serial.print(INFLUX_URL);
  Serial.print("  org=");
  Serial.print(INFLUX_ORG);
  Serial.print("  bucket=");
  Serial.println(INFLUX_BUCKET);
  ensureUploader();
  if (strchr(INFLUX_ORG, ' ') != nullptr) {
    Serial.println("[INFLUX] Tip: org names with spaces often fail. Prefer the org ID from Influx UI (Settings → About).");
  }
}

}  // namespace InfluxCloud

#endif  // SMART_GARDEN_INFLUX_CLOUD_H
