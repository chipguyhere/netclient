// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#include "MqttDataPointWatcher.h"
#include "MqttDataPoint.h"
#include "chipguy_MQTT_netclient.h"
#include <string.h>
#include <stdlib.h>
#include <algorithm>

// ============================================================================
// MqttDataPointWatcher Implementation (NetClient version)
// ============================================================================

// Define static members
std::vector<MqttDataPointWatcher*> MqttDataPointWatcher::allInstances;
UnixTimeCallback MqttDataPointWatcher::unixTimeCallback = nullptr;
bool MqttDataPointWatcher::macReady = false;

MqttDataPointWatcher::MqttDataPointWatcher(const char* topicFormat, ...)
    : currentValue(-1),
      hasCurrentValue(false),
      connectionEstablished(false),
      connectionMillis(0),
      inStartupPhase(false),
      receivedRetainedTopic(false),
      retainedTopicValue(-1)
{
    // Format the main topic. Uses MqttDataPoint::formatTopicV so a leading
    // "%P" passes through verbatim (NetClient expands it at publish time).
    va_list args;
    va_start(args, topicFormat);

    va_list args_copy;
    va_copy(args_copy, args);
    int len = MqttDataPoint::formatTopicV(nullptr, 0, topicFormat, args_copy);
    va_end(args_copy);

    topic = new char[len + 1];
    MqttDataPoint::formatTopicV(topic, len + 1, topicFormat, args);
    va_end(args);

    // Create became_0 and became_1 topics
    size_t topicLen = strlen(topic);
    becameTopic[0] = new char[topicLen + 13];  // "_inactive_at" + null
    becameTopic[1] = new char[topicLen + 11];  // "_active_at" + null

    strcpy(becameTopic[0], topic);
    strcat(becameTopic[0], "_inactive_at");

    strcpy(becameTopic[1], topic);
    strcat(becameTopic[1], "_active_at");

    receivedRetainedBecame[0] = false;
    receivedRetainedBecame[1] = false;
    retainedBecameTime[0] = 0;
    retainedBecameTime[1] = 0;

    pendingBecame[0].pending = false;
    pendingBecame[0].eventMillis = 0;
    pendingBecame[0].publishAttempted = false;

    pendingBecame[1].pending = false;
    pendingBecame[1].eventMillis = 0;
    pendingBecame[1].publishAttempted = false;

    lastBecamePublish[0] = 0;
    lastBecamePublish[1] = 0;
    lastBecameTimestamp[0] = 0;
    lastBecameTimestamp[1] = 0;
    haveBecameTimestamp[0] = false;
    haveBecameTimestamp[1] = false;

    allInstances.push_back(this);

    // If MAC_ADDRESS_AVAILABLE has already fired (this watcher was created
    // late), it won't fire again — register expansion + auto-subscribe right
    // now so the watcher matches incoming MQTT messages.
    if (macReady) {
        refreshTopicExpansions();
        registerAutoSubscriptions();
    }
}

// Constructor that watches a local MqttDataPoint. The watcher still
// auto-subscribes to its topics via NetClient on connect, but the local
// publish callback gives it an immediate, non-retained view of values
// emitted by the data point without waiting for the broker round trip.
MqttDataPointWatcher::MqttDataPointWatcher(MqttDataPoint* dataPoint)
    : currentValue(-1),
      hasCurrentValue(false),
      connectionEstablished(false),
      connectionMillis(0),
      inStartupPhase(false),
      receivedRetainedTopic(false),
      retainedTopicValue(-1)
{
    // Copy the topic from the data point
    const char* srcTopic = dataPoint->getTopic();
    size_t len = strlen(srcTopic);
    topic = new char[len + 1];
    strcpy(topic, srcTopic);

    becameTopic[0] = new char[len + 13];
    becameTopic[1] = new char[len + 11];

    strcpy(becameTopic[0], topic);
    strcat(becameTopic[0], "_inactive_at");

    strcpy(becameTopic[1], topic);
    strcat(becameTopic[1], "_active_at");

    receivedRetainedBecame[0] = false;
    receivedRetainedBecame[1] = false;
    retainedBecameTime[0] = 0;
    retainedBecameTime[1] = 0;

    pendingBecame[0].pending = false;
    pendingBecame[0].eventMillis = 0;
    pendingBecame[0].publishAttempted = false;

    pendingBecame[1].pending = false;
    pendingBecame[1].eventMillis = 0;
    pendingBecame[1].publishAttempted = false;

    lastBecamePublish[0] = 0;
    lastBecamePublish[1] = 0;
    lastBecameTimestamp[0] = 0;
    lastBecameTimestamp[1] = 0;
    haveBecameTimestamp[0] = false;
    haveBecameTimestamp[1] = false;

    allInstances.push_back(this);

    // Same late-construction handling as the other constructor.
    if (macReady) {
        refreshTopicExpansions();
        registerAutoSubscriptions();
    }

    // Register a callback to receive publish notifications from the data
    // point. The data point's attemptPublish() invokes registered callbacks
    // on successful publish attempts. The data point holds the topic in its
    // raw (unexpanded) form, so we expand here before calling feedMessage —
    // feedMessage compares only against the cached expanded form.
    dataPoint->registerPublishCallback([](const char* dpTopic, const char* payload) {
        std::string expanded = NetClient.expandTopic(dpTopic);
        const char* match = expanded.c_str();
        for (auto* watcher : MqttDataPointWatcher::allInstances) {
            if (!watcher->expandedTopic.empty() && watcher->expandedTopic == match) {
                watcher->feedMessage(match, (const byte*)payload, strlen(payload), false);
            }
        }
    });
}

