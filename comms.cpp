/* Network task: WiFi, NTP and MQTT off the control loop (see README "Data
 * paths"). The loop hands over finished payloads and never waits on a
 * socket; this task never touches pumps or sensors. When MQTT is down and
 * the Pi's serial bridge is talking on the USB cable, the same messages go
 * out on Serial instead. */
#include "garden.h"

#if ENABLE_WIFI
#include "shared/durable_queue.h"
#include <sys/time.h>

static portMUX_TYPE ntpMux = portMUX_INITIALIZER_UNLOCKED;

NtpMap ntpSnapshot() {
  NtpMap m;
  portENTER_CRITICAL(&ntpMux);
  m.valid = ntpEpochValid;
  m.epochAtSync = ntpEpochSecondsAtSync;
  m.millisAtSync = millisAtNtpSyncMs;
  portEXIT_CRITICAL(&ntpMux);
  return m;
}

bool recordedAtUtcForUptime(unsigned long uptimeMs, char* buf, size_t buflen) {
  const NtpMap m = ntpSnapshot();
  return SensorDataStore::recordedAtUtcFromNtp(m.valid, m.epochAtSync, m.millisAtSync,
                                               uptimeMs, buf, buflen);
}

namespace Comms {

namespace {

#if ENABLE_MQTT
// --- Big slots: telemetry JSON (live readings and command-triggered status).
enum SlotState : uint8_t { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_SENDING };

struct BigSlot {
  volatile SlotState state = SLOT_FREE;
  bool isReading = false;
  uint32_t seq = 0;
  uint32_t order = 0;
  size_t len = 0;
  char json[MQTT_TELEMETRY_MAX_BYTES];
  char csv[SensorDataStore::CSV_LINE_MAX];
};

constexpr int kBigSlots = 2;
BigSlot bigSlots[kBigSlots];
uint32_t nextOrder = 1;
portMUX_TYPE slotMux = portMUX_INITIALIZER_UNLOCKED;

// --- Small messages: alerts (retained), command acks, control events.
struct SmallMsg {
  char topic[64];
  bool retained;
  uint16_t len;
  char payload[COMMS_SMALL_PAYLOAD_MAX];
};
QueueHandle_t smallQueue = nullptr;

// --- Inbound commands, executed on the loop task.
struct InboundCmd {
  char topic[64];
  uint16_t len;
  char payload[COMMS_INBOX_PAYLOAD_MAX];
};
QueueHandle_t inbox = nullptr;

DurableQueue outbox(SensorDataStore::MQTT_OUTBOX_PATH,
                    SensorDataStore::MQTT_OUTBOX_CURSOR_PATH,
                    COMMS_MQTT_OUTBOX_MAX_BYTES);

#endif  // ENABLE_MQTT

volatile bool mqttUp = false;
volatile bool wifiUp = false;
// millis() of the last "@LINK" or "@CMD" line from the Pi (0 = never).
volatile unsigned long serialLinkLastMs = 0;
volatile bool calPaused = false;
volatile bool bulkHold = false;
volatile bool connectRequested = false;
TaskHandle_t taskHandle = nullptr;

Stats stats_;
unsigned long mqttBackoffMs = 0;
unsigned long nextMqttAttemptMs = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastReplayMs = 0;
unsigned long lastNtpLatchMs = 0;

bool serialLinkFresh() {
  const unsigned long last = serialLinkLastMs;
  // Signed: the loop task stores millis() | 1, which can be 1 ms ahead of
  // millis() here, and an unsigned difference would wrap to "stale".
  const long age = static_cast<long>(millis() - last);
  return last != 0 && age < static_cast<long>(SERIAL_LINK_TIMEOUT_MS);
}

#if ENABLE_MQTT
// Link for this network-task pass. MQTT wins when both are up, so a message
// never goes out twice.
enum Link : uint8_t { LINK_NONE, LINK_MQTT, LINK_SERIAL };
volatile Link activeLink = LINK_NONE;

const char* linkName() {
  return activeLink == LINK_SERIAL ? "serial" : "MQTT";
}

// One write call: HardwareSerial holds the UART lock for the whole buffer, so
// log lines from the loop task cannot land inside a frame. The leading
// newline ends any half-printed log line first.
bool writeSerialFrame(const char* topic, const char* payload, size_t len, bool retained) {
  static char frame[MQTT_TELEMETRY_MAX_BYTES + 96];  // network task only
  const int head = snprintf(frame, sizeof(frame), "\n@MQ %c %s ", retained ? 'r' : '-', topic);
  if (head <= 0 || static_cast<size_t>(head) + len + 1 > sizeof(frame)) {
    return false;
  }
  memcpy(frame + head, payload, len);
  for (size_t i = head; i < head + len; i++) {
    if (frame[i] == '\n' || frame[i] == '\r') {
      frame[i] = ' ';
    }
  }
  frame[head + len] = '\n';
  const size_t total = head + len + 1;
  return Serial.write(reinterpret_cast<const uint8_t*>(frame), total) == total;
}

void onMessage(char* topic, byte* payload, unsigned int length) {
  if (inbox == nullptr) {
    return;
  }
  InboundCmd cmd;
  if (length >= sizeof(cmd.payload)) {
    stats_.inboxDropped++;
    Serial.println("[NET] command too long — dropped");
    return;
  }
  strncpy(cmd.topic, topic, sizeof(cmd.topic) - 1);
  cmd.topic[sizeof(cmd.topic) - 1] = '\0';
  memcpy(cmd.payload, payload, length);
  cmd.payload[length] = '\0';
  cmd.len = static_cast<uint16_t>(length);
  // Power goes straight to the HMI: the inbox is not read in standby and is
  // cleared at ON, so a queued power-on would never run.
  if (strstr(cmd.payload, "\"power\"") != nullptr) {
    JsonDocument doc;
    if (!deserializeJson(doc, cmd.payload) && strcmp(doc["cmd"] | "", "power") == 0) {
      Serial.print("[MQTT] power command: ");
      Serial.println(cmd.payload);
      Hmi::requestRemote(doc["action"] | "status");
      return;
    }
  }
  if (xQueueSend(inbox, &cmd, 0) != pdTRUE) {
    stats_.inboxDropped++;
    Serial.println("[NET] command inbox full — dropped");
  }
}
#endif  // ENABLE_MQTT

// Latch wall-clock time once SNTP has it, and again every few hours so the
// millis()-to-epoch mapping does not drift.
void latchNtp() {
  const unsigned long now = millis();
  if (ntpEpochValid && now - lastNtpLatchMs < NTP_RELATCH_INTERVAL_MS) {
    return;
  }
  const time_t epoch = time(nullptr);
  if (epoch < 1700000000) {
    return;  // SNTP has not answered yet
  }
  portENTER_CRITICAL(&ntpMux);
  ntpEpochSecondsAtSync = static_cast<uint32_t>(epoch);
  millisAtNtpSyncMs = now;
  ntpTimeSynced = true;
  ntpEpochValid = true;
  portEXIT_CRITICAL(&ntpMux);
  if (lastNtpLatchMs == 0) {
    Serial.println("[NET] NTP time latched");
  }
  lastNtpLatchMs = now | 1UL;
}

void ensureWifi() {
  const bool up = WiFi.status() == WL_CONNECTED;
  if (up && !wifiUp) {
    // Rejoined (here or by the driver's auto-reconnect): retry MQTT now
    // instead of waiting out a backoff built up while WiFi was down.
    mqttBackoffMs = 0;
    connectRequested = true;
  }
  wifiUp = up;
  if (up) {
    return;
  }
  const unsigned long now = millis();
  const unsigned long retryMs =
      serialLinkFresh() ? WIFI_RETRY_WITH_SERIAL_LINK_MS : WIFI_RETRY_INTERVAL_MS;
  if (lastWifiAttemptMs != 0 && now - lastWifiAttemptMs < retryMs) {
    return;
  }
  lastWifiAttemptMs = now | 1UL;
  stats_.wifiAttempts++;
  Serial.println("[NET] WiFi down — reconnecting (network task; control loop keeps running)");
  wifiUp = connectWifi();
  if (wifiUp) {
    mqttBackoffMs = 0;
    connectRequested = true;
  }
}

#if ENABLE_MQTT
void scheduleMqttRetry(bool failed) {
  if (!failed) {
    mqttBackoffMs = 0;
    return;
  }
  mqttBackoffMs = mqttBackoffMs == 0
                      ? MQTT_RECONNECT_MIN_MS
                      : (mqttBackoffMs * 2 > MQTT_RECONNECT_MAX_MS ? MQTT_RECONNECT_MAX_MS
                                                                  : mqttBackoffMs * 2);
  nextMqttAttemptMs = millis() + mqttBackoffMs;
  Serial.print("[NET] MQTT retry in ");
  Serial.print(mqttBackoffMs / 1000UL);
  Serial.println(" s (MQTT CONNECT tries now)");
}

void ensureMqtt() {
  if (mqttClient.connected()) {
    mqttUp = true;
    return;
  }
  if (mqttUp) {
    mqttUp = false;
    stats_.mqttDrops++;
    Serial.println("[NET] MQTT connection lost");
    scheduleMqttRetry(true);
  }
  if (calPaused) {
    return;
  }
  const bool due = connectRequested ||
                   static_cast<long>(millis() - nextMqttAttemptMs) >= 0;
  if (!due) {
    return;
  }
  connectRequested = false;
  stats_.mqttAttempts++;
  if (connectMqtt()) {
    mqttUp = true;
    stats_.mqttConnects++;
    scheduleMqttRetry(false);
  } else {
    scheduleMqttRetry(true);
  }
}

// Oldest READY slot, marked SENDING. nullptr when none.
BigSlot* takeReadySlot() {
  BigSlot* pick = nullptr;
  portENTER_CRITICAL(&slotMux);
  for (BigSlot& s : bigSlots) {
    if (s.state == SLOT_READY && (pick == nullptr || s.order < pick->order)) {
      pick = &s;
    }
  }
  if (pick != nullptr) {
    pick->state = SLOT_SENDING;
  }
  portEXIT_CRITICAL(&slotMux);
  return pick;
}

void releaseSlot(BigSlot* s) {
  portENTER_CRITICAL(&slotMux);
  s->state = SLOT_FREE;
  portEXIT_CRITICAL(&slotMux);
}

// A reading the live path could not deliver goes to the outbox for replay.
void parkReading(BigSlot* s) {
  if (!s->isReading || s->csv[0] == '\0') {
    stats_.statusDropped++;
    return;
  }
  if (outbox.append(s->csv)) {
    stats_.parked++;
  } else {
    stats_.outboxRejected++;
  }
}

void sendBigSlots(bool linkUp) {
  BigSlot* s;
  while ((s = takeReadySlot()) != nullptr) {
    bool ok = false;
    if (linkUp) {
      ok = linkPublish(MQTT_TOPIC, s->json, s->len, false);
    }
    if (ok) {
      stats_.livePublished++;
      Serial.print("[NET] ");
      Serial.print(linkName());
      Serial.print(" ");
      Serial.print(s->isReading ? "reading" : "status");
      if (s->isReading) {
        Serial.print(" seq=");
        Serial.print(s->seq);
      }
      Serial.print(" published (");
      Serial.print(s->len);
      Serial.println(" bytes)");
    } else {
      if (linkUp) {
        stats_.liveFailed++;
      }
      parkReading(s);
    }
    releaseSlot(s);
  }
}

void sendSmall(bool linkUp) {
  SmallMsg m;
  while (smallQueue != nullptr && xQueueReceive(smallQueue, &m, 0) == pdTRUE) {
    if (linkUp && linkPublish(m.topic, m.payload, m.len, m.retained)) {
      stats_.smallPublished++;
    } else {
      stats_.smallDropped++;
    }
  }
}

// Replay a few outbox rows per pass while the live path is idle.
void replayStep() {
  if (bulkHold || millis() - lastReplayMs < COMMS_REPLAY_INTERVAL_MS) {
    return;
  }
  lastReplayMs = millis();
  static char buf[COMMS_REPLAY_BATCH * (SensorDataStore::CSV_LINE_MAX + 1)];
  const DurableQueue::Batch b =
      outbox.readBatch(buf, sizeof(buf), COMMS_REPLAY_BATCH, SensorDataStore::CSV_LINE_MAX);
  if (b.endOffset == 0) {
    return;
  }
  const char* line = buf;
  for (int i = 0; i < b.lines; i++) {
    const int r = publishCsvRowToMqtt(line);
    if (r == 0) {
      // Link failed mid-batch: nothing is committed, so the rows already sent
      // go again next time. The Pi drops them by (boot_id, seq).
      return;
    }
    if (r > 0) {
      stats_.replayed++;
    } else {
      stats_.replayBadRows++;
    }
    line += strlen(line) + 1;
  }
  outbox.commit(b);
  if (outbox.pendingBytes() == 0) {
    Serial.print("[NET] MQTT outbox replay complete (");
    Serial.print(stats_.replayed);
    Serial.println(" rows since boot)");
  }
}
#endif  // ENABLE_MQTT

void task(void*) {
  // First WiFi join happens here, so setup() and the control loop start at once.
  wifiUp = connectWifi();
  lastWifiAttemptMs = millis() | 1UL;
  nextMqttAttemptMs = millis();
  for (;;) {
    ensureWifi();
    // With only the serial link, the Pi's "@LINK <epoch>" sets the clock.
    if (wifiUp || serialLinkFresh()) {
      latchNtp();
    }
#if ENABLE_MQTT
    if (wifiUp) {
      ensureMqtt();
    } else if (mqttUp) {
      mqttUp = false;
      stats_.mqttDrops++;
      mqttClient.disconnect();
    }
    if (calPaused && mqttClient.connected()) {
      mqttClient.disconnect();
      mqttUp = false;
    }
    const bool mqttLink = mqttUp && mqttClient.connected();
    // Calibration keeps Serial for the CAL prompts, so no frames then.
    activeLink = mqttLink ? LINK_MQTT
                    : (!calPaused && serialLinkFresh() ? LINK_SERIAL : LINK_NONE);
    const bool linkUp = activeLink != LINK_NONE;
    if (mqttLink) {
      mqttClient.loop();
    }
    sendSmall(linkUp);
    sendBigSlots(linkUp);
    if (linkUp) {
      replayStep();
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(COMMS_TASK_PERIOD_MS));
  }
}

}  // namespace

void begin() {
  if (taskHandle != nullptr) {
    return;
  }
#if ENABLE_MQTT
  smallQueue = xQueueCreate(COMMS_SMALL_QUEUE_DEPTH, sizeof(SmallMsg));
  inbox = xQueueCreate(COMMS_INBOX_DEPTH, sizeof(InboundCmd));
  mqttClient.setBufferSize(MQTT_TELEMETRY_MAX_BYTES);
  mqttClient.setCallback(onMessage);
  mqttClient.setKeepAlive(MQTT_KEEPALIVE_SECONDS);
  mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SECONDS);
#endif
  const BaseType_t ok = xTaskCreatePinnedToCore(task, "net", COMMS_TASK_STACK, nullptr,
                                                COMMS_TASK_PRIORITY, &taskHandle, 0);
  Serial.println(ok == pdPASS ? "[NET] network task started (WiFi, NTP, MQTT on core 0)"
                              : "[NET] network task failed to start");
}

