// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef NETCLIENT_EVENT_H
#define NETCLIENT_EVENT_H

#include <stdint.h>
#include <stddef.h>

enum class StatusColor : uint8_t {
    RED,        // No network / error
    YELLOW,     // Network connected, MQTT connecting
    GREEN,      // Fully connected
    CYAN,       // Initializing
    BLUE        // Reserved (never emitted in current version); kept in the
                // enum so sketches with `case StatusColor::BLUE:` still build.
};

enum class EventType : uint8_t {
    NONE = 0,
    MAC_ADDRESS_AVAILABLE,
    NETWORK_CONNECTED,
    NETWORK_CONNECTION_LOST,
    MQTT_SERVER_CONNECTED,
    MQTT_SERVER_CONNECTION_LOST,
    MQTT_MESSAGE_RECEIVED,
    STATUS_COLOR_CHANGE
};

struct MAC_ADDRESS_AVAILABLE_Event {
    const char* macAddress;  // Points to persistent "XX:XX:XX:XX:XX:XX" buffer
};

struct NETWORK_CONNECTED_Event {
    const char* localIP;     // Points to persistent buffer - safe to hold this pointer
};

struct NETWORK_CONNECTION_LOST_Event {
    // No additional data needed
};

struct MQTT_SERVER_CONNECTED_Event {
    // No additional data needed
};

struct MQTT_SERVER_CONNECTION_LOST_Event {
    // No additional data needed
};

struct MQTT_MESSAGE_RECEIVED_Event {
    const char* topic;       // Null-terminated string
    const char* payload;     // Null-terminated string (payloadLength excludes null)
    size_t payloadLength;
    bool retained;
};

struct STATUS_COLOR_CHANGE_Event {
    StatusColor color;
};

struct NetClientEvent {
    EventType type = EventType::NONE;

    union {
        MAC_ADDRESS_AVAILABLE_Event mac_address_available_event;
        NETWORK_CONNECTED_Event network_connected_event;
        NETWORK_CONNECTION_LOST_Event network_connection_lost_event;
        MQTT_SERVER_CONNECTED_Event mqtt_server_connected_event;
        MQTT_SERVER_CONNECTION_LOST_Event mqtt_server_connection_lost_event;
        MQTT_MESSAGE_RECEIVED_Event mqtt_message_received_event;
        STATUS_COLOR_CHANGE_Event status_color_change_event;
    };

    // Pointer to dynamically allocated data for this event (topic + payload for MQTT messages).
    // Freed when the next event is retrieved via eventAvailable().
    void* _allocatedData = nullptr;

    // Tracks whether this event has been delivered to MqttDataPointWatcher yet.
    // mutable so MqttDataPointWatcher::dispatchEvent (which takes a const ref)
    // can flip it on first call, making the dispatch idempotent.
    mutable bool _watcherDispatched = false;
};

// Helper to get string representation of status values (for debugging)
inline const char* toString(StatusColor color) {
    switch (color) {
        case StatusColor::RED: return "RED";
        case StatusColor::YELLOW: return "YELLOW";
        case StatusColor::GREEN: return "GREEN";
        case StatusColor::CYAN: return "CYAN";
        case StatusColor::BLUE: return "BLUE";
        default: return "UNKNOWN";
    }
}

inline const char* toString(EventType type) {
    switch (type) {
        case EventType::NONE: return "NONE";
        case EventType::MAC_ADDRESS_AVAILABLE: return "MAC_ADDRESS_AVAILABLE";
        case EventType::NETWORK_CONNECTED: return "NETWORK_CONNECTED";
        case EventType::NETWORK_CONNECTION_LOST: return "NETWORK_CONNECTION_LOST";
        case EventType::MQTT_SERVER_CONNECTED: return "MQTT_SERVER_CONNECTED";
        case EventType::MQTT_SERVER_CONNECTION_LOST: return "MQTT_SERVER_CONNECTION_LOST";
        case EventType::MQTT_MESSAGE_RECEIVED: return "MQTT_MESSAGE_RECEIVED";
        case EventType::STATUS_COLOR_CHANGE: return "STATUS_COLOR_CHANGE";
        default: return "UNKNOWN";
    }
}

#endif // NETCLIENT_EVENT_H
