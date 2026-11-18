# Data path review, 5 October 2026

This review covers the data paths from the ESP32 to Serial, the Pi
(MQTT → SQLite → dashboard) and InfluxDB Cloud, and how they interact with
sensing, control and the pumps. It started from the autonomous run of 30 Sep
– 5 Oct ([`autonomous_run_2026-09-30/`](autonomous_run_2026-09-30/README.md)).
That run used firmware **1.6.0**. The architecture that came out of it
(firmware 1.8.0, dashboard ingest service) is described in the main
[build guide](../BUILD_GUIDE.md#data-paths-and-firmware-architecture-180).

**Status:** 1.8.0 compiles (`arduino-cli`, esp32 core 3.3.12) and the
dashboard tests pass (65 + 6 new). It has **not been flashed or deployed**:
the board was mid-run. Check list for the first run on hardware is at the end.

## What the run data showed

| Observation | Evidence | Cause |
| --- | --- | --- |
| The Pi stored each reading 2 to 14 times, the count growing by day | `mqtt_messages`: 437 158 rows for 40 117 readings. 14 copies of one payload, all written within the same second. 16 MQTT clients on the broker. The Streamlit process at 3.1 GB (80 % of RAM). | Every browser session made its own MQTT subscriber, and each one wrote every message to SQLite and CSV. Sessions were never cleaned up. |
| Each reading also arrived a second time as a replay | 40 053 `replay` rows matched 40 117 live rows | 1.6.0 replayed the whole backup CSV (fixed in 1.6.2, never flashed) |
| The device kept no history | `DUMP SENSORS` returned 1 167 bytes after 5 days | In 1.6.0 the backup CSV was the replay buffer and was emptied on every replay |
| Replay rows could not be matched to live rows | Replay JSON had no `uptime_ms` and no reading id | No reading identity anywhere |
| Influx held 950 of about 40 000 readings | Gaps of hours; device log `connection refused ssl`, queue 8/8 | 1.6.0 handshake-timeout bug (fixed in 1.6.2), plus a RAM queue that drops when full |
| Three nutrient doses booked about 7.6 mL instead of 0.3–0.5 mL | `nutrient_rem_ml` steps in `actuator_events.csv` | Pump timers not serviced while the loop was blocked (fixed in 1.6.2 for sensor reads; 1.8.0 removes the other blocking paths below) |

## Fixed in firmware 1.8.0

| # | Severity | Finding | Change |
| --- | --- | --- | --- |
| 1 | High | **WiFi rejoin blocked the control loop.** `maintainNetwork()` called `connectWifi()` from `loop()` every 5 s while WiFi was down: a blocking scan, a 20 s (PSK) or 45 s (enterprise) join, then up to 10 s of NTP. Pump timers are serviced only in `loop()`, so a dose started before a WiFi drop could overrun. | WiFi, NTP and MQTT moved to a network task on core 0 (`comms.cpp`). The loop never calls WiFi or socket APIs. |
| 2 | High | **MQTT publish and keepalive ran on the loop.** `PubSubClient::publish` writes to the TCP socket and can block up to the socket timeout (5 s) on a stalled link, mid sensor cycle. | The loop copies the JSON into one of two slots; the network task publishes. |
| 3 | High | **Serial `DUMP` blocked the loop for about 2 min** (1.4 MB at 115 200 baud), with pumps unserviced. | Non-blocking: a few lines per pass while the TX buffer has room. Lines are framed with a number and CRC16, and END carries a count and CRC32. `scripts/serial_dump.py` verifies and resumes with `FROM n`. `RAW` keeps the old unframed format. |
| 4 | High | **MQTT never reconnected after a drop** (by design since 1.4). After a broker restart the Pi got nothing until someone typed `MQTT CONNECT`. | Backoff reconnect on the network task, 15 s doubling to 10 min. `MQTT CONNECT` still forces a try. |
| 5 | High | **No reading identity.** A replayed row could not be told from a live one, so the Pi could not de-duplicate. | Persistent `boot_id` (`/boot_id.txt`) plus per-boot `seq` in the journal, live MQTT, replay, Influx and the Pi. Control events carry `event_seq`. |
| 6 | Medium | **Replay rewrote the unsent file on the loop every 30 s** (up to 200 publishes plus a full file copy). | Append-only outbox with a persisted cursor (`shared/durable_queue.h`), replayed by the network task 5 rows every 250 ms, paused while a pump draws. |
| 7 | Medium | **Influx dropped data whenever it was down for more than about 80 s** (8-line RAM queue). | Overflow goes to `/influx_outbox.lp` (192 KB) and is drained when the RAM queue is empty. Resends are harmless because every point has its own timestamp. |
| 8 | Medium | **Influx points before NTP were stamped with uptime** (they land in 1970). | Not queued until NTP is valid. The journal and MQTT keep the reading. |
| 9 | Medium | **NTP was tried only inside the WiFi join.** One failed sync meant no wall clock until the next WiFi drop, and the millis→epoch mapping was never refreshed (drift of a few seconds a day). | The network task latches SNTP time when it arrives and every 6 h. Readers use `ntpSnapshot()` so a re-latch cannot be read half-updated. |
| 10 | Medium | **`millis()` wrap at 49.7 days** broke every timestamp (signed 64-bit subtraction of a wrapped value). | Unsigned difference cast to `int32` in both converters. |
| 11 | Medium | **Replay attached live state to old rows** (pH calibration session, hydroponic mode, pump calibration) and, once off the loop, would have read the loop's globals from another task. | Replay sends only what the row recorded. |
| 12 | Medium | **A replayed row from an earlier boot** without `recorded_at_utc` was stamped using the current boot's NTP mapping, which is wrong. | Mapped only when the row's `boot_id` is the current one; otherwise it has no timestamp (the Pi uses `received_at`). |
| 13 | Medium | **The journal was deleted** on a column-layout change and when it reached its cap. | Archived to `/sensor_readings_prev.csv` (`DUMP PREV`). The archive is removed first when flash is low. |
| 14 | Low | **Dose-complete rows** (copies of the last reading, written when a dose ends) went to the Pi only while MQTT was down. | Journal only. The dose reaches the Pi as a control event. |
| 15 | Low | **Control events never reached the Pi** (only Influx and the device log). | Published on `smart-garden/<id>/events`. The Pi stores them in `mqtt_messages`, keyed on `event_seq`. |
| 16 | Low | **No Serial TX buffer.** The ~3 KB of log per sensor cycle blocked the loop for about 0.3 s per cycle in `Serial.print`. | `Serial.setTxBufferSize(4096)` before `begin()`. |
| 17 | Low | `SENSOR_READ_MAX_DEFER_MS` was defined only with `ENABLE_MQTT`, so an Influx-only build did not compile. | Moved out of the guard. |
| 18 | Low | Dashboard commands ran inside the MQTT callback (in `mqttClient.loop()`). | The callback copies them into an inbox; the loop runs one per pass, like Serial commands. |

## Fixed on the Pi (dashboard repo)

| # | Severity | Finding | Change |
| --- | --- | --- | --- |
| P1 | High | One MQTT subscriber and one DB writer per browser session (finding above). It also kept growing memory use. | One shared subscriber per process (`shared_mqtt_client`); each session reads by cursor. The dashboard does not write when `SMART_GARDEN_DASHBOARD_PERSIST=0` (set in its unit). |
| P2 | High | `scripts/mqtt_to_sqlite.py` existed but no service ran it, so storage depended on a dashboard tab being open. | New `smart-garden-ingest` systemd unit, installed by `deploy-pi.sh`: the only DB writer. It reconnects with backoff. |
| P3 | High | Inserts were not idempotent. | `reading_key` / `msg_key` columns with unique partial indexes. `INSERT OR IGNORE`, so a repeat is counted and skipped, and the CSV mirror only gets new rows. |
| P4 | Medium | The existing database holds 437 k rows, mostly copies. | `scripts/dedupe_sqlite.py`: dry run by default, writes a backup before `--apply`. Not run yet. |

## Still open

| # | Severity | Finding | Suggestion |
| --- | --- | --- | --- |
| O1 | **High (control)** | **The pH and EC session caps never reset except on a stage change.** `PhControl::resetSession` / `EcControl::resetSession` are called only from the stage-change path. The pH cap (`phSessionCapMl` 2 mL) is reached by ordinary 0.05–0.15 mL corrections after 2–3 days, and then pH control stops for good: in the run, every `ph_check` from 4 Oct 02:52 UTC on says `ph_session_cap`, and pH drifted to 6.8. The EC cap (15 mL, hard-coded) will do the same. | Make the cap a rolling window (for example mL per 24 h), or reset the session when the value returns to the deadband and stays there for one check interval. This changes dosing behaviour, so it is left for you to decide. |
| O2 | Medium | MQTT is QoS 0, so a reading published in the last seconds before a silent link drop can be lost from the live path. The journal has it. | Visible on the Pi as a `seq` gap in one `boot_id`. If it matters, add an MQTT `resend` command (by seq range) that appends journal rows to the outbox. |
| O3 | Medium | Free heap. Static RAM grew by 19 KB (79 → 98 KB) for the two 6 KB telemetry slots, the replay buffers and the Influx outbox buffer. In 1.6.0 the board ran with about 125–133 KB free and still had TLS failures (`Generic error heap 133280`). TLS needs about 40–45 KB of contiguous heap. | After flashing, watch `free_heap_b` and Influx `last_http`. If TLS fails, cut to one telemetry slot or add `heap_caps_get_largest_free_block` to telemetry. |
| O4 | Low | `/control_events.csv` has no size cap (about 3 KB/day). | Rotate it like the journal when it passes, say, 128 KB. |
| O5 | Low | Dashboard commands that arrive while MQTT is down are lost; the dashboard shows `Sent` once the broker accepts them. | Fine for a LAN dashboard. The acks on `/ack` could be extended to every command. |
| O6 | Low | The Serial log is still interleaved by two tasks (`[NET]`, `[INFLUX]` lines) with no prefix discipline. | Cosmetic. `DUMP` framing already copes. |

## First run on hardware

1. `DUMP SENSORS` / `DUMP CTRL` with the old firmware first (`scripts/serial_dump.py` needs 1.8.0 framing; use the raw dump from this session for 1.6.0).
2. Flash 1.8.0. Expect `[DEBUG] boot_id 1`, `[NET] network task started`, `[NET] NTP time latched`, `[NET] MQTT reading seq=1 published`.
3. `NET` and `INFLUX` after a few minutes: live published rising, outbox 0 B, Influx `uploaded` rising, `free_heap_b` above ~90 KB.
4. Stop Mosquitto on the Pi for 2 min, then start it. Expect `MQTT retry in 15 s`, `30 s`, …, then `outbox replay complete`. On the Pi, check that there is no `seq` gap and no repeated `reading_key`.
5. Start a manual dose and a `DUMP SENSORS` together. The dose should end on time (`[DOSE]` line, `nutrient_rem_ml` step about 0.8 mL).
6. On the Pi: `git pull` the dashboard and run `./scripts/deploy-pi.sh`. Confirm `smart-garden-ingest active`, and `mosquitto_sub -t '$SYS/broker/clients/connected'` shows about 4 clients (ingest, dashboard, notifier, camera). Then `scripts/dedupe_sqlite.py` (dry run), and `--apply` with the services stopped.
