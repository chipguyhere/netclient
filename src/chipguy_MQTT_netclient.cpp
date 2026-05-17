// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#include "chipguy_MQTT_netclient.h"
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_log.h>
#include <soc/soc.h>
#include <esp_memory_utils.h>

static const char* TAG = "NetClient";

// Access to retained flag from modified PubSubClient
extern bool mqtt_receive_was_retained;

// Helper to convert PubSubClient state to human-readable string
static const char* mqttStateToString(int state) {
    switch (state) {
        case -4: return "MQTT_CONNECTION_TIMEOUT (server didn't respond within keepalive time)";
        case -3: return "MQTT_CONNECTION_LOST (network connection was broken)";
        case -2: return "MQTT_CONNECT_FAILED (network connection failed)";
        case -1: return "MQTT_DISCONNECTED (client disconnected cleanly)";
        case  0: return "MQTT_CONNECTED";
        case  1: return "MQTT_CONNECT_BAD_PROTOCOL (server doesn't support requested MQTT version)";
        case  2: return "MQTT_CONNECT_BAD_CLIENT_ID (server rejected client identifier)";
        case  3: return "MQTT_CONNECT_UNAVAILABLE (server unable to accept connection)";
        case  4: return "MQTT_CONNECT_BAD_CREDENTIALS (username/password rejected)";
        case  5: return "MQTT_CONNECT_UNAUTHORIZED (client not authorized to connect)";
        default: return "UNKNOWN";
    }
}

// Singleton instance pointer for static callbacks
ChipguyNetClient* ChipguyNetClient::_instance = nullptr;

// Global instance (like Serial or WiFi)
ChipguyNetClient NetClient;

ChipguyNetClient::ChipguyNetClient()
    : _mqttClient(_wifiClient)
{
    memset(_macAddress, 0, sizeof(_macAddress));
    memset(_macAddressShort, 0, sizeof(_macAddressShort));
    memset(_localIP, 0, sizeof(_localIP));
    memset(_clientId, 0, sizeof(_clientId));
    memset(_lastWillTopic, 0, sizeof(_lastWillTopic));
    memset(_otaHostname, 0, sizeof(_otaHostname));
}

ChipguyNetClient::~ChipguyNetClient() {
    end();
}

// Check if pointer is in flash memory (ESP32 data ROM)
bool ChipguyNetClient::isInFlash(const void* ptr) {
    if (!ptr) return true;  // Treat null as "no need to free"
    // Use ESP-IDF function that works across all ESP32 variants
    return esp_ptr_in_drom(ptr);
}

// Copy string: if in flash, just return pointer; if in RAM, strdup it
const char* ChipguyNetClient::copyConfigString(const char* src) {
    if (!src) return nullptr;
    if (isInFlash(src)) return src;
    return strdup(src);
}

// Free string if it was dynamically allocated (not in flash)
void ChipguyNetClient::freeConfigString(const char*& ptr) {
    if (ptr && !isInFlash(ptr)) {
        free((void*)ptr);
    }
    ptr = nullptr;
}

// Free all dynamically allocated config strings
void ChipguyNetClient::freeAllConfigStrings() {
    freeConfigString(_wifiSSID);
    freeConfigString(_wifiPassword);
    freeConfigString(_wifiUsername);
    freeConfigString(_otaPassword);
    freeConfigString(_mqttServer);
    freeConfigString(_mqttUsername);
    freeConfigString(_mqttPassword);
    freeConfigString(_caCert);
    freeConfigString(_lastWillMessage);
    freeConfigString(_topicPrefix);
}

bool ChipguyNetClient::begin(const NetClientConfig& config) {
    if (_running) {
        ESP_LOGW(TAG, "Already running, call end() first");
        return false;
    }

    _instance = this;

    // Copy configuration struct
    _config = config;

    // Handle Ethernet configuration
    if (config.ethernet) {
        _useEthernet = true;
        _ethernetConfig = *config.ethernet;
        _config.ethernet = &_ethernetConfig;
    } else {
        _useEthernet = false;
    }

    // Copy all config strings (handles flash vs RAM automatically).
    // WiFi strings are not currently part of updateConfiguration's scope, so
    // they're copied inline here. The broker/OTA/topicPrefix sections share
    // their copy logic with updateConfiguration via the _apply* helpers.
    _wifiSSID = copyConfigString(config.wifiSSID);
    _wifiPassword = copyConfigString(config.wifiPassword);
    _wifiUsername = copyConfigString(config.wifiUsername);

    _applyOTAConfig(config.otaHostname, config.otaPassword);
    _applyTopicPrefix(config.topicPrefix, /*duplicateRamString=*/true);
    _applyBrokerConfig(config.mqtt);

    // Create event queue
    _eventQueue = xQueueCreate(32, sizeof(NetClientEvent));
    if (!_eventQueue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        return false;
    }

    // Create subscription mutex
    _subscriptionMutex = xSemaphoreCreateMutex();
    if (!_subscriptionMutex) {
        ESP_LOGE(TAG, "Failed to create subscription mutex");
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
        return false;
    }

    // Create persist mutex
    _persistMutex = xSemaphoreCreateMutex();
    if (!_persistMutex) {
        ESP_LOGE(TAG, "Failed to create persist mutex");
        vSemaphoreDelete(_subscriptionMutex);
        _subscriptionMutex = nullptr;
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
        return false;
    }

    // Create UDP queue mutex
    _udpMutex = xSemaphoreCreateMutex();
    if (!_udpMutex) {
        ESP_LOGE(TAG, "Failed to create UDP mutex");
        vSemaphoreDelete(_persistMutex);
        _persistMutex = nullptr;
        vSemaphoreDelete(_subscriptionMutex);
        _subscriptionMutex = nullptr;
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
        return false;
    }

    // Create command queue mutex
    _commandMutex = xSemaphoreCreateMutex();
    if (!_commandMutex) {
        ESP_LOGE(TAG, "Failed to create command mutex");
        vSemaphoreDelete(_udpMutex);
        _udpMutex = nullptr;
        vSemaphoreDelete(_persistMutex);
        _persistMutex = nullptr;
        vSemaphoreDelete(_subscriptionMutex);
        _subscriptionMutex = nullptr;
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
        return false;
    }

    // Create config mutex (serializes updateConfiguration vs network task)
    _configMutex = xSemaphoreCreateMutex();
    if (!_configMutex) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        vSemaphoreDelete(_commandMutex);
        _commandMutex = nullptr;
        vSemaphoreDelete(_udpMutex);
        _udpMutex = nullptr;
        vSemaphoreDelete(_persistMutex);
        _persistMutex = nullptr;
        vSemaphoreDelete(_subscriptionMutex);
        _subscriptionMutex = nullptr;
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
        return false;
    }

    // Create persistWithSmoothing mutex
    _smoothingPointsMutex = xSemaphoreCreateMutex();
    if (!_smoothingPointsMutex) {
        ESP_LOGE(TAG, "Failed to create smoothingPoints mutex");
        vSemaphoreDelete(_commandMutex);
        _commandMutex = nullptr;
        vSemaphoreDelete(_udpMutex);
        _udpMutex = nullptr;
        vSemaphoreDelete(_persistMutex);
        _persistMutex = nullptr;
        vSemaphoreDelete(_subscriptionMutex);
        _subscriptionMutex = nullptr;
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
        return false;
    }

    // Create persistWithTimestamps mutex
    _timestampEntriesMutex = xSemaphoreCreateMutex();
    if (!_timestampEntriesMutex) {
        ESP_LOGE(TAG, "Failed to create timestampEntries mutex");
        vSemaphoreDelete(_smoothingPointsMutex);
        _smoothingPointsMutex = nullptr;
        vSemaphoreDelete(_commandMutex);
        _commandMutex = nullptr;
        vSemaphoreDelete(_udpMutex);
        _udpMutex = nullptr;
        vSemaphoreDelete(_persistMutex);
        _persistMutex = nullptr;
        vSemaphoreDelete(_subscriptionMutex);
        _subscriptionMutex = nullptr;
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
        return false;
    }

    // Create persistWithDebounce mutex
    _debounceEntriesMutex = xSemaphoreCreateMutex();
    if (!_debounceEntriesMutex) {
        ESP_LOGE(TAG, "Failed to create debounceEntries mutex");
        vSemaphoreDelete(_timestampEntriesMutex);
        _timestampEntriesMutex = nullptr;
        vSemaphoreDelete(_smoothingPointsMutex);
        _smoothingPointsMutex = nullptr;
        vSemaphoreDelete(_commandMutex);
        _commandMutex = nullptr;
        vSemaphoreDelete(_udpMutex);
        _udpMutex = nullptr;
        vSemaphoreDelete(_persistMutex);
        _persistMutex = nullptr;
        vSemaphoreDelete(_subscriptionMutex);
        _subscriptionMutex = nullptr;
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
        return false;
    }

    // Read MAC address from eFuse early so getMacAddress() works immediately.
    // Use Ethernet MAC type if an Ethernet adapter is configured, otherwise WiFi STA.
    // Note: eFuse may return all zeros on boards where WiFi is provided by a
    // co-processor (e.g., ESP32-P4 + ESP32-C6).  In that case the MAC will be
    // updated from the network library once the interface is up.
    esp_read_mac(_macBytes, _useEthernet ? ESP_MAC_ETH : ESP_MAC_WIFI_STA);
    formatMacStrings();
    _macInitialized = true;
    ESP_LOGI(TAG, "Initial MAC (from eFuse): %s", _macAddress);

    // Watchdog feed flags (actual watchdog setup happens in network task)
    _watchdogFeedRequested = false;
    _watchdogForceFeedRequested = false;
    _starveWatchdog = false;

    _currentStatus = StatusColor::CYAN;
    _macEventRaised = false;
    _networkConnectedEventRaised = false;
    _mqttConnectedEventRaised = false;
    // Setup flags: false = "needs setup" on next network-up iteration.
    // _apply*Config above has already cleared the broker/OTA flags; do MAC
    // explicitly too in case a prior end() left it true.
    _macSubstitutionsDone = false;
    queueStatusColor(StatusColor::CYAN);

    _running = true;

    // Create network task on core 0
    BaseType_t result = xTaskCreatePinnedToCore(
        networkTaskEntry,
        "NetClientA",
        8192,
        this,
        1,
        &_networkTaskHandle,
        0  // Core 0
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create network task");
        _running = false;
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
        vSemaphoreDelete(_subscriptionMutex);
        _subscriptionMutex = nullptr;
        return false;
    }

    ESP_LOGI(TAG, "Started");
    return true;
}

