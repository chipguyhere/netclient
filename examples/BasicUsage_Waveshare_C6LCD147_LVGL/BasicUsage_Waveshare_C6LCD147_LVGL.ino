/*
 * BasicUsage_Waveshare_C6LCD147_LVGL.ino - Example sketch for chipguy_MQTT_netclient library
 *
 * Target: Waveshare ESP32-C6-LCD-1.47 (ST7789, 172x320)
 *
 * This example demonstrates:
 * - Configuring WiFi with an MQTT broker connection
 * - Polling for events in loop()
 * - Handling different event types
 * - Publishing messages
 * - Subscribing to topics
 * - Starting a minimal UI using LVGL
 *
 * This variant uses minimal ESP-IDF SPI LCD drivers (built-in to lv_setup.hpp)
 * instead of a third-party graphics library.  This routes all LCD SPI traffic
 * through spi_device_polling_transmit(), so the ESP-IDF driver serializes bus
 * access between the LCD and any other SPI devices automatically.
 *
 * IT IS VERY LIKELY THAT THE PARTITION TYPE NEEDS TO BE CHANGED TO "Minimal SPIFFS"
 * (OR ANOTHER BETTER SELECTION OF YOUR CHOICE) IF COMPILING RUNS OUT OF SPACE
 * (error message: "text section exceeds available space in board")
 *
 * The library handles all network and MQTT connection management in a
 * background thread, delivering events via a thread-safe queue.
 */

#include <chipguy_MQTT_netclient_WiFi.h>

#include "lv_setup.hpp"
#include "src/ui.h"

// Credentials - edit creds.hpp with your WiFi, MQTT, and certificate settings
#include "creds.hpp"


// Track connection state for application logic
bool networkConnected = false;
bool mqttConnected = false;

void updateOnscreenFlags() {
    if (!networkConnected && !mqttConnected) {
        lv_label_set_text(ui_lblBottomRight, "NO NETWORK\nNO MQTT");
        lv_obj_remove_flag(ui_lblBottomRight, LV_OBJ_FLAG_HIDDEN);
    } else if (!networkConnected) {
        lv_label_set_text(ui_lblBottomRight, "NO NETWORK");
        lv_obj_remove_flag(ui_lblBottomRight, LV_OBJ_FLAG_HIDDEN);
    } else if (!mqttConnected) {
        lv_label_set_text(ui_lblBottomRight, "NO MQTT");
        lv_obj_remove_flag(ui_lblBottomRight, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui_lblBottomRight, LV_OBJ_FLAG_HIDDEN);
    }
}

uint32_t topLabelCount = 0;
uint32_t seenCount = 0;

void updateTopLabel() {
    char buf[128];
    snprintf(buf, sizeof(buf), "Sent: %lu  Seen: %lu\nMAC: %s\nIP: %s\nRAM: %u",
             (unsigned long)topLabelCount,
             (unsigned long)seenCount,
             NetClient.getMacAddress(),
             NetClient.getLocalIP(),
             ESP.getFreeHeap());
    lv_label_set_text(ui_lblTopLeft, buf);
}

void setup() {
    Serial.begin(115200);

    lv_setup.begin();
    ui_init();
    updateOnscreenFlags();

    Serial.println("\n\nchipguy_MQTT_netclient Waveshare C6 LCD 1.47 LVGL Example");
    Serial.println("Waveshare ESP32-C6-LCD-1.47");
    Serial.println("=============================================\n");

    NetClientConfig config;

    // Configure WiFi
    config.wifiSSID = WIFI_SSID;
    config.wifiPassword = WIFI_PASSWORD;

    // Configure how this client will appear to the network and the MQTT server
    config.otaHostname = "esp32-%6X";  // %X = 12 hex chars of MAC
    config.mqtt.clientId = "esp32-%X";

    // Configure ArduinoOTA (disabled by default - set OTA_PASSWORD to enable)
    config.otaPassword = OTA_PASSWORD;

    // Topic prefix - use %P in topics to substitute this (e.g., "%P/status")
    config.topicPrefix = "devices/%X";  // %X = 12 hex chars of MAC

    // Configure MQTT Broker
    config.mqtt.server = MQTT_SERVER;
    config.mqtt.port = 8883;  // This library requires TLS
    config.mqtt.username = MQTT_USER;
    config.mqtt.password = MQTT_PASSWORD;
    config.mqtt.caCert = CA_CERT;  // nullptr to skip certificate verification
    config.mqtt.lastWillTopic = "%P/status";
    config.mqtt.onlineMessage = "online";
    config.mqtt.lastWillMessage = "offline";

    // Start the network client
    if (!NetClient.begin(config)) {
        Serial.println("Failed to start NetClient!");
        while (1) { delay(1000); }
    }

    Serial.printf("NetClient started successfully (%s mode)\n",
                  NetClient.isEthernet() ? "Ethernet" : "WiFi");

    updateTopLabel();
}


