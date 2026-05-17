// chipguy_MQTT_netclient
// Copyright (c) 2026 chipguyhere
// SPDX-License-Identifier: MIT
// See LICENSE in the project root for the full license text.

#ifndef ETHERNET_CONFIG_H
#define ETHERNET_CONFIG_H

#include <stdint.h>
#include <ETH.h>
#include <SPI.h>

// Function pointer for optional hardware reset (GPIO toggle, PMIC power cycle, etc.)
typedef void (*EthernetResetCallback)();

struct EthernetConfig {
    // PHY Configuration (all interfaces)
    eth_phy_type_t phyType = ETH_PHY_W5500;  // Default to SPI Ethernet (works on all ESP32 variants)
    int8_t phyAddr = 1;
    int8_t phyRst = -1;           // Hardware reset GPIO (-1 = not used)
    int8_t phyPower = -1;         // Power enable GPIO (-1 = not used)

#if CONFIG_IDF_TARGET_ESP32
    // RMII Interface (built-in Ethernet like LAN8720, IP101)
    // Only available on original ESP32 which has the EMAC peripheral
    int8_t phyMdc = 23;
    int8_t phyMdio = 18;
    eth_clock_mode_t clkMode = ETH_CLOCK_GPIO0_IN;
#elif CONFIG_IDF_TARGET_ESP32P4
    // RMII Interface for ESP32-P4 (has EMAC peripheral)
    // Data pins are configured via compile-time macros (ETH_RMII_TX0, etc.)
    int8_t phyMdc = 31;
    int8_t phyMdio = 52;
    eth_clock_mode_t clkMode = EMAC_CLK_EXT_IN;
#endif

    // SPI Interface (W5500, etc.) - works on all ESP32 variants
    int8_t spiCs = -1;            // Chip select (-1 = use RMII on ESP32, invalid on S3/C3)
    int8_t spiIrq = -1;           // Interrupt GPIO (-1 = polling mode)
    int8_t spiSck = -1;           // SPI clock (-1 = use default)
    int8_t spiMiso = -1;          // SPI MISO (-1 = use default)
    int8_t spiMosi = -1;          // SPI MOSI (-1 = use default)
    uint8_t spiFreqMhz = 25;      // SPI frequency in MHz
    SPIClass* spiBus = nullptr;   // SPI bus instance (nullptr = default SPI)
    spi_host_device_t spiHost = SPI2_HOST;  // SPI host peripheral

    // Optional hardware reset callback (called before ETH.begin)
    EthernetResetCallback resetCallback = nullptr;
};

#endif // ETHERNET_CONFIG_H