void ChipguyNetClient::end() {
    if (!_running) return;

    _running = false;

    if (_networkTaskHandle) {
        vTaskDelay(pdMS_TO_TICKS(100));
        vTaskDelete(_networkTaskHandle);
        _networkTaskHandle = nullptr;
    }

    // Disconnect client
    if (_mqttClient.connected()) {
        _mqttClient.disconnect();
    }
    WiFi.disconnect(true);

    // Clean up event memory
    if (_lastAllocatedData) {
        free(_lastAllocatedData);
        _lastAllocatedData = nullptr;
    }

    // Drain and free any pending events in the queue
    if (_eventQueue) {
        NetClientEvent event;
        while (xQueueReceive(_eventQueue, &event, 0) == pdTRUE) {
            if (event._allocatedData) {
                free(event._allocatedData);
            }
        }
        vQueueDelete(_eventQueue);
        _eventQueue = nullptr;
    }
    if (_subscriptionMutex) {
        vSemaphoreDelete(_subscriptionMutex);
        _subscriptionMutex = nullptr;
    }
    if (_persistMutex) {
        vSemaphoreDelete(_persistMutex);
        _persistMutex = nullptr;
    }
    _persists.clear();
    _subscriptions.clear();

    if (_commandMutex) {
        vSemaphoreDelete(_commandMutex);
        _commandMutex = nullptr;
    }
    _commandQueue.clear();

    if (_udpMutex) {
        vSemaphoreDelete(_udpMutex);
        _udpMutex = nullptr;
    }
    _udpQueue.clear();

    if (_configMutex) {
        vSemaphoreDelete(_configMutex);
        _configMutex = nullptr;
    }

    if (_smoothingPointsMutex) {
        xSemaphoreTake(_smoothingPointsMutex, portMAX_DELAY);
        for (auto& entry : _smoothingPoints) {
            delete entry.dataPoint;
        }
        _smoothingPoints.clear();
        xSemaphoreGive(_smoothingPointsMutex);
        vSemaphoreDelete(_smoothingPointsMutex);
        _smoothingPointsMutex = nullptr;
    }

    if (_timestampEntriesMutex) {
        xSemaphoreTake(_timestampEntriesMutex, portMAX_DELAY);
        for (auto& entry : _timestampEntries) {
            // Delete the watcher first: its publish callback holds a reference
            // to the data point's callback list, and we want it gone before the
            // data point itself goes.
            delete entry.watcher;
            delete entry.dataPoint;
        }
        _timestampEntries.clear();
        xSemaphoreGive(_timestampEntriesMutex);
        vSemaphoreDelete(_timestampEntriesMutex);
        _timestampEntriesMutex = nullptr;
    }

    if (_debounceEntriesMutex) {
        xSemaphoreTake(_debounceEntriesMutex, portMAX_DELAY);
        for (auto& entry : _debounceEntries) {
            delete entry.dataPoint;
        }
        _debounceEntries.clear();
        xSemaphoreGive(_debounceEntriesMutex);
        vSemaphoreDelete(_debounceEntriesMutex);
        _debounceEntriesMutex = nullptr;
    }

    // Free any dynamically allocated config strings
    freeAllConfigStrings();

    _instance = nullptr;
    ESP_LOGI(TAG, "Stopped");
}

bool ChipguyNetClient::eventAvailable() {
    if (!_eventQueue) return false;

    // Now that the application has had its turn with the previously-returned
    // event (between the prior getEvent() and this call), deliver it to
    // MqttDataPointWatcher before we tear down the event's allocated memory.
    // dispatchEvent() is idempotent — this is a no-op if the application
    // already dispatched manually.
    if (_pendingEvent.type != EventType::NONE) {
        MqttDataPointWatcher::dispatchEvent(_pendingEvent);
    }

    // Free memory from previous event
    if (_lastAllocatedData) {
        free(_lastAllocatedData);
        _lastAllocatedData = nullptr;
    }

    // Try to receive event (non-blocking)
    if (xQueueReceive(_eventQueue, &_pendingEvent, 0) == pdTRUE) {
        _lastAllocatedData = _pendingEvent._allocatedData;
        return true;
    }

    _pendingEvent.type = EventType::NONE;
    return false;
}

NetClientEvent ChipguyNetClient::getEvent() {
    return _pendingEvent;
}

bool ChipguyNetClient::publish(const char* topic, const char* payload, bool retained) {
    if (!payload) {
        // nullptr payload = remove pending publishes for this topic
        _removePublish(topic);
        return true;
    }
    return publish(topic, (const uint8_t*)payload, strlen(payload), retained);
}

bool ChipguyNetClient::publish(const char* topic, const uint8_t* payload, size_t length, bool retained) {
    if (!payload) {
        _removePublish(topic);
        return true;
    }
    _enqueuePublish(topic, payload, length, retained);
    return true;  // Returns true = successfully queued
}

bool ChipguyNetClient::publishf(const char* topic, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return publish(topic, buffer, true);
}

// ============================================================================
// Subscribe methods (one-time, not remembered for reconnect)
// ============================================================================

bool ChipguyNetClient::subscribe(const char* topic, uint8_t qos) {
    _enqueueSubscribe(topic, qos);
    return true;  // Returns true = successfully queued
}

// ============================================================================
// Auto-subscribe methods (remembered for automatic resubscribe on reconnect)
// ============================================================================

bool ChipguyNetClient::autoSubscribe(const char* topic, uint8_t qos) {
    return _autoSubscribeTo(topic, qos);
}

bool ChipguyNetClient::_autoSubscribeTo(const char* topic, uint8_t qos) {
    // Substitute MAC in topic for storage and comparison
    std::string substitutedTopic = substituteMAC(topic);

    xSemaphoreTake(_subscriptionMutex, portMAX_DELAY);

    // Check if already in list
    for (auto& entry : _subscriptions) {
        if (entry.topic == substitutedTopic) {
            bool alreadySubscribed = (entry.qos == qos);
            entry.qos = qos;
            xSemaphoreGive(_subscriptionMutex);

            // Enqueue subscribe if anything changed (best effort, non-blocking)
            if (!alreadySubscribed) {
                _enqueueSubscribe(substitutedTopic.c_str(), qos);
            }
            return true; // Success - subscription is remembered
        }
    }

    // Add new subscription with substituted topic
    SubscriptionEntry newEntry;
    newEntry.topic = substitutedTopic;
    newEntry.qos = qos;
    _subscriptions.push_back(std::move(newEntry));

    xSemaphoreGive(_subscriptionMutex);

    // Enqueue subscribe (best effort, non-blocking) - topic already substituted
    _enqueueSubscribe(substitutedTopic.c_str(), qos);
    return true; // Success - subscription is remembered
}

// ============================================================================
// Unsubscribe methods
// ============================================================================

bool ChipguyNetClient::unsubscribe(const char* topic) {
    return _unsubscribeFrom(topic);
}

bool ChipguyNetClient::_unsubscribeFrom(const char* topic) {
    // Substitute MAC in topic for comparison
    std::string substitutedTopic = substituteMAC(topic);

    xSemaphoreTake(_subscriptionMutex, portMAX_DELAY);

    for (auto it = _subscriptions.begin(); it != _subscriptions.end(); ++it) {
        if (it->topic == substitutedTopic) {
            _subscriptions.erase(it);
            break;
        }
    }

    xSemaphoreGive(_subscriptionMutex);

    // Enqueue unsubscribe (best effort, non-blocking) - topic already substituted
    _enqueueUnsubscribe(substitutedTopic.c_str());
    return true;
}

