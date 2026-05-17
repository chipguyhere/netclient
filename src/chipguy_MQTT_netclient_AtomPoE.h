// Board configuration for M5Stack AtomPoE
// W5500 Ethernet via SPI interface
// Works on all ESP32 variants (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)

// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef CHIPGUY_MQTT_NETCLIENT_ATOMPOE_H
#define CHIPGUY_MQTT_NETCLIENT_ATOMPOE_H

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

#ifdef ARDUINO_M5STACK_ATOMS3
    // SPI pins for AtomPoE with AtomS3
    ethConfig.spiCs = 6;
    ethConfig.spiIrq = -1;
    ethConfig.phyRst = -1;
    ethConfig.spiSck = 5;
    ethConfig.spiMiso = 7;
    ethConfig.spiMosi = 8;
    ethConfig.spiFreqMhz = 10;
#else
    // SPI pins for AtomPoE with a classic Atom
    ethConfig.spiCs = 19;
    ethConfig.spiIrq = -1;
    ethConfig.phyRst = -1;
    ethConfig.spiSck = 22;
    ethConfig.spiMiso = 23;
    ethConfig.spiMosi = 33;
    ethConfig.spiFreqMhz = 10;
#endif

    config.ethernet = &ethConfig;
}

#endif