MqttDataPointWatcher::~MqttDataPointWatcher() {
    auto it = std::find(allInstances.begin(), allInstances.end(), this);
    if (it != allInstances.end()) {
        allInstances.erase(it);
    }

    delete[] topic;
    delete[] becameTopic[0];
    delete[] becameTopic[1];
}

void MqttDataPointWatcher::setUnixTimeCallback(UnixTimeCallback callback) {
    unixTimeCallback = callback;
}

void MqttDataPointWatcher::registerAutoSubscriptions() {
    // NetClient.autoSubscribe is idempotent. Called exactly once per watcher,
    // when the MAC and topic prefix are known to be settled.
    NetClient.autoSubscribe(topic);
    NetClient.autoSubscribe(becameTopic[0]);
    NetClient.autoSubscribe(becameTopic[1]);
}

void MqttDataPointWatcher::refreshTopicExpansions() {
    expandedTopic = NetClient.expandTopic(topic);
    expandedBecameTopic[0] = NetClient.expandTopic(becameTopic[0]);
    expandedBecameTopic[1] = NetClient.expandTopic(becameTopic[1]);
}

void MqttDataPointWatcher::dispatchEvent(const NetClientEvent& event) {
    // Idempotent: skip if NetClient (or anyone else) has already dispatched
    // this event. Auto-dispatch from eventAvailable() relies on this so an
    // application that still calls dispatchEvent manually doesn't double-fire.
    if (event._watcherDispatched) return;
    event._watcherDispatched = true;
    switch (event.type) {
        case EventType::MAC_ADDRESS_AVAILABLE: {
            // The MAC just became authoritative; the sketch's own handler ran
            // first and has already applied any updateTopicPrefix(). Compute
            // the (now final) expansions and register subscriptions.
            macReady = true;
            for (auto* w : allInstances) {
                w->refreshTopicExpansions();
                w->registerAutoSubscriptions();
            }
            break;
        }
        case EventType::MQTT_SERVER_CONNECTED: {
            for (auto* w : allInstances) {
                if (!w->connectionEstablished) {
                    w->onConnectionEstablished();
                }
            }
            break;
        }
        case EventType::MQTT_SERVER_CONNECTION_LOST: {
            for (auto* w : allInstances) {
                w->onConnectionLost();
            }
            break;
        }
        case EventType::MQTT_MESSAGE_RECEIVED: {
            const auto& m = event.mqtt_message_received_event;
            for (auto* w : allInstances) {
                w->feedMessage(m.topic, (const byte*)m.payload, m.payloadLength, m.retained);
            }
            break;
        }
        default:
            break;
    }
}

void MqttDataPointWatcher::onConnectionEstablished() {
    connectionEstablished = true;
    connectionMillis = millis();
    inStartupPhase = true;

    // Reset startup phase tracking. We do NOT reset pendingBecame here:
    // an event recorded while disconnected should still be eligible to
    // publish when we come back up.
    receivedRetainedTopic = false;
    receivedRetainedBecame[0] = false;
    receivedRetainedBecame[1] = false;
    retainedTopicValue = -1;
    retainedBecameTime[0] = 0;
    retainedBecameTime[1] = 0;
}

void MqttDataPointWatcher::onConnectionLost() {
    // Mark disconnected so the next MQTT_SERVER_CONNECTED re-enters startup.
    // Pending became events and hourly-republish state are preserved.
    connectionEstablished = false;
    inStartupPhase = false;
}