// ============================================================================
// Persist methods
// ============================================================================

bool ChipguyNetClient::persist(const char* topic, const char* payload) {
    if (!payload) {
        // nullptr payload = clear persist and remove pending publishes
        clearPersist(topic);
        _removePublish(topic);
        return true;
    }
    return _persistTo(topic, (const uint8_t*)payload, strlen(payload));
}

bool ChipguyNetClient::persist(const char* topic, const uint8_t* payload, size_t length) {
    if (!payload) {
        clearPersist(topic);
        _removePublish(topic);
        return true;
    }
    return _persistTo(topic, payload, length);
}

bool ChipguyNetClient::persistf(const char* topic, const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return persist(topic, buffer);
}

bool ChipguyNetClient::_persistTo(const char* topic, const uint8_t* payload, size_t length) {
    // Substitute MAC in topic for storage and comparison
    std::string substitutedTopic = substituteMAC(topic);

    xSemaphoreTake(_persistMutex, portMAX_DELAY);

    // Check if topic already exists
    for (auto& entry : _persists) {
        if (entry.topic == substitutedTopic) {
            // Check if this is a duplicate (same payload)
            bool samePayload = (entry.payload.size() == length &&
                                memcmp(entry.payload.data(), payload, length) == 0);
            if (samePayload) {
                // Refresh the expiration deadline even on a duplicate-payload
                // persist: the caller is signalling "this value is still
                // fresh", so the TTL clock should restart.
                if (entry.expirationSeconds > 0) {
                    entry.expirationDeadlineMillis = millis() + entry.expirationSeconds * 1000UL;
                }
                xSemaphoreGive(_persistMutex);
                return true; // Duplicate - nothing else to do
            }

            // Update payload
            entry.payload.assign(payload, payload + length);
            // Stamp publish time so republishPersists() won't resend prematurely
            unsigned long now = millis();
            entry.lastPublishTime = now;
            // Fresh value: refresh the expiration deadline if one is configured.
            if (entry.expirationSeconds > 0) {
                entry.expirationDeadlineMillis = now + entry.expirationSeconds * 1000UL;
            }
            xSemaphoreGive(_persistMutex);

            // Enqueue publish (non-blocking) - topic already substituted
            _enqueuePublish(substitutedTopic.c_str(), payload, length, true);
            return true;
        }
    }

    // Add new entry with substituted topic
    PersistEntry newEntry;
    newEntry.topic = substitutedTopic;
    newEntry.payload.assign(payload, payload + length);
    newEntry.lastPublishTime = millis();
    _persists.push_back(std::move(newEntry));

    xSemaphoreGive(_persistMutex);

    // Enqueue publish (non-blocking) - topic already substituted
    _enqueuePublish(substitutedTopic.c_str(), payload, length, true);
    return true;
}

bool ChipguyNetClient::persistWithSmoothing(float value, int decimalPlaces, float significanceThreshold,
                                            const char* topicFormat, ...) {
    if (!_smoothingPointsMutex) return false;

    // Format the topic with the user's varargs, preserving a leading "%P"
    // verbatim so NetClient.publish() can substitute the topic prefix at
    // publish time (same convention as MqttDataPoint topics).
    char formattedTopic[256];
    va_list args;
    va_start(args, topicFormat);
    MqttDataPoint::formatTopicV(formattedTopic, sizeof(formattedTopic), topicFormat, args);
    va_end(args);

    MqttNumericDataPoint* dp = nullptr;
    unsigned long expirationSeconds = 0;

    xSemaphoreTake(_smoothingPointsMutex, portMAX_DELAY);
    for (auto& entry : _smoothingPoints) {
        if (strcmp(entry.dataPoint->getTopic(), formattedTopic) == 0) {
            dp = entry.dataPoint;
            expirationSeconds = entry.expirationSeconds;
            break;
        }
    }
    if (!dp) {
        // Construct under the mutex so concurrent callers can't race to
        // create two data points for the same topic. The "%s" format keeps
        // the already-formatted topic intact (including any literal "%P").
        dp = new MqttNumericDataPoint(decimalPlaces, significanceThreshold,
                                      "%s", formattedTopic);
        _smoothingPoints.push_back({dp, 0});
    }
    xSemaphoreGive(_smoothingPointsMutex);

    // updateValue() takes the data point's own update semaphore; call it
    // outside our mutex. Passing expirationSeconds == 0 is equivalent to
    // updateNonExpiringValue() (the buffer is marked isNonExpiring).
    dp->updateValue(value, expirationSeconds);
    return true;
}

bool ChipguyNetClient::persistWithTimestamps(const char* value, const char* topicFormat, ...) {
    if (!_timestampEntriesMutex) return false;

    // Format the topic, preserving a leading "%P" (same convention as
    // MqttDataPoint topics).
    char formattedTopic[256];
    va_list args;
    va_start(args, topicFormat);
    MqttDataPoint::formatTopicV(formattedTopic, sizeof(formattedTopic), topicFormat, args);
    va_end(args);

    MqttStringDataPoint* dp = nullptr;
    unsigned long expirationSeconds = 0;

    xSemaphoreTake(_timestampEntriesMutex, portMAX_DELAY);
    for (auto& entry : _timestampEntries) {
        if (strcmp(entry.dataPoint->getTopic(), formattedTopic) == 0) {
            dp = entry.dataPoint;
            expirationSeconds = entry.expirationSeconds;
            break;
        }
    }
    if (!dp) {
        // Construct the data point first, then attach a watcher that observes
        // its publishes and emits "_active_at" / "_inactive_at" companions.
        // Both constructions happen under the mutex so concurrent callers
        // can't race to create two of either for the same topic.
        dp = new MqttStringDataPoint("%s", formattedTopic);
        MqttDataPointWatcher* watcher = new MqttDataPointWatcher(dp);
        _timestampEntries.push_back({dp, watcher, 0});
    }
    xSemaphoreGive(_timestampEntriesMutex);

    // updateValue() takes the data point's own update semaphore; call it
    // outside our mutex. Passing expirationSeconds == 0 is equivalent to
    // updateNonExpiringValue().
    dp->updateValue(value, expirationSeconds);
    return true;
}

bool ChipguyNetClient::persistWithDebounce(const char* value, const char* topicFormat, ...) {
    if (!_debounceEntriesMutex) return false;

    // Format the topic, preserving a leading "%P" (same convention as
    // MqttDataPoint topics).
    char formattedTopic[256];
    va_list args;
    va_start(args, topicFormat);
    MqttDataPoint::formatTopicV(formattedTopic, sizeof(formattedTopic), topicFormat, args);
    va_end(args);

    MqttStringDataPoint* dp = nullptr;
    unsigned long expirationSeconds = 0;

    xSemaphoreTake(_debounceEntriesMutex, portMAX_DELAY);
    for (auto& entry : _debounceEntries) {
        if (strcmp(entry.dataPoint->getTopic(), formattedTopic) == 0) {
            dp = entry.dataPoint;
            expirationSeconds = entry.expirationSeconds;
            break;
        }
    }
    if (!dp) {
        // Construct under the mutex so concurrent callers can't race to
        // create two data points for the same topic. The "%s" format keeps
        // the already-formatted topic intact (including any literal "%P").
        dp = new MqttStringDataPoint("%s", formattedTopic);
        _debounceEntries.push_back({dp, 0});
    }
    xSemaphoreGive(_debounceEntriesMutex);

    // updateValue() takes the data point's own update semaphore; call it
    // outside our mutex. Passing expirationSeconds == 0 is equivalent to
    // updateNonExpiringValue().
    dp->updateValue(value, expirationSeconds);
    return true;
}

bool ChipguyNetClient::persistWithTimestamps(bool value, const char* topicFormat, ...) {
    // Pre-format the topic here, then hand the fully-formatted string to the
    // const char* overload with a literal "%s" format — keeps the lookup and
    // construction logic in one place.
    char formattedTopic[256];
    va_list args;
    va_start(args, topicFormat);
    MqttDataPoint::formatTopicV(formattedTopic, sizeof(formattedTopic), topicFormat, args);
    va_end(args);
    return persistWithTimestamps(value ? "1" : "0", "%s", formattedTopic);
}

