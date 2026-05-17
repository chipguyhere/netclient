// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

// WiFi mode - no special configuration needed
// This header exists for API consistency with Ethernet board headers

#ifndef CHIPGUY_MQTT_NETCLIENT_WIFI_H
#define CHIPGUY_MQTT_NETCLIENT_WIFI_H

#include "chipguy_MQTT_netclient.h"

// No-op for WiFi mode - allows same code to work with WiFi or Ethernet
// by just changing the #include
inline void configureEthernet(NetClientConfig& config) {
    (void)config;  // Unused in WiFi mode
}

#endif
