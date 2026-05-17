// Board configuration for M5Stack StamPLC PoE Adapter ("StamPLCPoE")
// W5500 Ethernet via SPI interface
// Works on all ESP32 variants (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)
//
// The W5500 shares the SPI bus with the LCD display (MOSI=8, MISO=9, SCK=7).
// LCD uses CS=12, DC=6, RST=3.  W5500 uses CS=11, RST=3 (shared with LCD).
//
// NOTE: M5GFX writes directly to SPI peripheral registers, bypassing the
// ESP-IDF SPI driver's bus locking.  This makes it incompatible with sharing
// an SPI bus with the ETH driver.  If you need both LCD and Ethernet on the
// StamPLC, use a display library that works through the ESP-IDF SPI driver
// (e.g. TFT_eSPI) instead of M5GFX.

// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef CHIPGUY_MQTT_NETCLIENT_STAMPLCPOE_H
#define CHIPGUY_MQTT_NETCLIENT_STAMPLCPOE_H

// Signals that Ethernet shares its SPI bus with the on-board display.
// Examples that drive a display on the same SPI bus (e.g. M5GFX on
// M5Stack Core) should trap on this to refuse compilation.
#define CHIPGUY_NETCLIENT_ETHERNET_SHARES_DISPLAY_SPI

#include <Arduino.h>

// Auto-include main header if not already included
#ifndef CHIPGUY_MQTT_NETCLIENT_H
#include "chipguy_MQTT_netclient.h"
#endif

#include "EthernetConfig.h"

// Reset W5500 (and LCD, which shares GPIO 3) before setup() runs.
// Both chips need a clean reset, but after display.begin() we must avoid
// having drivers do the reset lest the second-initialized driver reset the device of
// the first.  The ETH driver is told phyRst = -1 below so it leaves the pin alone.
struct _StamPLCPoE_EarlyReset {
    _StamPLCPoE_EarlyReset() {
        pinMode(3, OUTPUT);
        digitalWrite(3, LOW);
        delay(50);
        digitalWrite(3, HIGH);
        delay(50);   // W5500 needs up to 50ms after reset for PLL lock
    }
};
static _StamPLCPoE_EarlyReset _stamPLCPoE_earlyReset;

inline void configureEthernet(NetClientConfig& config) {
    static EthernetConfig ethConfig;

    ethConfig.phyType = ETH_PHY_W5500;
    ethConfig.phyAddr = 1;

    // SPI pins for StamPLC PoE (shared bus with LCD display)
    // LCD uses: CS=12, DC=6, RST=3 (shared with W5500 RST)
    ethConfig.spiMosi = 8;
    ethConfig.spiMiso = 9;
    ethConfig.spiSck = 7;
    ethConfig.spiCs = 11;
    ethConfig.phyRst = -1; // wired to 3 but we don't let ETH driver touch RST (shared with LCD)
    ethConfig.spiIrq = -1; // wired to 14 but we observe that receive performance
      // degrades rather badly if not set to -1, symptom: incoming packets delivered late
      // (bug in ESP-IDF? notably M5Stack AtomPoE W5500 doesn't even wire up Int/IRQ)
    ethConfig.spiFreqMhz = 16;

    config.ethernet = &ethConfig;
}

#endif
