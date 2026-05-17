// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef CHIPGUY_MQTT_NETCLIENT_H
#define CHIPGUY_MQTT_NETCLIENT_H

#include <Arduino.h>
#include "NetClientConfig.h"
#include "NetClientEvent.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <ETH.h>
#include "chipguy_PubSubClient_fork.h"
#include "EthernetConfig.h"
#include <vector>
#include <string>
#include <deque>
#include <functional>
#include <cstdarg>


// Forward declarations
class ChipguyNetClient;
class MqttNumericDataPoint;
class MqttStringDataPoint;
class MqttDataPointWatcher;

// Subscription entry for tracking
struct SubscriptionEntry {
    std::string topic;
    uint8_t qos;
};

// Persist entry for periodic republishing
struct PersistEntry {
    std::string topic;
    std::vector<uint8_t> payload;
    unsigned long lastPublishTime = 0;   // millis() when last sent
    // Expiration: 0 = no expiration set (default), entry is republished forever.
    // Otherwise, expirationDeadlineMillis is the millis() deadline after which
    // periodic republishing of this entry stops. Each fresh persist() call
    // for the topic refreshes the deadline (now + expirationSeconds * 1000).
    unsigned long expirationSeconds = 0;
    unsigned long expirationDeadlineMillis = 0;
};

// UDP queue entry for fire-and-forget datagrams
struct UdpQueueEntry {
    IPAddress ip;
    uint16_t port;
    std::vector<uint8_t> payload;
    unsigned long expiresAt;  // millis() deadline
};

// Command types for the command queue (main thread -> network thread)
enum class CommandType : uint8_t {
    PUBLISH,
    SUBSCRIBE,
    UNSUBSCRIBE
};

// Command entry for async network operations
struct NetClientCommand {
    CommandType type;
    std::string topic;
    std::vector<uint8_t> payload;  // For publish
    size_t payloadLength;
    bool retained;                  // For publish
    uint8_t qos;                    // For subscribe
};

class ChipguyNetClient {
public:
    ChipguyNetClient();
    ~ChipguyNetClient();

    // Initialize and start the network client.
    // Creates the network thread which manages WiFi and MQTT connections.
    // Returns true on successful initialization.
    bool begin(const NetClientConfig& config);

    // Stop the network client and clean up resources.
    void end();

    // Check if an event is available. Call this in your loop().
    bool eventAvailable();

    // Get the next event. Call after available() returns true.
    // The returned event data (topic, payload, IP strings) remains valid
    // until the next call to available().
    NetClientEvent getEvent();

    // Publish to the MQTT broker.
    bool publish(const char* topic, const char* payload, bool retained = true);
    bool publish(const char* topic, const uint8_t* payload, size_t length, bool retained = true);

    // Printf-style publish (formats payload, retained=true).
    bool publishf(const char* topic, const char* format, ...);

    // Subscribe one-time (not remembered for reconnect).
    bool subscribe(const char* topic, uint8_t qos = 0);

    // Auto-subscribe: subscribe now and remember for automatic resubscribe on reconnect.
    // Silently ignores duplicate subscriptions to the same topic.
    bool autoSubscribe(const char* topic, uint8_t qos = 0);

    // Unsubscribe (also removes from auto-subscribe list).
    bool unsubscribe(const char* topic);

    // Persist messages - stored and republished periodically while connected.
    // Publishes immediately if topic is new or payload changed.
    // Survives disconnect-reconnect cycles.
    bool persist(const char* topic, const char* payload);
    bool persist(const char* topic, const uint8_t* payload, size_t length);

    // Printf-style persist (formats payload).
    bool persistf(const char* topic, const char* format, ...);

    // Persist a numeric reading through an MqttNumericDataPoint. The first call
    // for a given topic constructs the data point with the given decimalPlaces
    // and significanceThreshold; subsequent calls reuse it (decimalPlaces and
    // significanceThreshold are ignored after creation). The value is pushed
    // via updateNonExpiringValue(), so it lives until the next call.
    //
    // Topic format mirrors persist(): a leading "%P" is preserved and the
    // configured topic prefix is substituted at publish time. The data point
    // handles significance-based reporting and periodic re-publish; the
    // network task ticks it automatically, so the caller can invoke this as
    // fast as it likes.
    //
    // Thread-safe: callable from any task once begin() has returned.
    bool persistWithSmoothing(float value, int decimalPlaces, float significanceThreshold,
                              const char* topicFormat, ...)
        __attribute__((format(printf, 5, 6)));

