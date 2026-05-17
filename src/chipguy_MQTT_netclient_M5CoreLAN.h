// Board configuration for M5Stack M5Core with LAN Module (W5500)
// W5500 Ethernet via SPI interface
// Works on all ESP32 variants (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)

// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef CHIPGUY_MQTT_NETCLIENT_M5CORELAN_H
#define CHIPGUY_MQTT_NETCLIENT_M5CORELAN_H

// Signals that Ethernet shares its SPI bus with the on-board display.
// Examples that drive a display on the same SPI bus (e.g. M5GFX on
// M5Stack Core) should trap on this to refuse compilation.
#define CHIPGUY_NETCLIENT_ETHERNET_SHARES_DISPLAY_SPI

// Auto-include main header if not already included
#ifndef CHIPGUY_MQTT_NETCLIENT_H
#include "chipguy_MQTT_netclient.h"
#endif

#include "EthernetConfig.h"

inline void configureEthernet(NetClientConfig& config) {
    static EthernetConfig ethConfig;

    ethConfig.phyType = ETH_PHY_W5500;
    ethConfig.phyAddr = 1;

    // SPI pins for M5Core LAN Module
    ethConfig.spiCs = 26;
    ethConfig.spiIrq = 34;
    ethConfig.phyRst = 13;
    ethConfig.spiSck = 18;
    ethConfig.spiMiso = 19;
    ethConfig.spiMosi = 23;
    ethConfig.spiFreqMhz = 1;  // Low frequency for stability

    config.ethernet = &ethConfig;
}

#endif
