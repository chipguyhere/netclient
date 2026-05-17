// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef MQTT_DATAPOINT_WATCHER_H
#define MQTT_DATAPOINT_WATCHER_H

// ============================================================================
// MqttDataPointWatcher: timestamped transition history for a binary topic.
// ============================================================================
//
// A Watcher attaches to an MQTT topic whose payload is "0" or "1" — typically
// a binary-state DataPoint (door open/closed, motion detected, etc.) — and
// publishes two retained companion topics whenever the value transitions:
//
//     <topic>_active_at      Unix timestamp of the most recent 0 -> 1
//     <topic>_inactive_at    Unix timestamp of the most recent 1 -> 0
//
// This gives consumers a "since when" view of state — "this door has been
// open since 14:32" — without making them subscribe and accumulate history
// themselves. Both companion topics are retained, so subscribers see the
// last transitions immediately on connect.
//
// Two constructors:
//
//   MqttDataPointWatcher(const char* topicFormat, ...)
//     Watches an arbitrary MQTT topic. Auto-subscribes to the topic and its
//     two _at companions on every MQTT connect. Topic format follows the
//     same "%P preserved verbatim" rule as MqttDataPoint constructors.
//
//   MqttDataPointWatcher(MqttDataPoint*)
//     Watches a local MqttStringDataPoint. Same MQTT subscriptions as the
//     other form, but additionally registers as a publish callback on the
//     DataPoint, so local value changes are observed immediately without
//     waiting for the broker round-trip.
//
// Behavior on each MQTT (re)connect:
//
//   - Auto-subscribes; the broker delivers any existing retained values for
//     the topic and its two _at companions.
//   - For ~10s after connect, collects those retained values before deciding
//     whether the current state represents a *new* transition or just a
//     continuation. This avoids clobbering historical _at timestamps when
//     a routine reboot lands on the same state.
//   - If after the window the broker had no retained _at values but we know
//     the current state (from a local DataPoint or a fresh publish), records
//     a became event for the current state — so dashboards have a "since"
//     timestamp on first run against a fresh broker.
//
// Other features:
//
//   - Hourly re-publish of the most recent timestamps using the *same*
//     timestamp value (not "now"), defeating retained-message expiration
//     while preserving the original transition time.
//   - Pending transitions queued during MQTT disconnect are preserved across
//     reconnect; the timestamp is captured at the time of the event, not the
//     time of publish.
//
// Time source:
//
//   The Watcher needs a Unix-time provider. Set one once in setup():
//       MqttDataPointWatcher::setUnixTimeCallback(myUnixTimeFn);
//   Until the callback returns non-zero, became events stay queued and are
//   published as soon as time becomes available.
//
// ----------------------------------------------------------------------------
// Implementation notes — skip on first read.
//
//   Integration with NetClient:
//     - Publishes via NetClient and auto-subscribes through
//       NetClient.autoSubscribe — no direct PubSubClient use.
//     - Events are dispatched automatically from inside
//       NetClient::eventAvailable(), once the application has had its chance
//       to handle the prior event. That ordering is what lets the
//       application's MAC_ADDRESS_AVAILABLE handler call
//       NetClient.updateTopicPrefix() *before* the watcher caches its topic
//       expansions. dispatchEvent() remains public for legacy code and is
//       idempotent if invoked.
//     - tickAll() is driven internally by NetClient's network task; tick()
//       is also internal. Applications should not call either.
// ----------------------------------------------------------------------------

#include <Arduino.h>
#include <stdarg.h>
#include <string>
#include <vector>
#include "NetClientEvent.h"

// Forward declaration
class MqttDataPoint;

// Callback function type for getting Unix time
// Should return Unix timestamp (seconds since epoch) or 0 if time is unknown
typedef uint32_t (*UnixTimeCallback)();

class MqttDataPointWatcher {
private:
    // Static list of all watcher instances
    static std::vector<MqttDataPointWatcher*> allInstances;

    // Static Unix time callback (shared by all instances)
    static UnixTimeCallback unixTimeCallback;

    // Set true when MAC_ADDRESS_AVAILABLE has been dispatched. After that
    // moment, NetClient.expandTopic() and NetClient.autoSubscribe() are safe
    // to call with the final MAC and topic prefix; before it, watchers defer
    // both until dispatchEvent() reaches them.
    static bool macReady;

    char* topic;                    // Main topic to watch (e.g., "sensor/value")
    char* becameTopic[2];           // "topic_inactive_at" and "topic_active_at"

    // Cached %P/%X-expanded forms of topic and becameTopic[0..1]. Computed
    // exactly once, when MAC_ADDRESS_AVAILABLE is dispatched (or in the
    // constructor for watchers created after that point). Used to match
    // against the topics NetClient delivers from MQTT, which arrive in their
    // fully-substituted form.
    std::string expandedTopic;
    std::string expandedBecameTopic[2];

    // Current state if we know it. The state we'll post a "Became" message upon leaving.
    int currentValue;               // -1 = unknown, 0, 1
    bool hasCurrentValue;