    // Persist a string reading through an MqttStringDataPoint, with a
    // MqttDataPointWatcher attached so transitions of the value also publish
    // "_active_at" / "_inactive_at" Unix-timestamp companion topics. Designed
    // for binary-state values ("0" / "1"), though any short string is accepted
    // — the timestamp topics only fire on a "0" <-> "1" transition.
    //
    // The first call for a given topic constructs both the data point and the
    // watcher; subsequent calls just push the value. Topic format follows the
    // same "%P preserved" rule as persist(). For the timestamp companions to
    // actually publish, MqttDataPointWatcher::setUnixTimeCallback() must have
    // been called with a callback that returns a real Unix time.
    //
    // Thread-safe: callable from any task once begin() has returned.
    bool persistWithTimestamps(const char* value, const char* topicFormat, ...)
        __attribute__((format(printf, 3, 4)));

    // Bool overload: publishes "1" / "0".
    bool persistWithTimestamps(bool value, const char* topicFormat, ...)
        __attribute__((format(printf, 3, 4)));

    // Persist a string reading through an MqttStringDataPoint. The data
    // point applies debouncing and rate-limiting (≤20 publishes per minute
    // per topic, rapid-change detection, a guaranteed final publish 5s
    // after the value stabilizes) so a bursty source can't flood the
    // broker.
    //
    // The first call for a given topic constructs the data point;
    // subsequent calls just push the new value. No watcher is attached —
    // use persistWithTimestamps() instead if you want the "_active_at" /
    // "_inactive_at" transition companions.
    //
    // Topic format mirrors persist(): a leading "%P" is preserved and the
    // configured topic prefix is substituted at publish time.
    //
    // Thread-safe: callable from any task once begin() has returned.
    bool persistWithDebounce(const char* value, const char* topicFormat, ...)
        __attribute__((format(printf, 3, 4)));

    // Configure a per-topic expiration. After the configured number of
    // seconds since the last value push for the topic, publishing for that
    // topic stops.
    //
    // For topics backed by a data point (persistWithSmoothing /
    // persistWithTimestamps), the next value push uses updateValue() with the
    // configured TTL, and the data point's existing expiration logic takes
    // over (sensorValid flips false on expiry, tick() then declines to
    // publish).
    //
    // For topics in the plain persist() set, the periodic republish stops
    // once the deadline passes. A fresh persist() call refreshes the
    // deadline. The deadline tracked per entry is updated immediately by
    // this call so that calling setExpirationSeconds() alone is enough to
    // start the timer; no further publish is required.
    //
    // Pass seconds = 0 to clear an expiration previously set (revert to no
    // expiration). Returns true if the topic was found, false if it wasn't
    // recognized in any of the three sets.
    //
    // Topic matching uses the same form the caller used when persisting
    // (a leading "%P" is preserved for data point topics; persist() topics
    // are matched against their MAC/prefix-substituted form).
    //
    // Thread-safe.
    bool setExpirationSeconds(const char* topic, unsigned long seconds);

    // Clear persisted messages
    void clearPersist(const char* topic);
    void clearPersists();

    // Status queries
    bool isWifiConnected() const { return _wifiConnected; }
    bool isMqttConnected() const { return _mqttConnected; }
    StatusColor getCurrentStatus() const { return _currentStatus; }
    int8_t getWiFiRSSI() const;  // WiFi signal strength in dBm

    // Network mode queries
    bool isEthernet() const { return _useEthernet; }
    bool isWiFi() const { return !_useEthernet; }
    bool isNetworkConnected() const { return _useEthernet ? _ethConnected : _wifiConnected; }
    uint16_t getEthernetLinkSpeed() const;  // Returns Mbps or 0

    // Returns the SPI host used by the Ethernet adapter, or -1 if Ethernet
    // is not configured or does not use SPI (e.g. RMII).
    int getEthernetSpiHost() const {
        return (_useEthernet && _ethernetConfig.spiCs >= 0)
            ? (int)_ethernetConfig.spiHost : -1;
    }

    // Get device identifiers (lazy-init MAC from eFuse if not yet initialized)
    const char* getMacAddress() const;
    const char* getMacAddressShort() const;
    uint32_t getMacAddress32() const;   // Lowest 32 bits of MAC
    uint64_t getMacAddress64() const;   // Full 48-bit MAC in lowest bits of uint64_t
    const char* getLocalIP() const { return _localIP; }