bool ChipguyNetClient::setExpirationSeconds(const char* topic, unsigned long seconds) {
    // Try the data-point-backed sets first (smoothing, then timestamps,
    // then debounce). The data point's topic is stored with "%P" preserved
    // (publish-time prefix expansion), so we compare against `topic`
    // directly.
    if (_smoothingPointsMutex) {
        xSemaphoreTake(_smoothingPointsMutex, portMAX_DELAY);
        for (auto& entry : _smoothingPoints) {
            if (strcmp(entry.dataPoint->getTopic(), topic) == 0) {
                entry.expirationSeconds = seconds;
                xSemaphoreGive(_smoothingPointsMutex);
                return true;
            }
        }
        xSemaphoreGive(_smoothingPointsMutex);
    }

    if (_timestampEntriesMutex) {
        xSemaphoreTake(_timestampEntriesMutex, portMAX_DELAY);
        for (auto& entry : _timestampEntries) {
            if (strcmp(entry.dataPoint->getTopic(), topic) == 0) {
                entry.expirationSeconds = seconds;
                xSemaphoreGive(_timestampEntriesMutex);
                return true;
            }
        }
        xSemaphoreGive(_timestampEntriesMutex);
    }

    if (_debounceEntriesMutex) {
        xSemaphoreTake(_debounceEntriesMutex, portMAX_DELAY);
        for (auto& entry : _debounceEntries) {
            if (strcmp(entry.dataPoint->getTopic(), topic) == 0) {
                entry.expirationSeconds = seconds;
                xSemaphoreGive(_debounceEntriesMutex);
                return true;
            }
        }
        xSemaphoreGive(_debounceEntriesMutex);
    }

    // Fall back to the plain persist set. Those entries are stored with the
    // topic fully substituted (MAC + %P prefix), so apply the same
    // substitution to the lookup key.
    if (_persistMutex) {
        std::string substitutedTopic = substituteMAC(topic);
        xSemaphoreTake(_persistMutex, portMAX_DELAY);
        for (auto& entry : _persists) {
            if (entry.topic == substitutedTopic) {
                entry.expirationSeconds = seconds;
                if (seconds > 0) {
                    entry.expirationDeadlineMillis = millis() + seconds * 1000UL;
                } else {
                    entry.expirationDeadlineMillis = 0;
                }
                xSemaphoreGive(_persistMutex);
                return true;
            }
        }
        xSemaphoreGive(_persistMutex);
    }

    return false;
}

void ChipguyNetClient::clearPersist(const char* topic) {
    std::string substitutedTopic = substituteMAC(topic);

    xSemaphoreTake(_persistMutex, portMAX_DELAY);

    for (auto it = _persists.begin(); it != _persists.end(); ++it) {
        if (it->topic == substitutedTopic) {
            _persists.erase(it);
            break;
        }
    }

    xSemaphoreGive(_persistMutex);
}

void ChipguyNetClient::clearPersists() {
    xSemaphoreTake(_persistMutex, portMAX_DELAY);
    _persists.clear();
    xSemaphoreGive(_persistMutex);
}

void ChipguyNetClient::republishPersists() {
    // Publish at most one aged entry per call to spread network I/O over
    // time and avoid bursts.  The mutex is held only long enough to find
    // and copy out a candidate, so persist()/persistf() from the main
    // thread won't stall on network delays.

    if (!_mqttConnected) return;

    unsigned long now = millis();
    unsigned long interval = _config.persistIntervalMs;

    std::string topic;
    std::vector<uint8_t> payload;
    size_t foundIndex = SIZE_MAX;

    // ── Find the oldest entry that has aged past the interval ────────
    xSemaphoreTake(_persistMutex, portMAX_DELAY);
    for (size_t i = 0; i < _persists.size(); i++) {
        auto& entry = _persists[i];

        // Skip entries past their expiration deadline. Unsigned subtraction
        // handles millis() rollover the same way the persistInterval check
        // below does.
        if (entry.expirationSeconds > 0 &&
            (long)(now - entry.expirationDeadlineMillis) >= 0) {
            continue;
        }

        if (now - entry.lastPublishTime >= interval) {
            topic = entry.topic;
            payload = entry.payload;
            // Stamp now so it won't be picked again until it ages out
            entry.lastPublishTime = now;
            foundIndex = i;
            break;
        }
    }
    xSemaphoreGive(_persistMutex);

    if (foundIndex == SIZE_MAX) return;  // nothing due

    // ── Publish without holding the mutex ────────────────────────────
    _mqttClient.publish(topic.c_str(), payload.data(), payload.size(), true);

    feedWatchdog();
}

// ============================================================================
// Command queue methods (for non-blocking network operations)
// ============================================================================

// Helper to find and replace or append a command in a deque (deduplicates by topic and type)
static void enqueueOrReplace(std::deque<NetClientCommand>& queue,
                              SemaphoreHandle_t mutex,
                              const NetClientCommand& cmd) {
    xSemaphoreTake(mutex, portMAX_DELAY);

    // Search for existing command of same type and topic
    for (auto& existing : queue) {
        if (existing.type == cmd.type && existing.topic == cmd.topic) {
            // Replace with new command data
            existing.payload = cmd.payload;
            existing.payloadLength = cmd.payloadLength;
            existing.retained = cmd.retained;
            existing.qos = cmd.qos;
            xSemaphoreGive(mutex);
            return;
        }
    }

    // Not found - add new command
    queue.push_back(cmd);
    xSemaphoreGive(mutex);
}

// Helper to remove PUBLISH commands for a topic from a queue
static void removePublishFromQueue(std::deque<NetClientCommand>& queue,
                                    SemaphoreHandle_t mutex,
                                    const std::string& topic) {
    xSemaphoreTake(mutex, portMAX_DELAY);

    for (auto it = queue.begin(); it != queue.end(); ) {
        if (it->type == CommandType::PUBLISH && it->topic == topic) {
            it = queue.erase(it);
        } else {
            ++it;
        }
    }

    xSemaphoreGive(mutex);
}

void ChipguyNetClient::_enqueuePublish(const char* topic, const uint8_t* payload, size_t length,
                                        bool retained) {
    if (!isMqttEnabled()) return;  // disabled broker: drop, never drained

    NetClientCommand cmd;
    cmd.type = CommandType::PUBLISH;
    cmd.topic = substituteMAC(topic);  // Apply MAC substitution to topic
    cmd.payload.assign(payload, payload + length);
    cmd.payloadLength = length;
    cmd.retained = retained;

    enqueueOrReplace(_commandQueue, _commandMutex, cmd);
}

void ChipguyNetClient::_enqueueSubscribe(const char* topic, uint8_t qos) {
    if (!isMqttEnabled()) return;

    NetClientCommand cmd;
    cmd.type = CommandType::SUBSCRIBE;
    cmd.topic = substituteMAC(topic);  // Apply MAC substitution to topic
    cmd.qos = qos;

    enqueueOrReplace(_commandQueue, _commandMutex, cmd);
}

void ChipguyNetClient::_enqueueUnsubscribe(const char* topic) {
    if (!isMqttEnabled()) return;

    NetClientCommand cmd;
    cmd.type = CommandType::UNSUBSCRIBE;
    cmd.topic = substituteMAC(topic);  // Apply MAC substitution to topic

    enqueueOrReplace(_commandQueue, _commandMutex, cmd);
}

void ChipguyNetClient::_removePublish(const char* topic) {
    std::string substitutedTopic = substituteMAC(topic);
    removePublishFromQueue(_commandQueue, _commandMutex, substitutedTopic);
}

void ChipguyNetClient::_processCommands() {
    if (!_mqttConnected) return;

    // Process all pending commands
    while (true) {
        NetClientCommand cmd;

        xSemaphoreTake(_commandMutex, portMAX_DELAY);
        if (_commandQueue.empty()) {
            xSemaphoreGive(_commandMutex);
            break;
        }
        cmd = std::move(_commandQueue.front());
        _commandQueue.pop_front();
        xSemaphoreGive(_commandMutex);

        // Execute command
        switch (cmd.type) {
            case CommandType::PUBLISH:
                if (_mqttClient.publish(cmd.topic.c_str(), cmd.payload.data(), cmd.payloadLength, cmd.retained)) {
                    feedWatchdog();
                }
                break;
            case CommandType::SUBSCRIBE:
                _mqttClient.subscribe(cmd.topic.c_str(), cmd.qos);
                break;
            case CommandType::UNSUBSCRIBE:
                _mqttClient.unsubscribe(cmd.topic.c_str());
                break;
        }
    }
}

int8_t ChipguyNetClient::getWiFiRSSI() const {
    if (_useEthernet) return 0;  // No RSSI for Ethernet
    if (_wifiConnected) {
        return (int8_t)WiFi.RSSI();
    }
    return 0;
}

uint16_t ChipguyNetClient::getEthernetLinkSpeed() const {
    if (_useEthernet && _ethConnected) {
        return ETH.linkSpeed();
    }
    return 0;
}

// --- UDP send ---

bool ChipguyNetClient::sendUDP(const IPAddress& ip, uint16_t port,
                                const uint8_t* payload, size_t length,
                                bool deduplicate) {
    if (!isNetworkConnected()) return false;
    return _enqueueUdp(ip, port, payload, length, deduplicate);
}

bool ChipguyNetClient::sendUDP(std::nullptr_t, uint16_t port,
                                const uint8_t* payload, size_t length,
                                bool deduplicate) {
    if (!isNetworkConnected()) return false;
    IPAddress brokerIP = _wifiClient.remoteIP();
    if (brokerIP == IPAddress(0, 0, 0, 0)) return false;
    return _enqueueUdp(brokerIP, port, payload, length, deduplicate);
}