#if ENABLE_MQTT
BigSlot* acquire() {
  BigSlot* got = nullptr;
  portENTER_CRITICAL(&slotMux);
  for (BigSlot& s : bigSlots) {
    if (s.state == SLOT_FREE) {
      s.state = SLOT_FILLING;
      got = &s;
      break;
    }
  }
  portEXIT_CRITICAL(&slotMux);
  return got;
}

bool submitTelemetry(const JsonDocument& doc, uint32_t seq, const char* csvLine) {
  const bool isReading = seq > 0;
  BigSlot* s = acquire();
  if (s == nullptr) {
    // Network task is behind (or stuck in a connect). Keep the reading for
    // replay; a status snapshot is superseded by the next one anyway.
    if (isReading && csvLine != nullptr && outbox.append(csvLine)) {
      stats_.parked++;
    } else if (isReading) {
      stats_.outboxRejected++;
    } else {
      stats_.statusDropped++;
    }
    return false;
  }
  const size_t len = serializeJson(doc, s->json, sizeof(s->json));
  if (len == 0 || len >= sizeof(s->json)) {
    Serial.print("[ERROR] MQTT JSON too large (need ");
    Serial.print(measureJson(doc));
    Serial.println(" bytes)");
    releaseSlot(s);
    if (isReading && csvLine != nullptr) {
      outbox.append(csvLine);
    }
    return false;
  }
  s->len = len;
  s->isReading = isReading;
  s->seq = seq;
  if (csvLine != nullptr) {
    strncpy(s->csv, csvLine, sizeof(s->csv) - 1);
    s->csv[sizeof(s->csv) - 1] = '\0';
  } else {
    s->csv[0] = '\0';
  }
  portENTER_CRITICAL(&slotMux);
  s->order = nextOrder++;
  s->state = SLOT_READY;
  portEXIT_CRITICAL(&slotMux);
  return true;
}