    // Update the topic prefix used for %P substitution in topics.
    // If the string is in flash, stores the pointer directly.
    // If the string is in RAM and duplicateRamString is true (default),
    // duplicates the string into newly allocated memory.
    // Equivalent to calling updateConfiguration() with only the TOPIC_PREFIX
    // section; kept as a convenience.
    void updateTopicPrefix(const char* newPrefix, bool duplicateRamString = true);

    // Bitfield selecting which subsections of NetClientConfig
    // updateConfiguration() applies. Combine with bitwise OR.
    struct ConfigSection {
        static constexpr uint32_t TOPIC_PREFIX = 1u << 0;
        static constexpr uint32_t BROKER       = 1u << 1;
        static constexpr uint32_t OTA          = 1u << 3;
        static constexpr uint32_t ALL =
            TOPIC_PREFIX | BROKER | OTA;
    };

    // Update selected sections of the running configuration. Intended as a
    // one-shot supply of deferred configuration after MAC-dependent values
    // (broker hostname, topic prefix, OTA hostname) have been learned at
    // runtime. Safe to call from any task after begin() has returned. Pass
    // any combination of ConfigSection flags; unspecified sections are left
    // untouched.
    //
    //   TOPIC_PREFIX: replaces the %P prefix; cached %P/MAC expansions are
    //                 refreshed.
    //   BROKER:       replaces the broker config (server, port, creds,
    //                 lastWill, etc.). MQTT is re-set-up on the network
    //                 task's next iteration; if the configured server is
    //                 nullptr/empty, MQTT enters "wait" mode (no connect
    //                 attempts, no failures) until a later
    //                 updateConfiguration() supplies a server.
    //   OTA:          replaces otaHostname / otaPassword; ArduinoOTA is
    //                 re-set-up on the network task's next iteration.
    //
    // The "wait for an MQTT server to be configured" semantics: a broker
    // whose hostname is null/empty makes no connect attempts and reports no
    // failures; the rest of the network stack (link, OTA, UDP, etc.) runs
    // normally. Calling updateConfiguration() to supply a hostname later is
    // the supported way to make the device connect to MQTT after the MAC
    // address (or any other deferred information) has been learned.
    void updateConfiguration(const NetClientConfig& config, uint32_t sections);

    // Apply NetClient's topic substitutions (%P prefix and %X / %x MAC patterns
    // with optional 4/6/12 octet count) to a topic string and return the
    // result. This is what publish() and autoSubscribe() apply internally
    // before sending; expose it so external code (e.g. MqttDataPointWatcher)
    // can match the substituted form against incoming MQTT_MESSAGE_RECEIVED
    // topics. Safe to call any time after begin(); the result depends on the
    // current MAC and topic prefix, which may change after MAC_ADDRESS_AVAILABLE
    // or after a call to updateTopicPrefix(), so callers that cache results
    // should refresh when those things change.
    std::string expandTopic(const char* topic);

    // Send a UDP datagram. Thread-safe, non-blocking, fire-and-forget.
    // Frames are queued and sent from the network task within 250ms or discarded.
    // ip=nullptr means "send to broker A's IP".
    // Discards immediately (returns false) if the network is down.
    // deduplicate=true: if an identical frame (same ip+port+payload) is already
    // queued, refreshes its expiration time instead of adding a duplicate.
    bool sendUDP(const IPAddress& ip, uint16_t port, const uint8_t* payload, size_t length, bool deduplicate = true);
    bool sendUDP(std::nullptr_t, uint16_t port, const uint8_t* payload, size_t length, bool deduplicate = true);

    // Feed the watchdog timer.
    //   force=false: the normal feed. Inhibited while a starve is active.
    //   force=true:  bypasses the starve flag. Used by paths that must keep
    //                the device alive even during an intentional starve —
    //                most importantly OTA progress callbacks, so a manual
    //                firmware push can recover a starved device.
    void feedWatchdog(bool force = false);

    // Starve / un-starve the watchdog. While starved, all non-forced
    // feedWatchdog() calls (both internal and external) are dropped, so the
    // hardware watchdog will eventually fire and reboot the device unless a
    // forced feed (e.g. OTA progress) intervenes. Setting starve=false does
    // NOT itself feed the watchdog — feeds resume on the next caller.
    void starveWatchdog(bool starve);

private:
    // Configuration (copied on begin)
    NetClientConfig _config;

