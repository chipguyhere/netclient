// Board configuration for Waveshare ESP32-P4-86-Panel-ETH-2RO ("P486Panel")
// Built-in IP101GRI Ethernet with RMII interface on ESP32-P4
//
// Schematic pin mapping:
//   RXD0=GPIO29, RXD1=GPIO30, CRS_DV=GPIO28
//   TXD0=GPIO34, TXD1=GPIO35, TX_EN=GPIO49
//   REF_CLK=GPIO50 (50MHz external clock input)
//   MDC=GPIO31, MDIO=GPIO52, RESET=GPIO51
//
// RMII data pins match the ESP32-P4 Arduino defaults so no macro overrides needed.

// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef CHIPGUY_MQTT_NETCLIENT_P486PANEL_H
#define CHIPGUY_MQTT_NETCLIENT_P486PANEL_H

// Auto-include main header if not already included
#ifndef CHIPGUY_MQTT_NETCLIENT_H
#include "chipguy_MQTT_netclient.h"
#endif

#include "EthernetConfig.h"

#if !CONFIG_IDF_TARGET_ESP32P4
#error "P486Panel requires ESP32-P4. This board uses RMII Ethernet with the ESP32-P4 EMAC peripheral."
#endif

inline void configureEthernet(NetClientConfig& config) {
    static EthernetConfig ethConfig;

    ethConfig.phyType = ETH_PHY_IP101;
    ethConfig.phyAddr = 1;
    ethConfig.phyRst = 51;                  // Hardware reset on GPIO 51
    ethConfig.phyMdc = 31;
    ethConfig.phyMdio = 52;
    ethConfig.clkMode = EMAC_CLK_EXT_IN;    // 50MHz clock from PHY on GPIO 50
    ethConfig.spiCs = -1;                   // Not SPI

    config.ethernet = &ethConfig;
}

#endif
