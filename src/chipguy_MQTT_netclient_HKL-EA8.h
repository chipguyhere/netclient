// Board configuration for HKL-EA8
// LAN8720 Ethernet with RMII interface
// NOTE: RMII Ethernet requires the original ESP32 (has EMAC peripheral)
//       ESP32-S2, S3, C3, etc. do NOT support RMII - use SPI Ethernet instead
//
// This board has no screen, so it is not compatible with LVGL examples.
// It has 16MB flash, and no PSRAM.
//

// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef CHIPGUY_MQTT_NETCLIENT_HKL_EA8_H
#define CHIPGUY_MQTT_NETCLIENT_HKL_EA8_H
// Note: include guard uses underscore since hyphens aren't valid in identifiers

// Auto-include main header if not already included
#ifndef CHIPGUY_MQTT_NETCLIENT_H
#include "chipguy_MQTT_netclient.h"
#endif

#include "EthernetConfig.h"

#if !CONFIG_IDF_TARGET_ESP32
#error "HKL-EA8 uses ESP32 - please check board type and try again"
#endif

#ifdef BOARD_HAS_PSRAM
#error HKL-EA8 board does not have PSRAM, must disable (conflicts with Ethernet)
#endif

inline void configureEthernet(NetClientConfig& config) {
    static EthernetConfig ethConfig;

    ethConfig.phyType = ETH_PHY_LAN8720;
    ethConfig.phyAddr = 0;
    ethConfig.phyPower = -1;
    ethConfig.phyMdc = 23;
    ethConfig.phyMdio = 18;
    ethConfig.clkMode = ETH_CLOCK_GPIO17_OUT;
    ethConfig.spiCs = -1;  // Not SPI (RMII interface)

    config.ethernet = &ethConfig;
}

#endif