    // State flags
    volatile bool _running = false;
    volatile bool _wifiConnected = false;
    volatile bool _mqttConnected = false;
    volatile StatusColor _currentStatus = StatusColor::CYAN;
    volatile bool _macEventRaised = false;                // Guards MAC_ADDRESS_AVAILABLE event
    volatile bool _networkConnectedEventRaised = false;  // Guards CONNECTION_LOST event
    volatile bool _mqttConnectedEventRaised = false;     // Guards MQTT CONNECTION_LOST

    // Per-section "setup needed" flags. The network task watches these
    // and (re-)runs the corresponding setup step when false. They are
    // cleared by updateConfiguration() when a section's config changes,
    // so the task picks up the new values on its next iteration.
    volatile bool _macSubstitutionsDone = false;
    volatile bool _otaSetupDone = false;
    volatile bool _mqttSetupDone = false;

    // Serializes runtime configuration changes (updateConfiguration,
    // updateTopicPrefix) against the network task's reads of the config
    // strings.
    SemaphoreHandle_t _configMutex = nullptr;

    // Ethernet mode
    bool _useEthernet = false;
    volatile bool _ethConnected = false;
    EthernetConfig _ethernetConfig;  // Copy of config

    // Device identifiers (mutable for lazy initialization from const methods)
    mutable uint8_t _macBytes[6];      // Raw MAC bytes
    mutable char _macAddress[18];      // "XX:XX:XX:XX:XX:XX"
    mutable char _macAddressShort[13]; // "XXXXXXXXXXXX"
    char _localIP[16];                 // "xxx.xxx.xxx.xxx"

    // Buffers for MAC-substituted strings
    char _clientId[128];
    char _lastWillTopic[128];
    char _otaHostname[64];

    // Copied config strings (either point to flash or dynamically allocated)
    const char* _wifiSSID = nullptr;
    const char* _wifiPassword = nullptr;
    const char* _wifiUsername = nullptr;
    const char* _otaPassword = nullptr;
    const char* _mqttServer = nullptr;
    const char* _mqttUsername = nullptr;
    const char* _mqttPassword = nullptr;
    const char* _caCert = nullptr;
    const char* _lastWillMessage = nullptr;
    const char* _topicPrefix = nullptr;

    // Network clients
    WiFiClientSecure _wifiClient;
    PubSubClient _mqttClient;

    // UDP send queue
    WiFiUDP _udp;
    std::deque<UdpQueueEntry> _udpQueue;
    SemaphoreHandle_t _udpMutex = nullptr;

    // Event queue
    QueueHandle_t _eventQueue = nullptr;
    void* _lastAllocatedData = nullptr;  // Memory to free when next event is retrieved
    NetClientEvent _pendingEvent;

    // Subscription tracking
    std::vector<SubscriptionEntry> _subscriptions;
    SemaphoreHandle_t _subscriptionMutex = nullptr;

    // Persist tracking
    std::vector<PersistEntry> _persists;
    SemaphoreHandle_t _persistMutex = nullptr;

    // persistWithSmoothing tracking: a numeric data point and its configured
    // expirationSeconds (0 = non-expiring), keyed by the data point's
    // (unexpanded) topic.
    struct SmoothingEntry {
        MqttNumericDataPoint* dataPoint;
        unsigned long expirationSeconds;  // 0 = no expiration (default)
    };
    std::vector<SmoothingEntry> _smoothingPoints;
    SemaphoreHandle_t _smoothingPointsMutex = nullptr;

    // persistWithTimestamps tracking: a string data point paired with its
    // watcher, plus configured expirationSeconds, keyed by the data point's
    // (unexpanded) topic.
    struct TimestampEntry {
        MqttStringDataPoint* dataPoint;
        MqttDataPointWatcher* watcher;
        unsigned long expirationSeconds;  // 0 = no expiration (default)
    };
    std::vector<TimestampEntry> _timestampEntries;
    SemaphoreHandle_t _timestampEntriesMutex = nullptr;

    // persistWithDebounce tracking: a string data point (no watcher) with
    // configured expirationSeconds, keyed by the data point's (unexpanded)
    // topic.
    struct DebounceEntry {
        MqttStringDataPoint* dataPoint;
        unsigned long expirationSeconds;  // 0 = no expiration (default)
    };
    std::vector<DebounceEntry> _debounceEntries;
    SemaphoreHandle_t _debounceEntriesMutex = nullptr;

    // Command queues (main thread -> network threads)
    // Using deque instead of queue to allow topic deduplication
    std::deque<NetClientCommand> _commandQueue;
    SemaphoreHandle_t _commandMutex = nullptr;

