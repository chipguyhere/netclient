// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef NETCLIENT_CONFIG_H
#define NETCLIENT_CONFIG_H

#include <stdint.h>
#include <stddef.h>

struct EthernetConfig;  // Defined in EthernetConfig.h

struct MQTTBrokerConfig {
    const char* server = nullptr;
    uint16_t port = 8883;
    const char* clientId = nullptr;          // %s replaced with MAC address
    const char* username = nullptr;
    const char* password = nullptr;
    const char* caCert = nullptr;
    const char* lastWillTopic = nullptr;     // %s replaced with MAC address
    const char* onlineMessage = "online";    // Published to lastWillTopic on connect
    const char* lastWillMessage = "offline"; // Published by broker if connection lost
    uint8_t lastWillQos = 1;
    bool lastWillRetain = true;
};

struct NetClientConfig {
    // WiFi Configuration
    const char* wifiSSID = nullptr;
    const char* wifiPassword = nullptr;
    const char* wifiUsername = nullptr;      // For WPA2-Enterprise only
    bool useWPA2Enterprise = false;
    bool doWifiScan = true;                  // Scan for best AP with matching SSID

    // ArduinoOTA Configuration
    const char* otaHostname = nullptr;       // %s replaced with MAC address; nullptr = auto
    const char* otaPassword = nullptr;       // nullptr = OTA disabled

    // Topic Prefix - use %P in topics to substitute this prefix (after MAC expansion)
    const char* topicPrefix = nullptr;       // e.g., "devices/%X" → "devices/AABBCCDDEEFF"

    // MQTT Broker. brokerA is a legacy alias kept so sketches written
    // against the pre-rename API still compile; both names refer to the
    // same storage. New code should prefer `mqtt`.
    union {
        MQTTBrokerConfig mqtt;
        MQTTBrokerConfig brokerA;
    };

    // MQTT buffer size for incoming messages (0 = use default 1024)
    size_t mqttBufferSize = 0;

    // Watchdog Configuration
    uint32_t watchdogTimeoutSec = 120;        // Hardware watchdog timeout (0 = disabled)

    // Reconnection timing
    uint32_t mqttReconnectIntervalMs = 20000; // Time between MQTT reconnect attempts
    uint32_t wifiReconnectIntervalMs = 5000;  // Time between WiFi reconnect attempts

    // Persist republish interval
    uint32_t persistIntervalMs = 50000;       // Time between persist republishes (default 50s)

    // Ethernet Configuration (optional - mutually exclusive with WiFi)
    // If ethernet is non-null, WiFi settings are ignored
    struct EthernetConfig* ethernet = nullptr;

    // Explicit default constructor is required because the anonymous union
    // above contains a member (MQTTBrokerConfig) with default-member
    // initializers, which suppresses the implicit default constructor.
    // We initialize the `mqtt` arm of the union; `brokerA` shares the
    // same storage and gets the same defaults.
    NetClientConfig() : mqtt{} {}
};

#endif // NETCLIENT_CONFIG_H