bool ChipguyNetClient::_enqueueUdp(const IPAddress& ip, uint16_t port,
                                     const uint8_t* payload, size_t length,
                                     bool deduplicate) {
    unsigned long deadline = millis() + 250;

    xSemaphoreTake(_udpMutex, portMAX_DELAY);

    if (deduplicate) {
        for (auto& entry : _udpQueue) {
            if (entry.ip == ip && entry.port == port &&
                entry.payload.size() == length &&
                memcmp(entry.payload.data(), payload, length) == 0) {
                entry.expiresAt = deadline;
                xSemaphoreGive(_udpMutex);
                return true;
            }
        }
    }

    _udpQueue.push_back({ip, port, {payload, payload + length}, deadline});
    xSemaphoreGive(_udpMutex);
    return true;
}

void ChipguyNetClient::_processUdpQueue() {
    std::deque<UdpQueueEntry> local;

    xSemaphoreTake(_udpMutex, portMAX_DELAY);
    local.swap(_udpQueue);
    xSemaphoreGive(_udpMutex);

    unsigned long now = millis();
    for (auto& entry : local) {
        if (now > entry.expiresAt) continue;  // expired
        _udp.beginPacket(entry.ip, entry.port);
        _udp.write(entry.payload.data(), entry.payload.size());
        _udp.endPacket();
    }
}

bool ChipguyNetClient::isMqttEnabled() const {
    return _mqttServer != nullptr && _mqttServer[0] != '\0';
}

void ChipguyNetClient::_applyTopicPrefix(const char* newPrefix, bool duplicateRamString) {
    freeConfigString(_topicPrefix);
    if (!newPrefix) {
        _topicPrefix = nullptr;
    } else if (!duplicateRamString || isInFlash(newPrefix)) {
        _topicPrefix = newPrefix;
    } else {
        _topicPrefix = strdup(newPrefix);
    }
    // Re-expand any buffers that use %P so they're ready for the next connect
    if (_macInitialized) {
        performMacSubstitutions();
    }
}

void ChipguyNetClient::_applyBrokerConfig(const MQTTBrokerConfig& cfg) {
    freeConfigString(_mqttServer);
    freeConfigString(_mqttUsername);
    freeConfigString(_mqttPassword);
    freeConfigString(_caCert);
    freeConfigString(_lastWillMessage);

    _config.mqtt = cfg;  // value fields (port, qos, etc.) and pointer-only fields (clientId, lastWillTopic, onlineMessage)
    _mqttServer = copyConfigString(cfg.server);
    _mqttUsername = copyConfigString(cfg.username);
    _mqttPassword = copyConfigString(cfg.password);
    _caCert = copyConfigString(cfg.caCert);
    _lastWillMessage = copyConfigString(cfg.lastWillMessage);

    if (_macInitialized) {
        substituteMAC(_clientId, sizeof(_clientId), _config.mqtt.clientId);
        substituteMAC(_lastWillTopic, sizeof(_lastWillTopic), _config.mqtt.lastWillTopic);
    }

    // Force the network task to re-run setupMqttClient with the new pointers
    // on its next iteration. It also handles the disconnect-before-resetup
    // case when applicable.
    _mqttSetupDone = false;
}

void ChipguyNetClient::_applyOTAConfig(const char* hostname, const char* password) {
    freeConfigString(_otaPassword);
    _otaPassword = copyConfigString(password);

    // hostname stays as a borrowed pointer in _config; the substituted form
    // (with MAC bytes / "esp32-MAC" default) lands in _otaHostname[].
    _config.otaHostname = hostname;
    if (_macInitialized) {
        if (hostname) {
            substituteMAC(_otaHostname, sizeof(_otaHostname), hostname);
        } else {
            snprintf(_otaHostname, sizeof(_otaHostname), "esp32-%s", _macAddressShort);
        }
    }

    _otaSetupDone = false;
}

void ChipguyNetClient::updateConfiguration(const NetClientConfig& config, uint32_t sections) {
    if (!_configMutex) return;  // not initialized yet
    xSemaphoreTake(_configMutex, portMAX_DELAY);
    if (sections & ConfigSection::TOPIC_PREFIX) {
        _applyTopicPrefix(config.topicPrefix, /*duplicateRamString=*/true);
    }
    if (sections & ConfigSection::BROKER) {
        _applyBrokerConfig(config.mqtt);
    }
    if (sections & ConfigSection::OTA) {
        _applyOTAConfig(config.otaHostname, config.otaPassword);
    }
    xSemaphoreGive(_configMutex);
}

void ChipguyNetClient::updateTopicPrefix(const char* newPrefix, bool duplicateRamString) {
    if (_configMutex) xSemaphoreTake(_configMutex, portMAX_DELAY);
    _applyTopicPrefix(newPrefix, duplicateRamString);
    if (_configMutex) xSemaphoreGive(_configMutex);
}

void ChipguyNetClient::feedWatchdog(bool force) {
    if (force) {
        // If we're already on the network task (the OTA callbacks are — they
        // run inside ArduinoOTA.handle() which is called from the network
        // loop), do the reset right here so the dog gets fed even if the
        // network task is stuck inside ArduinoOTA.handle() and won't drain
        // the flag for a while.
        if (_networkTaskHandle && xTaskGetCurrentTaskHandle() == _networkTaskHandle) {
            if (_config.watchdogTimeoutSec > 0) {
                esp_task_wdt_reset();
            }
            return;
        }
        _watchdogForceFeedRequested = true;
    } else {
        // Signal network task A to feed the hardware watchdog
        // (cross-thread safe - network task checks this flag).
        _watchdogFeedRequested = true;
    }
}

void ChipguyNetClient::starveWatchdog(bool starve) {
    // Just flips the flag; intentionally does NOT feed the watchdog when
    // un-starving — feeds resume on the next caller.
    _starveWatchdog = starve;
}

std::string ChipguyNetClient::expandTopic(const char* topic) {
    if (!topic) return std::string();
    return substituteMAC(topic);
}

void ChipguyNetClient::ensureMacInitialized() const {
    if (_macInitialized) return;

    // Read MAC from eFuse (WiFi STA type as default before begin() is called)
    esp_read_mac(_macBytes, ESP_MAC_WIFI_STA);
    formatMacStrings();
    _macInitialized = true;
}

const char* ChipguyNetClient::getMacAddress() const {
    ensureMacInitialized();
    return _macAddress;
}

const char* ChipguyNetClient::getMacAddressShort() const {
    ensureMacInitialized();
    return _macAddressShort;
}

uint32_t ChipguyNetClient::getMacAddress32() const {
    ensureMacInitialized();
    return ((uint32_t)_macBytes[2] << 24) | ((uint32_t)_macBytes[3] << 16) |
           ((uint32_t)_macBytes[4] << 8)  | (uint32_t)_macBytes[5];
}

uint64_t ChipguyNetClient::getMacAddress64() const {
    ensureMacInitialized();
    return ((uint64_t)_macBytes[0] << 40) | ((uint64_t)_macBytes[1] << 32) |
           ((uint64_t)_macBytes[2] << 24) | ((uint64_t)_macBytes[3] << 16) |
           ((uint64_t)_macBytes[4] << 8)  | (uint64_t)_macBytes[5];
}

void ChipguyNetClient::formatMacStrings() const {
    snprintf(_macAddress, sizeof(_macAddress), "%02X:%02X:%02X:%02X:%02X:%02X",
             _macBytes[0], _macBytes[1], _macBytes[2], _macBytes[3], _macBytes[4], _macBytes[5]);

    snprintf(_macAddressShort, sizeof(_macAddressShort), "%02X%02X%02X%02X%02X%02X",
             _macBytes[0], _macBytes[1], _macBytes[2], _macBytes[3], _macBytes[4], _macBytes[5]);
}

void ChipguyNetClient::performMacSubstitutions() {
    substituteMAC(_clientId, sizeof(_clientId), _config.mqtt.clientId);
    substituteMAC(_lastWillTopic, sizeof(_lastWillTopic), _config.mqtt.lastWillTopic);

    if (_config.otaHostname) {
        substituteMAC(_otaHostname, sizeof(_otaHostname), _config.otaHostname);
    } else {
        snprintf(_otaHostname, sizeof(_otaHostname), "esp32-%s", _macAddressShort);
    }
}

void ChipguyNetClient::updateMacFrom(const uint8_t* newMac) {
    if (memcmp(_macBytes, newMac, 6) == 0) return;  // No change

    ESP_LOGI(TAG, "MAC updated from network interface (was %s)", _macAddress);
    memcpy(_macBytes, newMac, 6);
    formatMacStrings();
    performMacSubstitutions();
    ESP_LOGI(TAG, "MAC is now %s", _macAddress);
}

void ChipguyNetClient::setupMacAddress() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    updateMacFrom(mac);
    queueMacAvailable();
}

void ChipguyNetClient::setupMacAddressEthernet() {
    uint8_t mac[6];
    ETH.macAddress(mac);
    updateMacFrom(mac);
    queueMacAvailable();
}

