// Board configuration for M5Stack PoESP32
// Built-in IP101 Ethernet with RMII interface

// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef CHIPGUY_MQTT_NETCLIENT_POESP32_H
#define CHIPGUY_MQTT_NETCLIENT_POESP32_H

// Auto-include main header if not already included
#ifndef CHIPGUY_MQTT_NETCLIENT_H
#include "chipguy_MQTT_netclient.h"
#endif

#include "EthernetConfig.h"

#if !CONFIG_IDF_TARGET_ESP32
#error "PoESP32 uses ESP32 Dev Kit - please check board type and try again"
#endif

#ifdef BOARD_HAS_PSRAM
#error PoESP32 board does not have PSRAM, must disable (conflict with Grove port at GPIO16/17)
#endif

inline void configureEthernet(NetClientConfig& config) {
    static EthernetConfig ethConfig;

    ethConfig.phyType = ETH_PHY_IP101;
    ethConfig.phyAddr = 1;
    ethConfig.phyPower = 5;
    ethConfig.phyMdc = 23;
    ethConfig.phyMdio = 18;
    ethConfig.clkMode = ETH_CLOCK_GPIO0_IN;
    ethConfig.spiCs = -1;  // Not SPI

    config.ethernet = &ethConfig;
}

#endif