    // Connection tracking
    bool connectionEstablished;
    unsigned long connectionMillis;
    static const unsigned long STARTUP_WINDOW = 10000;  // 10 seconds

    // Startup retained message collection
    bool inStartupPhase;
    bool receivedRetainedTopic;
    bool receivedRetainedBecame[2];
    int retainedTopicValue;         // -1 = not received, 0, 1
    unsigned long retainedBecameTime[2];  // Unix timestamps from became messages

    // Pending publishes (queue up to one of each)
    struct PendingBecame {
        bool pending;
        unsigned long eventMillis;  // When the event occurred (millis())
        bool publishAttempted;      // Have we tried to publish this yet?
    };
    PendingBecame pendingBecame[2]; // For values 0 and 1

    // Hourly re-publish tracking (to prevent retained message expiration)
    unsigned long lastBecamePublish[2];   // millis() when we last published each became message
    uint32_t lastBecameTimestamp[2];      // Unix timestamp value for each became message
    bool haveBecameTimestamp[2];          // Do we have a valid timestamp to re-publish?
    static const unsigned long REPUBLISH_INTERVAL = 3600000;  // 1 hour

public:
    // Checklist if things are not working:
    // - Did we setUnixTimeCallback() with a callback that returns a real time?
    // - Are we calling MqttDataPointWatcher::dispatchEvent(event) on every event from the NetClient loop?
    // - Are we calling MqttDataPointWatcher::tickAll() (or tick() per instance) regularly from loop()?

    // Constructor with printf-style topic formatting
    MqttDataPointWatcher(const char* topicFormat, ...) __attribute__((format(printf, 2, 3)));

    // Constructor that watches a MqttDataPoint (receives local publish callbacks
    // in addition to MQTT-delivered messages).
    MqttDataPointWatcher(MqttDataPoint* dataPoint);

    ~MqttDataPointWatcher();

    // Set callback function to get Unix time (returns 0 if time unknown).
    // Static, shared by all instances.
    static void setUnixTimeCallback(UnixTimeCallback callback);

    // Dispatch a NetClient event to all watchers. Call this from the main
    // loop with each event drained from NetClient.getEvent(). Recognized
    // event types:
    //   MAC_ADDRESS_AVAILABLE       -> compute expansions + auto-subscribe
    //   MQTT_SERVER_CONNECTED       -> enter startup phase to collect retained
    //   MQTT_SERVER_CONNECTION_LOST -> mark disconnected
    //   MQTT_MESSAGE_RECEIVED       -> route to feedMessage() on each watcher
    // Other event types are ignored. It's safe to dispatch every event.
    //
    // Assumption: the MAC and topic prefix are settled by the time
    // MAC_ADDRESS_AVAILABLE has fired and the user's handler has run.
    // Changing the topic prefix later silently breaks topic matching.
    static void dispatchEvent(const NetClientEvent& event);

    // Drive pending publishes and the hourly republish on a single watcher.
    void tick();

    // Drive pending publishes and the hourly republish on all watchers.
    static void tickAll();

    // Topic accessors (e.g. for diagnostics)
    const char* getTopic() const { return topic; }
    const char* getBecame0Topic() const { return becameTopic[0]; }
    const char* getBecame1Topic() const { return becameTopic[1]; }

    // Static method to get all watcher instances
    static const std::vector<MqttDataPointWatcher*>& getAllInstances() { return allInstances; }

private:
    // Called when MQTT (re)connects after a prior disconnect. Enters the
    // startup phase to collect retained messages.
    void onConnectionEstablished();

    // Called when all known MQTT connections drop.
    void onConnectionLost();

    // Subscribe via NetClient.autoSubscribe() to our three topics. Idempotent.
    void registerAutoSubscriptions();

    // Refill expandedTopic and expandedBecameTopic[] from NetClient.
    void refreshTopicExpansions();

    // Process an incoming MQTT message. Filters by topic; safe to call with
    // any topic (non-matching messages are silently ignored).
    void feedMessage(const char* messageTopic, const byte* payload, unsigned int length, bool retained);

    // Process incoming message for main topic
    void handleTopicMessage(const char* payload, unsigned int length, bool retained);

    // Process incoming message for became_0 or became_1 topic
    void handleBecameMessage(int value, const char* payload, unsigned int length, bool retained);

    // Called after startup phase ends to process collected retained messages
    void processStartupPhase();

    // Record a became event (queues for publishing)
    void recordBecameEvent(int value);

    // Try to publish a pending became event via NetClient
    bool tryPublishBecame(int value);

    // Check and handle hourly re-publishing (republishes with same timestamp)
    void checkHourlyRepublish();

    // Get current Unix time from callback (returns 0 if unavailable)
    uint32_t getCurrentUnixTime();

    // Parse payload as integer (0 or 1 for topic, Unix timestamp for became messages)
    bool parsePayload(const byte* payload, unsigned int length, unsigned long& result);
};

#endif // MQTT_DATAPOINT_WATCHER_H
