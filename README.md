# chipguy_MQTT_netclient

**An ESP32 networked-device client that gets your sketch on the network, keeps
it there, maintains an MQTT connection, and listens for OTA updates — all on
its own background task, without ever blocking your `setup()` or `loop()`.**

You write your sketch the way you always have. NetClient handles WiFi (or
Ethernet) bring-up, reconnect storms, broker dropouts, retained-message
maintenance, OTA upload windows, and watchdog feeding from a separate task on
core 0. Your application code just hands values to `publish()` / `persist()`
from any task, drains a small queue of events when something interesting
happens, and gets on with whatever sensors or UI you're building. There is no
"wait for connection" code to write.

## Highlights

- **Set-and-forget connectivity.** A single background task owns WiFi or
  Ethernet, MQTT, OTA, and the watchdog. Your `loop()` never blocks waiting
  on any of them.

- **Thread-safe by design.** Any FreeRTOS task can publish, subscribe,
  or persist. Sensor pollers and display tasks coexist with the
  network task without coordination — internal queues and mutexes handle it.

- **Event-driven, not state-polled.** NetClient exposes a tiny event queue:
  `MAC_ADDRESS_AVAILABLE`, `NETWORK_CONNECTED`, `MQTT_SERVER_CONNECTED`,
  `MQTT_MESSAGE_RECEIVED`, etc. Drain it at your own pace. The library never
  pulls control of `loop()` away from you.

- **LVGL-friendly.** Because the network and MQTT work runs on its own
  task, an LVGL interface in your `loop()` stays smooth — reconnects, TLS
  handshakes, and bursts of incoming messages don't stutter the UI. Several
  ready-to-flash LVGL-touchscreen examples are included and this library
  has been used successfully as the network layer behind LVGL UIs on
  multiple boards.

- **The `persist()` concept.** Hand a topic and a payload to `persist()` and
  NetClient guarantees, on a best-effort basis, that the broker is told —
  *and kept told* — about that value. Re-publishes on a timer to defeat
  retained-message expiration. Survives broker dropouts and reconnects
  without any application-level retry code. You can call `persist()`
  *before* the broker is connected — even right out of `setup()` — and
  NetClient will hold the latest value and publish it as soon as the
  connection comes up. If your application updates the same topic many
  times during a disconnect, only the most recent payload is sent on
  reconnect; intermediate values are simply overwritten in place.
  `persist(topic, nullptr)` discontinues pending efforts to publish the
  topic.

- **Smoothing and debouncing built in.** Three companion helpers wrap
  `persist()` for tricky data:
  - `persistWithSmoothing(value, decimals, threshold, topic)` — for numeric
    sensors. Small jitter is absorbed; only changes past the significance
    threshold publish promptly.
  - `persistWithDebounce(value, topic)` — for string values that change
    burstily. Caps publish rate per topic, debounces rapid changes,
    guarantees a final publish once the value settles.
  - `persistWithTimestamps(value, topic)` — for binary state values. Same
    debouncing as above, plus retained `_active_at` / `_inactive_at`
    companion topics carrying the Unix timestamp of each transition.

- **Auto-subscribe that survives reconnects.** `autoSubscribe()` remembers
  the topic and re-subscribes on every reconnection. No application-level
  retry logic needed.

- **Topic substitution.** `"%P/temperature"` expands to the configured
  topic prefix at publish time. `"%X"` / `"%x"` substitute the MAC address
  (with optional 4/6/12 octet counts). One format string per topic, never
  hand-built MAC strings again.

- **WiFi or Ethernet — same API.** Swap a single `#include` and the same
  sketch runs over PoE. Six board-specific Ethernet headers ship pre-wired
  for common hardware (M5Stack PoESP32, AtomPoE, M5CoreLAN, StamPLCPoE,
  Hankerila HKL-EA8, Waveshare ESP32-P4-86-Panel-ETH).

- **Deferred broker configuration.** Start with a null broker hostname,
  learn it later (mDNS, split-horizon DNS based on MAC, an out-of-band
  command — whatever you like), then call `updateConfiguration()`. NetClient
  picks up the new server on its next iteration and connects.