void ChipguyNetClient::substituteMAC(char* dest, size_t destSize, const char* src) {
    if (!src) {
        dest[0] = '\0';
        return;
    }

    // MAC substitution patterns: %[separator][count][case]
    //   separator: '-' (dash), ':' (colon), or none
    //   count: 4 (last 4 hex), 6 (last 6 hex), 12 or omitted (full 12 hex)
    //   case: 'x' (lowercase), 'X' (uppercase)
    //
    // Examples:
    //   %x   = aabbccddeeff       %X   = AABBCCDDEEFF
    //   %-x  = aa-bb-cc-dd-ee-ff  %-X  = AA-BB-CC-DD-EE-FF
    //   %:x  = aa:bb:cc:dd:ee:ff  %:X  = AA:BB:CC:DD:EE:FF
    //   %4x  = eeff               %4X  = EEFF
    //   %6x  = ddeeff             %6X  = DDEEFF
    //   %-6x = dd-ee-ff           %-6X = DD-EE-FF
    //   %:4x = ee:ff              %:4X = EE:FF
    //   %12x = aabbccddeeff       %12X = AABBCCDDEEFF (same as %x/%X)

    const char* p = src;
    char* d = dest;
    char* dEnd = dest + destSize - 1;

    while (*p && d < dEnd) {
        if (*p == '%') {
            const char* start = p;
            p++;  // Skip '%'

            // Parse optional separator
            char separator = '\0';
            if (*p == '-' || *p == ':') {
                separator = *p++;
            }

            // Parse optional count (4, 6, or 12; 1 and 2 are ignored as part of 12)
            int numOctets = 6;  // Default: all 6 octets (12 hex chars)
            if (*p == '4') {
                numOctets = 2;  // Last 2 octets (4 hex chars)
                p++;
            } else if (*p == '6') {
                numOctets = 3;  // Last 3 octets (6 hex chars)
                p++;
            } else if (*p == '1') {
                p++;  // Skip '1', check for '2'
                if (*p == '2') {
                    p++;  // Skip '2' - %12x means full MAC (default)
                }
                // numOctets stays 6 (full MAC)
            }

            // Check for %P (topic prefix substitution)
            if (separator == '\0' && numOctets == 6 && *p == 'P') {
                p++;  // Skip 'P'
                // Recursively expand the prefix (handles %X in prefix)
                if (_topicPrefix && _topicPrefix[0]) {
                    std::string expandedPrefix = substituteMAC(_topicPrefix);
                    for (const char* pp = expandedPrefix.c_str(); *pp && d < dEnd; ) {
                        *d++ = *pp++;
                    }
                }
            }
            // Parse required case specifier for MAC patterns
            else if (*p == 'x' || *p == 'X') {
                bool uppercase = (*p == 'X');
                p++;  // Skip 'x' or 'X'

                // Determine which octets to output
                int startOctet = 6 - numOctets;

                // Format the MAC
                const char* hexLower = "0123456789abcdef";
                const char* hexUpper = "0123456789ABCDEF";
                const char* hex = uppercase ? hexUpper : hexLower;

                for (int i = startOctet; i < 6 && d < dEnd; i++) {
                    if (separator && i > startOctet && d < dEnd) {
                        *d++ = separator;
                    }
                    if (d < dEnd) *d++ = hex[(_macBytes[i] >> 4) & 0x0F];
                    if (d < dEnd) *d++ = hex[_macBytes[i] & 0x0F];
                }
            } else {
                // Not a valid pattern, copy the '%' and continue
                p = start;
                *d++ = *p++;
            }
        } else {
            *d++ = *p++;
        }
    }
    *d = '\0';
}

std::string ChipguyNetClient::substituteMAC(const char* src) {
    if (!src) return std::string();

    // Use a reasonable buffer size for most topics
    char buffer[256];
    substituteMAC(buffer, sizeof(buffer), src);
    return std::string(buffer);
}

void ChipguyNetClient::setupWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);
}

bool ChipguyNetClient::connectWiFi() {
    int bestRSSI = -999;
    uint8_t bestBSSID[6] = {0};
    int bestChannel = 0;
    bool foundAP = false;

    // Scan for best AP if enabled
    if (_config.doWifiScan) {
        ESP_LOGI(TAG, "Scanning for WiFi networks...");
        int n = WiFi.scanNetworks();

        for (int i = 0; i < n; i++) {
            if (strcmp(WiFi.SSID(i).c_str(), _wifiSSID) == 0) {
                if (WiFi.RSSI(i) > bestRSSI) {
                    bestRSSI = WiFi.RSSI(i);
                    memcpy(bestBSSID, WiFi.BSSID(i), 6);
                    bestChannel = WiFi.channel(i);
                    foundAP = true;
                }
            }
        }
        WiFi.scanDelete();

        if (foundAP) {
            ESP_LOGI(TAG, "Best AP: %02X:%02X:%02X:%02X:%02X:%02X, RSSI: %d, Channel: %d",
                     bestBSSID[0], bestBSSID[1], bestBSSID[2],
                     bestBSSID[3], bestBSSID[4], bestBSSID[5],
                     bestRSSI, bestChannel);
        }
    }

    // Connect to WiFi
    if (_config.useWPA2Enterprise) {
        // WPA2-Enterprise setup using new Arduino ESP32 3.x API
        // Signature: begin(ssid, method, identity, username, password, ca_pem, client_crt, client_key, ttls_phase2, channel, bssid, connect)
        WiFi.disconnect(true);
        WiFi.mode(WIFI_STA);

        if (foundAP) {
            WiFi.begin(_wifiSSID, WPA2_AUTH_PEAP,
                      _wifiUsername, _wifiUsername, _wifiPassword,
                      nullptr, nullptr, nullptr, -1,
                      bestChannel, bestBSSID, true);
        } else {
            WiFi.begin(_wifiSSID, WPA2_AUTH_PEAP,
                      _wifiUsername, _wifiUsername, _wifiPassword);
        }
    } else {
        // Standard WPA2
        if (foundAP) {
            WiFi.begin(_wifiSSID, _wifiPassword, bestChannel, bestBSSID);
        } else {
            WiFi.begin(_wifiSSID, _wifiPassword);
        }
    }

    // Update MAC from WiFi interface (may differ from eFuse on co-processor boards)
    setupMacAddress();

    // Wait for connection
    ESP_LOGI(TAG, "Connecting to WiFi '%s'...", _wifiSSID);
    unsigned long startTime = millis();
    const unsigned long timeout = 30000; // 30 seconds

    while (WiFi.status() != WL_CONNECTED && (millis() - startTime) < timeout) {
        delay(100);
        feedWatchdog();

        if (_otaEnabled) {
            ArduinoOTA.handle();
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        strncpy(_localIP, WiFi.localIP().toString().c_str(), sizeof(_localIP) - 1);
        ESP_LOGI(TAG, "WiFi connected, IP: %s", _localIP);
        feedWatchdog();
        queueNetworkConnected(_localIP);
        return true;
    } else {
        ESP_LOGW(TAG, "WiFi connection failed");
        return false;
    }
}

// ============================================================================
// Ethernet methods
// ============================================================================

void ChipguyNetClient::onEthernetEvent(arduino_event_id_t event) {
    if (_instance) _instance->handleEthernetEvent(event);
}

void ChipguyNetClient::handleEthernetEvent(arduino_event_id_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            ESP_LOGI(TAG, "Ethernet started");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            ESP_LOGI(TAG, "Ethernet link up");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            ESP_LOGI(TAG, "Ethernet got IP: %s", ETH.localIP().toString().c_str());
            strncpy(_localIP, ETH.localIP().toString().c_str(), sizeof(_localIP) - 1);
            setupMacAddressEthernet();  // Update MAC from ETH interface if different
            _ethConnected = true;
            if (!_networkConnectedEventRaised) {
                queueNetworkConnected(_localIP);
                _networkConnectedEventRaised = true;
            }
            updateStatusColor();
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link down");
            if (_ethConnected) {
                _ethConnected = false;
                _mqttConnected = false;
                queueNetworkConnectionLost();  // Also queues MQTT connection lost events
                updateStatusColor();
            }
            break;
        default:
            break;
    }
}

void ChipguyNetClient::setupEthernet() {
    // Register event handler
    WiFi.onEvent(onEthernetEvent);

    // Call optional hardware reset callback
    if (_ethernetConfig.resetCallback) {
        ESP_LOGI(TAG, "Calling Ethernet reset callback");
        _ethernetConfig.resetCallback();
    }
}