    // Task handles
    TaskHandle_t _networkTaskHandle = nullptr;

    // Watchdog
    volatile bool _watchdogFeedRequested = false;       // Set by external callers
    volatile bool _watchdogForceFeedRequested = false;  // Force-feed (bypasses starve)
    volatile bool _starveWatchdog = false;              // Inhibits non-forced feeds

    // Timing
    unsigned long _lastMqttConnectAttempt = 0;
    unsigned long _lastWifiConnectAttempt = 0;

    // OTA enabled flag
    bool _otaEnabled = false;

    // MAC initialization flag (mutable for lazy initialization from const methods)
    mutable bool _macInitialized = false;

    // True when the broker has a non-null, non-empty configured hostname.
    // A disabled broker makes no connect attempts and emits no
    // CONNECTION_LOST events.
    bool isMqttEnabled() const;

    // Per-section configuration helpers, shared between begin() and
    // updateConfiguration(). All assume _configMutex is held by the caller
    // (or that begin() is still inside its single-threaded setup phase).
    void _applyTopicPrefix(const char* newPrefix, bool duplicateRamString);
    void _applyBrokerConfig(const MQTTBrokerConfig& cfg);
    void _applyOTAConfig(const char* hostname, const char* password);

    // Internal methods
    void ensureMacInitialized() const; // Lazy-init MAC from eFuse if needed
    void formatMacStrings() const;     // Format _macBytes into _macAddress/_macAddressShort
    void performMacSubstitutions();    // Substitute MAC into clientId, lastWillTopic, etc.
    void updateMacFrom(const uint8_t* newMac);  // Update MAC if different from current
    void setupMacAddress();
    void setupMacAddressEthernet();
    void substituteMAC(char* dest, size_t destSize, const char* src);
    std::string substituteMAC(const char* src);  // Returns substituted string
    void setupWiFi();
    bool connectWiFi();

    // Ethernet methods
    void setupEthernet();
    bool connectEthernet();
    static void onEthernetEvent(arduino_event_id_t event);
    void handleEthernetEvent(arduino_event_id_t event);
    void setupMqttClient();
    bool connectMqtt();
    void resubscribe();
    void republishPersists();
    void setupOTA();

    // Internal implementations
    bool _persistTo(const char* topic, const uint8_t* payload, size_t length);
    bool _autoSubscribeTo(const char* topic, uint8_t qos);
    bool _unsubscribeFrom(const char* topic);

    // UDP queue processing (called from network task)
    void _processUdpQueue();
    bool _enqueueUdp(const IPAddress& ip, uint16_t port, const uint8_t* payload, size_t length, bool deduplicate);

    // Command queue operations
    void _enqueuePublish(const char* topic, const uint8_t* payload, size_t length, bool retained);
    void _enqueueSubscribe(const char* topic, uint8_t qos);
    void _enqueueUnsubscribe(const char* topic);
    void _removePublish(const char* topic);
    void _processCommands();

    // Event queueing (thread-safe)
    void queueMacAvailable();
    void queueNetworkConnected(const char* ip);
    void queueNetworkConnectionLost();
    void queueMqttConnected();
    void queueMqttConnectionLost();
    void queueMqttMessage(const char* topic, const uint8_t* payload, size_t length, bool retained);
    void queueStatusColor(StatusColor color);

    // Update overall status color based on connection states
    void updateStatusColor();

    // String copy helpers (detect flash vs RAM)
    static bool isInFlash(const void* ptr);
    const char* copyConfigString(const char* src);
    void freeConfigString(const char*& ptr);
    void freeAllConfigStrings();

    // Static task entry point
    static void networkTaskEntry(void* param);

    // Task implementation
    void networkTask();

    // MQTT callback (payload is null-terminated)
    static void mqttCallback(const char* topic, const char* payload, unsigned int length);

    // Singleton reference for static callbacks
    static ChipguyNetClient* _instance;
};

// Global instance for convenience (like Serial or WiFi)
extern ChipguyNetClient NetClient;

// Optional NetClient-aware data point classes. They publish via the global
// NetClient (no PubSubClient dependency) and consume MQTT events the
// application drains from the NetClient event loop. Included here so user
// sketches can reach them as a natural part of NetClient.
#include "MqttDataPoint.h"
#include "MqttDataPointWatcher.h"

#endif // CHIPGUY_MQTT_NETCLIENT_H