bool publish(const char* topic, const char* payload, size_t len, bool retained) {
  if (smallQueue == nullptr || len >= COMMS_SMALL_PAYLOAD_MAX) {
    stats_.smallDropped++;
    return false;
  }
  SmallMsg m;
  strncpy(m.topic, topic, sizeof(m.topic) - 1);
  m.topic[sizeof(m.topic) - 1] = '\0';
  memcpy(m.payload, payload, len);
  m.payload[len] = '\0';
  m.len = static_cast<uint16_t>(len);
  m.retained = retained;
  if (xQueueSend(smallQueue, &m, 0) != pdTRUE) {
    stats_.smallDropped++;
    return false;
  }
  return true;
}

void serviceInbox() {
  if (inbox == nullptr) {
    return;
  }
  InboundCmd cmd;
  // One command per pass keeps a burst from delaying pump timers.
  if (xQueueReceive(inbox, &cmd, 0) == pdTRUE) {
    onMqttCommand(cmd.topic, reinterpret_cast<byte*>(cmd.payload), cmd.len);
  }
}

int clearInbox() {
  if (inbox == nullptr) {
    return 0;
  }
  int dropped = 0;
  InboundCmd cmd;
  while (xQueueReceive(inbox, &cmd, 0) == pdTRUE) {
    dropped++;
  }
  return dropped;
}