bool ChipguyNetClient::connectEthernet() {
    ESP_LOGI(TAG, "Starting Ethernet interface...");

    bool success;
    if (_ethernetConfig.spiCs >= 0) {
        // SPI Ethernet (W5500, etc.)
        // Use the spi_host_device_t overload so ETH.begin() calls
        // spi_bus_initialize() itself.  This is safe even if the bus is
        // already initialized (e.g. by a display driver sharing the bus)
        // because ESP-IDF returns ESP_ERR_INVALID_STATE, which ETH
        // treats as success.  Avoids the Arduino SPI.begin() path which
        // can conflict with other SPI drivers on a shared bus.
        if (_ethernetConfig.spiBus) {
            // If a custom SPIClass was provided, use SPI.begin() path instead
            _ethernetConfig.spiBus->begin(_ethernetConfig.spiSck, _ethernetConfig.spiMiso, _ethernetConfig.spiMosi);
            success = ETH.begin(
                _ethernetConfig.phyType,
                _ethernetConfig.phyAddr,
                _ethernetConfig.spiCs,
                _ethernetConfig.spiIrq,
                _ethernetConfig.phyRst,
                *_ethernetConfig.spiBus,
                _ethernetConfig.spiFreqMhz
            );
        } else {
            success = ETH.begin(
                _ethernetConfig.phyType,
                _ethernetConfig.phyAddr,
                _ethernetConfig.spiCs,
                _ethernetConfig.spiIrq,
                _ethernetConfig.phyRst,
                _ethernetConfig.spiHost,
                _ethernetConfig.spiSck,
                _ethernetConfig.spiMiso,
                _ethernetConfig.spiMosi,
                _ethernetConfig.spiFreqMhz
            );
        }
    }
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32P4
    else {
        // RMII Ethernet (LAN8720, IP101, etc.) - available on ESP32 and ESP32-P4
        success = ETH.begin(
            _ethernetConfig.phyType,
            _ethernetConfig.phyAddr,
            _ethernetConfig.phyMdc,
            _ethernetConfig.phyMdio,
            _ethernetConfig.phyPower,
            _ethernetConfig.clkMode
        );
    }
#else
    else {
        // RMII not available on ESP32-S2/S3/C3/etc. - must use SPI Ethernet
        ESP_LOGE(TAG, "RMII Ethernet not available on this chip. Use SPI Ethernet (spiCs >= 0).");
        return false;
    }
#endif

    if (!success) {
        ESP_LOGE(TAG, "ETH.begin() failed");
        return false;
    }
    return true;
}

void ChipguyNetClient::setupMqttClient() {
    if (_caCert) {
        _wifiClient.setCACert(_caCert);
    } else {
        _wifiClient.setInsecure(); // Allow connection without cert verification
    }

    _wifiClient.setHandshakeTimeout(10);  // 10 seconds (default is 120)

    _mqttClient.setServer(_mqttServer, _config.mqtt.port);
    _mqttClient.setBufferSize(_config.mqttBufferSize > 0 ? _config.mqttBufferSize : 1024);
    _mqttClient.setCallback(mqttCallback);
}

bool ChipguyNetClient::connectMqtt() {
    ESP_LOGI(TAG, "Connecting to MQTT broker (%s:%d)...", _mqttServer, _config.mqtt.port);

    bool connected = false;
    if (_lastWillTopic[0]) {
        connected = _mqttClient.connect(_clientId, _mqttUsername, _mqttPassword,
                                        _lastWillTopic, _config.mqtt.lastWillQos,
                                        _config.mqtt.lastWillRetain, _lastWillMessage);
    } else {
        connected = _mqttClient.connect(_clientId, _mqttUsername, _mqttPassword);
    }

    if (connected) {
        ESP_LOGI(TAG, "MQTT connected");

        // Persist online status to lastWillTopic (nullptr onlineMessage disables this)
        const char* onlineMsg = _config.mqtt.onlineMessage;
        if (_lastWillTopic[0] && onlineMsg && onlineMsg[0]) {
            persist(_lastWillTopic, onlineMsg);
        }

        // Feed hardware watchdog on MQTT connect (we're on the network task).
        if (_config.watchdogTimeoutSec > 0 && !_starveWatchdog) {
            esp_task_wdt_reset();
        }

        queueMqttConnected();
        resubscribe();
        return true;
    } else {
        int state = _mqttClient.state();
        ESP_LOGW(TAG, "MQTT connection failed: %d %s", state, mqttStateToString(state));
        return false;
    }
}

void ChipguyNetClient::resubscribe() {
    // Resubscribe one topic at a time, holding the mutex only long enough
    // to copy the data out.  This avoids blocking autoSubscribe() during
    // potentially slow network I/O.

    size_t index = 0;

    while (true) {
        std::string topic;
        uint8_t qos = 0;

        xSemaphoreTake(_subscriptionMutex, portMAX_DELAY);
        if (index < _subscriptions.size()) {
            const auto& entry = _subscriptions[index];
            topic = entry.topic;
            qos = entry.qos;
            index++;
            xSemaphoreGive(_subscriptionMutex);
        } else {
            xSemaphoreGive(_subscriptionMutex);
            break;
        }

        _mqttClient.subscribe(topic.c_str(), qos);
        ESP_LOGD(TAG, "Resubscribed to %s", topic.c_str());
    }
}

void ChipguyNetClient::setupOTA() {
    if (!_otaPassword || _otaPassword[0] == '\0') {
        _otaEnabled = false;
        return;
    }

    ArduinoOTA.setHostname(_otaHostname);
    ArduinoOTA.setPassword(_otaPassword);

    // OTA callbacks force-feed (bypassing the starve flag) so a manual
    // firmware push can recover an intentionally-starved device.
    ArduinoOTA.onStart([this]() {
        ESP_LOGI(TAG, "OTA update starting...");
        feedWatchdog(true);
    });

    ArduinoOTA.onEnd([this]() {
        ESP_LOGI(TAG, "OTA update complete");
        feedWatchdog(true);
    });

    ArduinoOTA.onProgress([this](unsigned int progress, unsigned int total) {
        feedWatchdog(true);
        ESP_LOGD(TAG, "OTA progress: %u%%", (progress * 100) / total);
    });

    ArduinoOTA.onError([this](ota_error_t error) {
        ESP_LOGE(TAG, "OTA error: %u", error);
        feedWatchdog(true);  // Feed watchdog so device can recover from error
    });

    ArduinoOTA.begin();
    _otaEnabled = true;
    ESP_LOGI(TAG, "OTA enabled, hostname: %s", _otaHostname);
}

void ChipguyNetClient::queueMacAvailable() {
    if (_macEventRaised) return;

    NetClientEvent event;
    event.type = EventType::MAC_ADDRESS_AVAILABLE;
    event._allocatedData = nullptr;
    event.mac_address_available_event.macAddress = _macAddress;
    _macEventRaised = true;
    xQueueSend(_eventQueue, &event, pdMS_TO_TICKS(100));
}

void ChipguyNetClient::queueNetworkConnected(const char* ip) {
    NetClientEvent event;
    event.type = EventType::NETWORK_CONNECTED;
    event._allocatedData = nullptr;

    // Point directly to _localIP member buffer - safe to hold this pointer
    // as it persists for the lifetime of the NetClient instance
    event.network_connected_event.localIP = _localIP;

    _networkConnectedEventRaised = true;
    xQueueSend(_eventQueue, &event, pdMS_TO_TICKS(100));
}

void ChipguyNetClient::queueNetworkConnectionLost() {
    // Only raise CONNECTION_LOST if CONNECTED was previously raised
    if (!_networkConnectedEventRaised) {
        return;
    }

    // Proactively queue MQTT connection lost first (so user sees sensible sequence).
    // Only queues if the connected event was previously raised; clears the flag too,
    // so any later "real" MQTT connection lost from PubSubClient will be discarded.
    queueMqttConnectionLost();

    NetClientEvent event;
    event.type = EventType::NETWORK_CONNECTION_LOST;
    event._allocatedData = nullptr;

    // Discard any pending UDP frames — network is gone, no point keeping them
    xSemaphoreTake(_udpMutex, portMAX_DELAY);
    _udpQueue.clear();
    xSemaphoreGive(_udpMutex);

    _networkConnectedEventRaised = false;
    xQueueSend(_eventQueue, &event, pdMS_TO_TICKS(100));
}

void ChipguyNetClient::queueMqttConnected() {
    NetClientEvent event;
    event.type = EventType::MQTT_SERVER_CONNECTED;
    event._allocatedData = nullptr;

    _mqttConnectedEventRaised = true;

    xQueueSend(_eventQueue, &event, pdMS_TO_TICKS(100));
}

void ChipguyNetClient::queueMqttConnectionLost() {
    // Only raise CONNECTION_LOST if CONNECTED was previously raised
    if (!_mqttConnectedEventRaised) return;
    _mqttConnectedEventRaised = false;

    NetClientEvent event;
    event.type = EventType::MQTT_SERVER_CONNECTION_LOST;
    event._allocatedData = nullptr;

    xQueueSend(_eventQueue, &event, pdMS_TO_TICKS(100));
}

