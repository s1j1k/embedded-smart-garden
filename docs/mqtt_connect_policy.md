# MQTT connect policy

## Current behaviour (firmware 1.8.0+)

WiFi, NTP and MQTT run on the network task (core 0, `comms.cpp`), not on the
control loop, so a slow join, DNS lookup or broker connect cannot hold a pump
on or delay a sensor read.

| Event | Behaviour |
| --- | --- |
| Boot | `setup()` starts the network task and returns; the task joins WiFi, then connects MQTT. |
| Broker down / DNS fail | Retry after 15 s, doubling to at most 1 min (`MQTT_RECONNECT_MIN_MS` / `MQTT_RECONNECT_MAX_MS`; 10 min before 1.9.25). Each attempt tries the configured broker, then `MQTT_BROKER_MDNS_HOST`. The backoff restarts when WiFi rejoins. |
| Mid-run drop | Detected on the next 20 ms pass; same backoff. |
| WiFi lost | Rejoined every 30 s (`WIFI_RETRY_INTERVAL_MS`) on the network task; MQTT follows. |
| Serial `MQTT CONNECT` (aliases `MQTT RECONNECT`, `MQTT RETRY`) | Next pass tries at once, ignoring the backoff. |
| Serial `NET` / `MQTT` | WiFi, NTP, MQTT state, next retry, publish / replay / outbox counters. |
| Pump `CAL` session | MQTT disconnected for the session; reconnects when it finishes or aborts. |
| Readings while offline | Journal always; the MQTT outbox keeps the missed rows (256 KB) and replays them after reconnect. |

The original reason for "no retries" (below) was that reconnect attempts ran
on the loop and competed with Influx and Serial CAL. With the connect on its
own task, a long backoff, and replay paused while a pump draws, that no longer
applies.

## History: firmware 1.4 – 1.7

Assumption: when the garden node powers on, the MQTT broker (Raspberry Pi
Mosquitto / local dashboard) should already be available on the LAN.

| Event | Behaviour |
| --- | --- |
| Boot (`initNetwork`) | One connect attempt after WiFi succeeds. |
| Broker down / DNS fail at boot | Log the error and **stop**. No timed retries. |
| Mid-run drop (broker dies, WiFi blip after MQTT was up) | Stay disconnected. No background reconnect loop. |
| WiFi reconnects later | WiFi may auto-reconnect; **MQTT does not** follow automatically. |
| Serial `MQTT` / `MQTT STATUS` | Print broker host, WiFi, and client state. |
| Serial `MQTT CONNECT` (aliases: `MQTT RECONNECT`, `MQTT RETRY`) | One explicit connect attempt. |
| Pump `CAL` session | Disconnects MQTT for the session; on finish, one explicit reconnect if MQTT was up before CAL. |
| Telemetry / CSV flush while offline | Skipped quietly after a one-time Serial warning. Influx and local CSV continue independently. |

WiFi still uses a paced reconnect interval (`MQTT_RECONNECT_INTERVAL_MS` name is
historical — it paces **WiFi** only).

### Why

Repeated `PubSubClient.connect()` attempts against an unreachable broker
(state `-2`) were suspected of stressing the shared WiFi TCP stack and
contributing to stalled Influx HTTPS uploads (`HTTPClient` return `-1`) while
WiFi still reported connected. Serial CAL also suffered when reconnect spam
fought the operator session.

### Operator checklist

1. Bring the broker up first (Pi Mosquitto or local dashboard port-forward).
2. Power / reset the ESP32 (or flash).
3. If Serial shows MQTT failed at boot: fix the broker, then type **`MQTT CONNECT`**.
4. Garden control (pH / EC / level) does **not** require MQTT — Influx + Serial remain primary for unattended grows when the Pi is offline.

## Possible improvements

- **Bluetooth (or BLE Serial) MQTT arming** — pair a phone / laptop and send
  broker host + “connect now” without USB Serial; keeps boot quiet by default.
- **Optional exponential backoff mode** — compile-time or Serial toggle for
  unattended sites that *do* want slow retries (e.g. every 5–15 min), still
  avoiding the old 5 s hammer.
- **WiFi-up hook with single deferred attempt** — if WiFi was down at boot and
  later joins, optionally allow **one** automatic MQTT try (still no loop).
- **Dashboard retained “MQTT wanted” flag** — cannot use MQTT to request
  reconnect when already offline; Bluetooth / Serial / physical button remain
  the out-of-band channels.
- **Separate TCP clients** — ensure Influx HTTPS and MQTT never share one
  `WiFiClient` instance (already separate today); revisit if TLS + MQTT both
  block the loop.
- **Health LED / grow-light blink code** — non-Serial cue that MQTT is offline
  so a missing Pi is obvious without opening the monitor.