void loop() {

    // 64-bit MQTT connected-time tracking (reset on each reconnection)
    static unsigned long lastConnectedMillis = 0;
    static uint64_t connectedMs = 0;
    static unsigned long lastConnectedPublish = 0;

    while (NetClient.eventAvailable()) {

        NetClientEvent client_event = NetClient.getEvent();
        switch (client_event.type) {

            case EventType::MAC_ADDRESS_AVAILABLE: {
                const auto& event = client_event.mac_address_available_event;
                Serial.printf("[Network] MAC address: %s\n", event.macAddress);
                NetClient.persist("%P/macaddress", NetClient.getMacAddress());
                updateTopLabel();
                break;
            }

            case EventType::NETWORK_CONNECTED: {
                const auto& event = client_event.network_connected_event;
                Serial.printf("[Network] Connected via %s, IP: %s, MAC: %s\n",
                              NetClient.isEthernet() ? "Ethernet" : "WiFi",
                              event.localIP, NetClient.getMacAddress());
                networkConnected = true;
                updateOnscreenFlags();
                NetClient.persist("%P/ipaddress", NetClient.getLocalIP());
                break;
            }

            case EventType::MQTT_SERVER_CONNECTED: {
                const auto& event = client_event.mqtt_server_connected_event;
                Serial.println("[MQTT] Connected");

                mqttConnected = true;

                lastConnectedMillis = millis();
                connectedMs = 0;
                lastConnectedPublish = 0;

                updateOnscreenFlags();
                Serial.println("Setting up subscriptions...");
                NetClient.autoSubscribe("%P/command");
                NetClient.autoSubscribe("broadcast/#");
                NetClient.autoSubscribe("%P/counter");
                NetClient.autoSubscribe("unix_time/unix_time");

                break;
            }

            case EventType::MQTT_MESSAGE_RECEIVED: {
                const auto& event = client_event.mqtt_message_received_event;
                Serial.printf("[MQTT] Message on '%s'%s: %s\n",
                              event.topic,
                              event.retained ? " [retained]" : "",
                              event.payload);

                if (String(event.topic).endsWith("/counter")) {
                    seenCount = strtoul(event.payload, NULL, 10);
                }

                if (String(event.topic).endsWith("/command")) {
                    Serial.printf("Received command: %s\n", event.payload);

                    if (event.retained==false) {
                        if (strcmp(event.payload, "restart") == 0) {
                            Serial.println("Restarting...");
                            ESP.restart();
                        } else if (strcmp(event.payload, "status") == 0) {
                            publishSensorData();
                        }
                    }

                }
                updateOnscreenFlags();
                break;
            }

            case EventType::MQTT_SERVER_CONNECTION_LOST: {
                const auto& event = client_event.mqtt_server_connection_lost_event;
                Serial.println("[MQTT] Connection lost");
                mqttConnected = false;
                updateOnscreenFlags();
                break;
            }

            case EventType::NETWORK_CONNECTION_LOST: {
                Serial.println("[Network] Connection lost");
                networkConnected = false;
                updateOnscreenFlags();
                break;
            }

            case EventType::STATUS_COLOR_CHANGE: {
                const auto& event = client_event.status_color_change_event;
                Serial.printf("[Status] Color: %s\n", toString(event.color));
                break;
            }

            default:
                break;
        }

        updateTopLabel();
    }

    // Update 64-bit uptime counter
    static unsigned long lastUptimeMillis = 0;
    static uint64_t uptimeMs = 0;
    static unsigned long lastUptimePublish = 0;
    unsigned long now = millis();
    uptimeMs += (unsigned long)(now - lastUptimeMillis);
    lastUptimeMillis = now;

    if (mqttConnected) {
        publishSensorData();

        connectedMs += (unsigned long)(now - lastConnectedMillis);
        lastConnectedMillis = now;

        if (now - lastUptimePublish >= 20000) {
            lastUptimePublish = now;
            lastConnectedPublish = now;
            NetClient.publishf("%P/uptime-seconds", "%llu", uptimeMs / 1000);
            NetClient.publishf("%P/connected-seconds", "%llu", connectedMs / 1000);
        }
    }

    // UI logic — keep LVGL running
    lv_timer_handler();
}

void publishSensorData() {

    static unsigned long lastCountTime = 0;
    static unsigned long lastSensorTime = 0;
    unsigned long now = millis();

    if (now - lastCountTime >= 1000) {
        lastCountTime = now;
        topLabelCount++;
        updateTopLabel();
        NetClient.persistf("%P/counter", "%lu", topLabelCount);
    }

    if (now - lastSensorTime >= 5000) {
        lastSensorTime = now;
        char payload[128];

        if (NetClient.isEthernet()) {
            snprintf(payload, sizeof(payload),
                     "{\"uptime\":%lu,\"heap\":%u,\"link\":%d}",
                     millis() / 1000,
                     ESP.getFreeHeap(),
                     NetClient.getEthernetLinkSpeed());
        } else {
            snprintf(payload, sizeof(payload),
                     "{\"uptime\":%lu,\"heap\":%u,\"rssi\":%d}",
                     millis() / 1000,
                     ESP.getFreeHeap(),
                     NetClient.getWiFiRSSI());
        }

        NetClient.persist("%P/stats", payload);
        Serial.printf("Persisted: %s\n", payload);
    }

}