- **MAC-aware out of the box.** MAC is lazily read from eFuse and updated
  once the network interface has its authoritative MAC. Available as a
  string, short hex, `uint32_t`, or `uint64_t`. The `MAC_ADDRESS_AVAILABLE`
  event gives you the right hook for any MAC-dependent decisions.

- **Hardware watchdog tied to broker traffic.** If MQTT traffic stops for
  the configured timeout, the device reboots — your stuck-network detector
  is built in.

- **Last Will & Testament.** Configure `lastWillTopic` / `lastWillMessage`
  / `onlineMessage` once. NetClient publishes "online" on connect and the
  broker publishes "offline" on disconnect, automatically.

- **TLS by default.** TLS via `WiFiClientSecure` with optional CA
  certificate. No `caCert` configured falls back to `setInsecure()` so you
  can develop against self-signed brokers without ceremony.

- **ArduinoOTA wired in.** Set `otaPassword` and you're done. Hostname
  defaults to `esp32-<MAC>`; override with `otaHostname`. OTA can land on
  a device that has otherwise lost MQTT contact.

- **Per-topic expiration.** `setExpirationSeconds(topic, seconds)` arms a
  TTL. Stale values stop being republished after the TTL elapses — the
  "sensor unplugged" semantic. Each fresh `persist()` call refreshes the
  clock.

- **Status-color hint.** A small `StatusColor` enum (RED / YELLOW / GREEN /
  CYAN) drops straight into a status-LED routine — no manual derivation
  from connection booleans.

## Installing

Clone or download into your Arduino libraries directory.

```
~/Documents/Arduino/libraries/chipguy_MQTT_netclient/
```

The library is ESP32-only and tested on Arduino-ESP32 v3.x.

## Hello world

```cpp
#include <chipguy_MQTT_netclient_WiFi.h>     // or a board-specific Ethernet header

void setup() {
    Serial.begin(115200);

    NetClientConfig config;
    config.wifiSSID     = "MyWiFi";
    config.wifiPassword = "secret";

    config.topicPrefix      = "devices/%X";  // %X = full MAC in hex
    config.mqtt.server      = "mqtt.example.com";
    config.mqtt.port        = 8883;
    config.mqtt.username    = "user";
    config.mqtt.password    = "password";
    config.mqtt.clientId    = "esp32-%X";
    config.mqtt.lastWillTopic = "%P/status";  // %P = topicPrefix above

    NetClient.begin(config);
}

void loop() {
    // Drain events as they happen — never blocks.
    while (NetClient.eventAvailable()) {
        NetClientEvent ev = NetClient.getEvent();
        switch (ev.type) {
            case EventType::MQTT_SERVER_CONNECTED:
                NetClient.autoSubscribe("%P/command");
                break;
            case EventType::MQTT_MESSAGE_RECEIVED: {
                const auto& m = ev.mqtt_message_received_event;
                Serial.printf("Got %s = %s\n", m.topic, m.payload);
                break;
            }
            default: break;
        }
        // Any strings the event points to (topic, payload, IP, MAC) are
        // valid only until the next eventAvailable() call, which releases
        // the previous event's memory. Copy anything you need to keep.
    }

    // Publish whatever you like, as often as you like — persist() dedupes
    // identical payloads, so calling it 50 times a second is fine. The
    // broker only sees a new message when the value actually changes
    // (here, once per second as the uptime tick rolls over).
    NetClient.persistf("%P/uptime", "%lu", millis() / 1000UL);

    delay(20);
}
```

## Concepts

### publish vs. persist

`publish()` queues a one-shot publish. If the broker is offline at that
moment the message is dropped — same semantics as PubSubClient.

`persist()` registers a topic+payload pair that NetClient will keep mirrored
at the broker. It re-publishes periodically (default every 50 s) so retained
messages don't age out, and re-publishes on every reconnect. Pass `nullptr`
as the payload to discontinue pending efforts to publish the topic.