// Called by both dispatchEvent (MQTT path) and the local DataPoint publish
// callback. Both paths supply messageTopic in its expanded (substituted)
// form, so we only compare against the cached expansion.
void MqttDataPointWatcher::feedMessage(const char* messageTopic, const byte* payload, unsigned int length, bool retained) {
    if (!expandedTopic.empty() && expandedTopic == messageTopic) {
        handleTopicMessage((const char*)payload, length, retained);
    } else if (!expandedBecameTopic[0].empty() && expandedBecameTopic[0] == messageTopic) {
        handleBecameMessage(0, (const char*)payload, length, retained);
    } else if (!expandedBecameTopic[1].empty() && expandedBecameTopic[1] == messageTopic) {
        handleBecameMessage(1, (const char*)payload, length, retained);
    }
}

void MqttDataPointWatcher::handleTopicMessage(const char* payload, unsigned int length, bool retained) {
    // Accept only exact strings "0" or "1" (not "00", "01", etc.)
    if (length != 1) {
        return;
    }

    int value;
    if (payload[0] == '0') {
        value = 0;
    } else if (payload[0] == '1') {
        value = 1;
    } else {
        return;
    }

    if (inStartupPhase && retained) {
        receivedRetainedTopic = true;
        retainedTopicValue = (int)value;
        if (!hasCurrentValue) {
            currentValue = value;
            hasCurrentValue = true;
        }

        // If we've received the retained topic value and both became messages,
        // we can complete the startup phase early.
        if (receivedRetainedTopic && receivedRetainedBecame[0] && receivedRetainedBecame[1]) {
            processStartupPhase();
            inStartupPhase = false;
        }
        Serial.printf("in startup phase and retained. got message for %s: %d\n", topic, value);
    } else if (inStartupPhase && !receivedRetainedTopic) {
        // Non-retained message during the startup window, before any retained
        // topic value has arrived. Capture the current value so we have a state
        // to fall back on at end-of-startup if no retained data ever shows up
        // — but don't record a became event yet; retained data may still be
        // on its way and we'd rather use the broker-side history when it exists.
        if (!hasCurrentValue) {
            currentValue = (int)value;
            hasCurrentValue = true;
            Serial.printf("startup-phase capture (non-retained) for %s: %d\n", topic, value);
        }
    } else if (inStartupPhase == false || receivedRetainedTopic == true) {
        // Either startup phase has ended, or we've already received the retained
        // topic (which always arrives first if it exists).
        if (!hasCurrentValue || currentValue != (int)value) {
            Serial.printf("hascurrentvalue=%s currentvalue=%d newvalue=%d\n", hasCurrentValue ? "yes" : "no", currentValue, value);

            currentValue = (int)value;
            hasCurrentValue = true;
            Serial.printf("recording became event: got message for %s: %d\n", topic, value);

            recordBecameEvent(currentValue);
        } else {
            Serial.printf("ignored for not being a change, got message for %s: %d\n", topic, value);
        }
    }
}

// Handles incoming MQTT message that matches the pattern of
// (topic)_inactive_at or (topic)_active_at
void MqttDataPointWatcher::handleBecameMessage(int value, const char* payload, unsigned int length, bool retained) {
    if (value < 0 || value > 1) return;

    Serial.printf("handled became %d message for %s\n", value, topic);

    unsigned long timestamp;
    if (!parsePayload((const byte*)payload, length, timestamp)) {
        return;
    }

    if (inStartupPhase && retained) {
        receivedRetainedBecame[value] = true;
        retainedBecameTime[value] = timestamp;

        // Store this for hourly re-publishing
        haveBecameTimestamp[value] = true;
        lastBecameTimestamp[value] = timestamp;
        lastBecamePublish[value] = millis();

        if (receivedRetainedTopic && receivedRetainedBecame[0] && receivedRetainedBecame[1]) {
            processStartupPhase();
            inStartupPhase = false;
        }
    }
    // Non-retained became messages, or any became messages outside the startup
    // phase, are intentionally ignored — only this instance should be publishing
    // these for our topic.
}