void requestConnect(const char* via) {
  Serial.print("[MQTT] Connect requested");
  if (via != nullptr && via[0] != '\0') {
    Serial.print(" (");
    Serial.print(via);
    Serial.print(")");
  }
  Serial.println(" — the network task tries on its next pass");
  connectRequested = true;
}

bool linkPublish(const char* topic, const char* payload, size_t len, bool retained) {
  if (activeLink == LINK_MQTT) {
    return mqttClient.publish(topic, reinterpret_cast<const uint8_t*>(payload), len, retained);
  }
  if (activeLink == LINK_SERIAL) {
    return writeSerialFrame(topic, payload, len, retained);
  }
  return false;
}

#endif  // ENABLE_MQTT

void handleSerialLinkLine(const char* line) {
  if (strncmp(line, "@LINK", 5) == 0) {
    const bool wasUp = serialLinkFresh();
    serialLinkLastMs = millis() | 1UL;
    const long long epoch = atoll(line + 5);
    if (epoch > 1700000000LL && time(nullptr) < 1700000000) {
      struct timeval tv = {static_cast<time_t>(epoch), 0};
      settimeofday(&tv, nullptr);
    }
    if (!wasUp) {
      Serial.println("[NET] serial link to the Pi up");
    }
    // The bridge forwards dashboard commands only while this says serial, so
    // a command the node also got over MQTT does not run twice.
    char reply[24];
    const int n = snprintf(reply, sizeof(reply), "\n@LS %s\n", mqttUp ? "mqtt" : "serial");
    Serial.write(reinterpret_cast<const uint8_t*>(reply), n);
    return;
  }
#if ENABLE_MQTT
  if (strncmp(line, "@CMD ", 5) == 0) {
    serialLinkLastMs = millis() | 1UL;
    const char* payload = line + 5;
    onMessage(const_cast<char*>(MQTT_CMD_TOPIC),
              reinterpret_cast<byte*>(const_cast<char*>(payload)), strlen(payload));
    return;
  }
#endif
  Serial.print("[NET] unknown serial-link line: ");
  Serial.println(line);
}