Calls to `persist()` are valid before the broker is connected — including
right out of `setup()` — and during disconnects. NetClient holds the most
recent payload per topic and publishes it the moment the connection comes
up. If your application updates the same topic many times before the
broker is reachable (say, 200 sensor readings during a 30-second reconnect
window), only the final payload is actually sent; the intermediate values
are overwritten in place rather than queued.

This is the right primitive for "tell the world my current state" values:
firmware version, IP address, last-known-good sensor reading, configuration
fingerprint. You set it once and stop thinking about it.

### Data points

For values that change frequently, the bundled data-point classes apply
smoothing and rate-limiting before they hit `persist()`. The three
`persistWith*` helpers create the right kind of data point on the first call
and reuse it after that, so you can keep your application code as compact as
`persist()` while still getting the smarter publishing behavior.

```cpp
NetClient.persistWithSmoothing(thermistorC, 1, 0.5f, "%P/temp_c");
NetClient.persistWithDebounce(selectedMode,           "%P/selected_mode");
NetClient.persistWithTimestamps(doorIsOpen,           "%P/door");
```

### Event loop

NetClient never calls user code from its own task. Everything you need to
react to — connect, disconnect, message arrival, MAC learned, status color
change — arrives as an event in your `loop()`. Drain at your own pace.
Drop events you don't care about; the library doesn't care either.

## Network modes

Default include is WiFi. To run the *exact same sketch* on Ethernet,
replace the `#include` at the top:

```cpp
#include <chipguy_MQTT_netclient_WiFi.h>          // WiFi (default)
#include <chipguy_MQTT_netclient_PoESP32.h>       // M5Stack PoESP32      (IP101 RMII)
#include <chipguy_MQTT_netclient_AtomPoE.h>       // M5Stack AtomPoE      (W5500 SPI)
#include <chipguy_MQTT_netclient_M5CoreLAN.h>     // M5Stack Core + LAN   (W5500 SPI)
#include <chipguy_MQTT_netclient_StamPLCPoE.h>    // M5Stack StamPLC PoE  (W5500 SPI)
#include <chipguy_MQTT_netclient_HKL-EA8.h>       // Hankerila HKL-EA8    (LAN8720 RMII)
#include <chipguy_MQTT_netclient_P486Panel.h>     // Waveshare P4-86      (IP101 RMII)
```

The board header provides a `configureEthernet(config)` helper your sketch
can always call, even if Ethernet is not used. With the WiFi include it's a no-op; with
an Ethernet include it fills in the pinout for that adapter.

## Examples

See the `examples/` directory:

- **BasicUsage** — minimum viable use of the library.
- **BasicUsage_M5Stack_with_RGB_LED** — drives a status LED from the
  `STATUS_COLOR_CHANGE` event on M5Stack RGB-LED-equipped boards.
- **BasicUsage_M5Stack_M5GFX_LVGL** — LVGL UI driven through unified M5GFX
  library, compatible with most M5Stack ESP32 products that have a display.
- **BasicUsage_Waveshare_C6LCD147_LVGL** — Waveshare ESP32-C6-LCD-1.47 with
  a minimal ESP-IDF SPI LCD driver and LVGL.

## License

This library is released under the MIT License — see [`LICENSE`](LICENSE).

### Third-party code

- **PubSubClient** by Nicholas O'Leary (MIT) — forked into
  `src/chipguy_PubSubClient_fork.{h,cpp}`. Original license at
  [`LICENSES/PubSubClient.txt`](LICENSES/PubSubClient.txt); modifications
  documented in [`CHANGES.md`](CHANGES.md). Upstream:
  <https://github.com/knolleary/pubsubclient>.
- **SquareLine Studio-generated UI** in the LVGL examples
  (`examples/*/src/ui*.{c,h}`). Generated by SquareLine Studio per the
  export-time license terms; original headers preserved in those files.
- **LVGL configuration** (`examples/*/lv_conf.h`) — derived from the
  LVGL project template (MIT). Upstream: <https://github.com/lvgl/lvgl>.