void MqttDataPointWatcher::processStartupPhase() {
    Serial.printf("Watcher Startup phase is over for %s\n", topic);

    if (!receivedRetainedTopic) {
        if (hasCurrentValue) {
            // Broker has no retained value for this topic, but we already know
            // the current state from a local update during the startup window.
            // Record a became event so dashboards see a "since" timestamp on
            // first run instead of an empty topic.
            Serial.printf("No retained topic for %s; recording became %d from local value\n",
                          topic, currentValue);
            recordBecameEvent(currentValue);
        } else {
            Serial.printf("No retained topic and no current value for %s\n", topic);
        }
        return;
    }

    currentValue = retainedTopicValue;
    hasCurrentValue = true;

    bool haveBecame0 = receivedRetainedBecame[0];
    bool haveBecame1 = receivedRetainedBecame[1];

    if (!haveBecame0 && !haveBecame1) {
        // No became messages at all — assume current state is new.
        Serial.printf("recording %d for %s believing state is new.\n", currentValue, topic);
        recordBecameEvent(currentValue);
        return;
    }

    int mostRecentBecame = -1;

    if (haveBecame0 && haveBecame1) {
        // Both exist; pick the newer one. If they're identical timestamps,
        // pick the one matching the current value.
        if (retainedBecameTime[0] == retainedBecameTime[1]) {
            mostRecentBecame = currentValue;
        } else if (retainedBecameTime[0] > retainedBecameTime[1]) {
            mostRecentBecame = 0;
        } else {
            mostRecentBecame = 1;
        }
    } else if (haveBecame0) {
        mostRecentBecame = 0;
    } else if (haveBecame1) {
        mostRecentBecame = 1;
    }

    if (currentValue != mostRecentBecame) {
        // Mismatch — current value doesn't match the most recent became, so
        // generate a new became event with the current time.
        Serial.printf("recording became %d for %s due to mismatch", currentValue, topic);
        recordBecameEvent(currentValue);
    }
}

void MqttDataPointWatcher::recordBecameEvent(int value) {
    if (value < 0 || value > 1) return;

    Serial.printf("Record became event %d\n", value);

    pendingBecame[value].pending = true;
    pendingBecame[value].eventMillis = millis();
    pendingBecame[value].publishAttempted = false;
}

bool MqttDataPointWatcher::tryPublishBecame(int value) {
    if (value < 0 || value > 1) return false;
    if (!pendingBecame[value].pending) return true;

    uint32_t currentUnixTime = getCurrentUnixTime();
    if (currentUnixTime == 0) {
        return false;  // Time unavailable, will retry later.
    }

    char payload[32];
    ultoa(currentUnixTime, payload, 10);

    Serial.println("Trying to publish became.");
    bool success = NetClient.publish(becameTopic[value], payload, true);  // retained

    if (success) {
        pendingBecame[value].pending = false;

        haveBecameTimestamp[value] = true;
        lastBecameTimestamp[value] = currentUnixTime;
        lastBecamePublish[value] = millis();
    }

    pendingBecame[value].publishAttempted = true;
    return success;
}

uint32_t MqttDataPointWatcher::getCurrentUnixTime() {
    if (unixTimeCallback == nullptr) {
        return 0;
    }
    return unixTimeCallback();
}

bool MqttDataPointWatcher::parsePayload(const byte* payload, unsigned int length, unsigned long& result) {
    if (length == 0 || length > 20) return false;

    char buffer[21];
    memcpy(buffer, payload, length);
    buffer[length] = '\0';

    char* endPtr;
    unsigned long value = strtoul(buffer, &endPtr, 10);

    if (endPtr != buffer + length) {
        return false;
    }

    result = value;
    return true;
}

void MqttDataPointWatcher::checkHourlyRepublish() {
    // Re-publish became messages every hour to prevent retained-message
    // expiration on brokers that age them out. We re-publish with the SAME
    // timestamp (not current time) so the recorded transition time is preserved.
    unsigned long now = millis();

    for (int i = 0; i < 2; i++) {
        if (!haveBecameTimestamp[i]) {
            continue;
        }

        if (now - lastBecamePublish[i] < REPUBLISH_INTERVAL) {
            continue;
        }

        char payload[32];
        ultoa(lastBecameTimestamp[i], payload, 10);

        bool success = NetClient.publish(becameTopic[i], payload, true);  // retained

        if (success) {
            lastBecamePublish[i] = now;
        }
    }
}

void MqttDataPointWatcher::tick() {
    // Check if startup phase has timed out
    if (inStartupPhase) {
        if (millis() - connectionMillis >= STARTUP_WINDOW) {
            processStartupPhase();
            inStartupPhase = false;
        }
    }

    // Try to publish pending became events (only when we believe an MQTT broker
    // is connected). NetClient.publish() always queues, but isMqttConnected()
    // tells us whether queuing is meaningful right now.
    if (NetClient.isMqttConnected()) {
        for (int i = 0; i < 2; i++) {
            if (pendingBecame[i].pending) {
                tryPublishBecame(i);
            }
        }

        checkHourlyRepublish();
    }
}

void MqttDataPointWatcher::tickAll() {
    for (auto* w : allInstances) {
        if (w) {
            w->tick();
        }
    }
}