bool linkUp() {
  return mqttUp || (!calPaused && serialLinkFresh());
}

bool serialLinkActive() {
  return serialLinkFresh();
}

void setCalPause(bool paused) {
  calPaused = paused;
  if (!paused) {
    connectRequested = true;
  }
}

void setBulkHold(bool hold) {
  bulkHold = hold;
}

bool mqttConnected() {
  return mqttUp;
}

bool wifiConnected() {
  return wifiUp;
}

bool calPauseActive() {
  return calPaused;
}

const Stats& stats() {
  return stats_;
}

size_t outboxPendingBytes() {
#if ENABLE_MQTT
  return outbox.pendingBytes();
#else
  return 0;
#endif
}

void printStatus() {
  Serial.print("[NET] WiFi ");
  Serial.print(wifiUp ? "up" : "DOWN");
  Serial.print(", NTP ");
  Serial.print(ntpEpochValid ? "ok" : "not yet");
  Serial.print(", MQTT ");
  Serial.print(mqttUp ? "up" : "DOWN");
  if (calPaused) {
    Serial.print(" (paused for CAL)");
  } else if (!mqttUp && mqttBackoffMs > 0) {
    Serial.print(" (next try in ");
    const long wait = static_cast<long>(nextMqttAttemptMs - millis());
    Serial.print(wait > 0 ? wait / 1000L : 0L);
    Serial.print(" s)");
  }
  Serial.println();
  Serial.print("[NET] serial link to the Pi ");
  Serial.println(serialLinkFresh() ? "up" : "down");
#if ENABLE_MQTT
  Serial.print("[NET] broker ");
  Serial.print(mqttBrokerHost());
  Serial.print(":");
  Serial.print(MQTT_BROKER_PORT);
  Serial.print(", client state ");
  Serial.print(mqttClient.state());
  Serial.print(" (");
  Serial.print(mqttStateLabel(mqttClient.state()));
  Serial.println(")");
  Serial.print("[NET] live published ");
  Serial.print(stats_.livePublished);
  Serial.print(", live failed ");
  Serial.print(stats_.liveFailed);
  Serial.print(", parked for replay ");
  Serial.print(stats_.parked);
  Serial.print(", replayed ");
  Serial.print(stats_.replayed);
  Serial.print(", outbox ");
  Serial.print(outbox.pendingBytes());
  Serial.print("/");
  Serial.print(outbox.maxBytes());
  Serial.print(" B, outbox full ");
  Serial.println(stats_.outboxRejected);
  Serial.print("[NET] small sent ");
  Serial.print(stats_.smallPublished);
  Serial.print(", small dropped ");
  Serial.print(stats_.smallDropped);
  Serial.print(", status dropped ");
  Serial.print(stats_.statusDropped);
  Serial.print(", inbox dropped ");
  Serial.print(stats_.inboxDropped);
  Serial.print(", MQTT connects ");
  Serial.print(stats_.mqttConnects);
  Serial.print("/");
  Serial.print(stats_.mqttAttempts);
  Serial.print(", drops ");
  Serial.print(stats_.mqttDrops);
  Serial.print(", WiFi attempts ");
  Serial.println(stats_.wifiAttempts);
#else
  Serial.print("[NET] WiFi attempts ");
  Serial.println(stats_.wifiAttempts);
#endif
}

}  // namespace Comms

#endif  // ENABLE_WIFI