void ChipguyNetClient::queueMqttMessage(const char* topic, const uint8_t* payload, size_t length, bool retained) {
    // Only queue messages if we're in connected state (between CONNECTED and CONNECTION_LOST)
    if (!_mqttConnectedEventRaised) return;

    size_t topicLen = strlen(topic) + 1;
    // Include null terminator in payload copy (payload is already null-terminated from PubSubClient)
    size_t totalSize = topicLen + length + 1;

    // Allocate a single block for topic and payload
    char* block = (char*)malloc(totalSize);
    if (!block) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for MQTT message - starving watchdog", totalSize);
        _starveWatchdog = true;  // Force reboot via watchdog timeout
        return;
    }

    // Copy topic at the start of the block
    char* topicCopy = block;
    memcpy(topicCopy, topic, topicLen);

    // Copy payload (including null terminator) after the topic
    char* payloadCopy = block + topicLen;
    memcpy(payloadCopy, payload, length + 1);

    NetClientEvent event;
    event.type = EventType::MQTT_MESSAGE_RECEIVED;
    event.mqtt_message_received_event.topic = topicCopy;
    event.mqtt_message_received_event.payload = payloadCopy;
    event.mqtt_message_received_event.payloadLength = length;
    event.mqtt_message_received_event.retained = retained;
    event._allocatedData = block;

    xQueueSend(_eventQueue, &event, pdMS_TO_TICKS(100));
}

void ChipguyNetClient::queueStatusColor(StatusColor color) {
    _currentStatus = color;

    NetClientEvent event;
    event.type = EventType::STATUS_COLOR_CHANGE;
    event.status_color_change_event.color = color;
    event._allocatedData = nullptr;

    xQueueSend(_eventQueue, &event, pdMS_TO_TICKS(100));
}

void ChipguyNetClient::updateStatusColor() {
    StatusColor newColor;

    bool networkConnected = _useEthernet ? _ethConnected : _wifiConnected;
    // A disabled broker is "satisfied" (the device isn't trying to use it,
    // so it doesn't hold the status back).
    bool satisfied = !isMqttEnabled() || _mqttConnected;

    if (!networkConnected) {
        newColor = StatusColor::RED;
    } else if (!satisfied) {
        newColor = StatusColor::YELLOW;  // network up, broker is waiting
    } else {
        newColor = StatusColor::GREEN;   // network up, broker connected or disabled
    }

    if (newColor != _currentStatus) {
        queueStatusColor(newColor);
    }
}

void ChipguyNetClient::networkTaskEntry(void* param) {
    ChipguyNetClient* self = static_cast<ChipguyNetClient*>(param);
    self->networkTask();
}

void ChipguyNetClient::networkTask() {
    // Configure and add this task to hardware watchdog
    if (_config.watchdogTimeoutSec > 0) {
        // Reconfigure global TWDT to our timeout
        esp_task_wdt_config_t wdt_config = {
            .timeout_ms = _config.watchdogTimeoutSec * 1000,
            .idle_core_mask = 0,
            .trigger_panic = true
        };
        esp_task_wdt_reconfigure(&wdt_config);

        esp_task_wdt_add(NULL);
        esp_task_wdt_reset();  // Initial feed
    }

    // Setup network interface based on mode
    if (_useEthernet) {
        setupEthernet();
        connectEthernet();  // Non-blocking, events handle connection
    } else {
        setupWiFi();
    }
    // OTA and MQTT setup deferred until network connected when MAC is
    // available. Per-step flags (_macSubstitutionsDone / _otaSetupDone /
    // _mqttSetupDone) gate each setup step independently so updateConfiguration()
    // can reset just the affected step and the next loop iteration picks it up.

    while (_running) {
        // Force-feed (set by feedWatchdog(true) from off-task callers) bypasses
        // the starve flag — used by OTA so a manual recovery can land even on
        // a deliberately-starved device.
        if (_watchdogForceFeedRequested) {
            _watchdogForceFeedRequested = false;
            if (_config.watchdogTimeoutSec > 0) {
                esp_task_wdt_reset();
            }
        }

        // Normal feed request (from broker B or external caller). Inhibited
        // while starved.
        if (_watchdogFeedRequested) {
            _watchdogFeedRequested = false;
            if (_config.watchdogTimeoutSec > 0 && !_starveWatchdog) {
                esp_task_wdt_reset();
            }
        }

        // Wait-for-MQTT-config keepalive: when the broker is not enabled,
        // no MQTT traffic can ever feed the watchdog (the normal heartbeat
        // path is mqttCallback / connect success / republishPersists, all
        // gated on the broker being live). Feed it from here, at the top
        // of every iteration, so a device sitting in updateConfiguration-
        // wait mode doesn't reboot after the watchdog timeout — this fires
        // whether the network is up yet or not, so the device stays alive
        // through the initial WiFi/Ethernet bring-up. Once a server is
        // supplied via updateConfiguration(), the original "no broker
        // traffic = unhealthy" semantics resume and this branch stops
        // firing — a *configured-but-unreachable* broker will still trip
        // the watchdog as intended.
        if (!isMqttEnabled()) {
            if (_config.watchdogTimeoutSec > 0 && !_starveWatchdog) {
                esp_task_wdt_reset();
            }
        }

        // Handle OTA
        if (_otaEnabled) {
            ArduinoOTA.handle();
        }

        // Check network connection based on mode
        bool networkUp = _useEthernet ? _ethConnected : (WiFi.status() == WL_CONNECTED);

        if (!networkUp) {
            // Handle disconnect
            if (_useEthernet) {
                // Ethernet mode: events handle reconnection automatically
                // Just update state if we thought we were connected
                if (_ethConnected) {
                    _ethConnected = false;
                    _mqttConnected = false;
                    queueNetworkConnectionLost();  // Also queues MQTT connection lost
                    updateStatusColor();
                }
            } else {
                // WiFi mode: existing reconnect logic
                if (_wifiConnected) {
                    _wifiConnected = false;
                    _mqttConnected = false;
                    queueNetworkConnectionLost();  // Also queues MQTT connection lost
                    updateStatusColor();
                }

                unsigned long now = millis();
                if (now - _lastWifiConnectAttempt >= _config.wifiReconnectIntervalMs) {
                    _lastWifiConnectAttempt = now;
                    if (connectWiFi()) {
                        _wifiConnected = true;
                        updateStatusColor();
                    }
                }
            }

            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!_useEthernet) {
            _wifiConnected = true;
        }

        // Network is up. Run any deferred setup steps. Each step is gated by
        // its own "done" flag so updateConfiguration() can invalidate just
        // one step (e.g. broker) without disturbing the others.
        if (!_macSubstitutionsDone) {
            performMacSubstitutions();
            _macSubstitutionsDone = true;
        }
        if (!_otaSetupDone) {
            setupOTA();
            _otaSetupDone = true;
        }
        if (isMqttEnabled() && !_mqttSetupDone) {
            // Drop any prior connection (carrying the old server pointer)
            // before re-running setupMqttClient with the new one.
            if (_mqttClient.connected()) {
                _mqttClient.disconnect();
            }
            setupMqttClient();
            _lastMqttConnectAttempt = millis() - _config.mqttReconnectIntervalMs;
            _mqttSetupDone = true;
        }

        // If the broker has been disabled at runtime (server set to null/empty),
        // make sure any prior connection is torn down and the user sees a
        // CONNECTION_LOST event exactly once. Then the connect loop below is
        // skipped so no further attempts happen.
        if (!isMqttEnabled() && _mqttConnected) {
            _mqttConnected = false;
            if (_mqttClient.connected()) {
                _mqttClient.disconnect();
            }
            queueMqttConnectionLost();
            updateStatusColor();
        }

        // Check MQTT connection — only if the broker is enabled. Otherwise
        // skip entirely (no attempts, no failure logs).
        if (isMqttEnabled()) {
            if (!_mqttClient.connected()) {
                if (_mqttConnected) {
                    _mqttConnected = false;
                    queueMqttConnectionLost();
                    updateStatusColor();
                }

                unsigned long now = millis();
                if (now - _lastMqttConnectAttempt >= _config.mqttReconnectIntervalMs) {
                    _lastMqttConnectAttempt = now;
                    if (connectMqtt()) {
                        _mqttConnected = true;
                        updateStatusColor();
                    }
                }
            } else {
                if (!_mqttConnected) {
                    _mqttConnected = true;
                    updateStatusColor();
                }
                _mqttClient.loop();

                // Process queued commands (publish, subscribe, etc.)
                _processCommands();

                // Republish one aged persist entry (if any are due).
                // Per-entry timestamps control the interval; this just scans.
                republishPersists();
            }
        }

        // Send any queued UDP datagrams (network is up at this point)
        _processUdpQueue();

        // Drive the bundled DataPoint / Watcher publish timers. Cheap when
        // nothing is due, and short-circuits when MQTT is disconnected.
        MqttDataPoint::tickAll();
        MqttDataPointWatcher::tickAll();

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // Remove from watchdog before deleting task
    if (_config.watchdogTimeoutSec > 0) {
        esp_task_wdt_delete(NULL);
    }
    vTaskDelete(NULL);
}

void ChipguyNetClient::mqttCallback(const char* topic, const char* payload, unsigned int length) {
    if (_instance) {
        _instance->queueMqttMessage(topic, (const uint8_t*)payload, length,
                                    mqtt_receive_was_retained);
        // Direct watchdog feed - we're in the network task context
        if (_instance->_config.watchdogTimeoutSec > 0 && !_instance->_starveWatchdog) {
            esp_task_wdt_reset();
        }
    }
}
