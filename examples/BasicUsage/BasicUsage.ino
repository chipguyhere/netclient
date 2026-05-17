/*
 * BasicUsage.ino - Example sketch for chipguy_MQTT_netclient library
 *
 * This example demonstrates:
 * - Configuring WiFi or Ethernet with an MQTT broker connection
 * - Polling for events in loop()
 * - Handling different event types
 * - Publishing messages
 * - Subscribing to topics
 *
 * The library handles all network and MQTT connection management in a
 * background thread, delivering events via a thread-safe queue.
 *
 * NETWORK MODE:
 * This example defaults to WiFi. To use Ethernet instead, just replace the
 * #include below with your board's Ethernet header:
 *      #include <chipguy_MQTT_netclient_PoESP32.h>   // M5Stack PoESP32 (IP101 RMII)
 *      #include <chipguy_MQTT_netclient_AtomPoE.h>   // M5Stack AtomPoE (W5500 SPI)
 *      #include <chipguy_MQTT_netclient_M5CoreLAN.h> // M5Stack M5Core + LAN Module (W5500 SPI)
 *      #include <chipguy_MQTT_netclient_P486Panel.h>  // Waveshare ESP32-P4-86-Panel-ETH-2RO (IP101 RMII)
 *      #include <chipguy_MQTT_netclient_StamPLCPoE.h> // M5Stack StamPLC PoE Adapter (W5500 SPI)
 *      #include <chipguy_MQTT_netclient_HKL-EA8.h>    // Hankerila HKL-EA8 PLC (LAN8720 RMII)
 */

#include <chipguy_MQTT_netclient_WiFi.h>

// Credentials - edit creds.hpp with your WiFi, MQTT, and certificate settings
#include "creds.hpp"

// Track connection state for application logic
bool mqttConnected = false;

void setup() {
    Serial.begin(115200);
    Serial.println("\n\nchipguy_MQTT_netclient Basic Usage Example");
    Serial.println("=============================================\n");

    NetClientConfig config;

    // Configure Ethernet (only has effect when an Ethernet header is included)
    configureEthernet(config);

    // Configure WiFi (ignored when Ethernet is configured)
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

    // Optional: Adjust timing settings
    // config.watchdogTimeoutSec = 60;        // Default 60 second watchdog
    // config.mqttReconnectIntervalMs = 20000; // Default 20 seconds

    // Start the network client
    if (!NetClient.begin(config)) {
        Serial.println("Failed to start NetClient!");
        while (1) { delay(1000); }
    }

    Serial.printf("NetClient started successfully (%s mode)\n",
                  NetClient.isEthernet() ? "Ethernet" : "WiFi");
}


void loop() {

    // 64-bit MQTT connected-time tracking (reset on each reconnection)
    static unsigned long lastConnectedMillis = 0;
    static uint64_t connectedMs = 0;
    static unsigned long lastConnectedPublish = 0;

    // In the event loop, we poll NetClient to see if there are any
    // new events available to tell us something happened.
    // Note: The library will reboot the device if too many events come
    // in that have not been picked up in your loop.

    while (NetClient.eventAvailable()) {

        // Grab the event from the library and decide what kind of event it is.
        NetClientEvent client_event = NetClient.getEvent();
        switch (client_event.type) {

            // MAC_ADDRESS_AVAILABLE fires once, after the MAC address has been
            // confirmed from the actual network interface (WiFi or Ethernet).
            // On most ESP32 variants this matches the eFuse MAC, but on boards
            // with a WiFi co-processor (e.g. ESP32-P4) it may differ.
            case EventType::MAC_ADDRESS_AVAILABLE: {
                const auto& event = client_event.mac_address_available_event;
                Serial.printf("[Network] MAC address: %s\n", event.macAddress);
                if (NetClient.getMacAddress64() == 0x123456789abc) {
                    // Example of where the topic prefix can be updated
                    // based on the MAC address, in time for it to influence
                    // the last will and other messages.
                    // So for example, instead of "esp32-123456789abc"
                    // it can be "application" or "application/instance"
                    // NetClient.updateTopicPrefix("powercycler/LivingRoomTV");
                }
                
                // Send the MAC address to the MQTT server upon connection
                NetClient.persist("%P/macaddress", NetClient.getMacAddress());
                break;
            }

            // We get a NETWORK_CONNECTED to report that we've successfully
            // joined a network and been assigned an IP address.
            case EventType::NETWORK_CONNECTED: {
                const auto& event = client_event.network_connected_event;
                Serial.printf("[Network] Connected via %s, IP: %s, MAC: %s\n",
                              NetClient.isEthernet() ? "Ethernet" : "WiFi",
                              event.localIP, NetClient.getMacAddress());
                if (NetClient.isEthernet()) {
                    Serial.printf("[Network] Link speed: %d Mbps\n",
                                  NetClient.getEthernetLinkSpeed());
                }
                // Send the local IP address to the MQTT server upon connection
                NetClient.persist("%P/ipaddress", NetClient.getLocalIP());
                break;
            }

            // We get a MQTT_SERVER_CONNECTED event to report that we are now
            // connected to MQTT.  This is the time to either subscribe or autosubscribe.
            // Autosubscribe means we will subscribe to the topic automatically upon
            // connection and can be done even before connection.  NetClient ignores
            // autosubscribes if the topic is duplicate, but here's a reminder:
            // messages that match more than one unique subscription (due to wildcards)
            // will get sent by the server multiple times.
            case EventType::MQTT_SERVER_CONNECTED: {
                const auto& event = client_event.mqtt_server_connected_event;
                Serial.println("[MQTT] Connected");

                mqttConnected = true;

                // Reset connected-time counter on each (re)connection
                lastConnectedMillis = millis();
                connectedMs = 0;
                lastConnectedPublish = 0;

                Serial.println("Setting up subscriptions...");
                // autoSubscribe remembers topics and resubscribes on reconnect.
                // These subscriptions are examples and probably should be
                // removed for your real application
                NetClient.autoSubscribe("%P/command");
                NetClient.autoSubscribe("%P/counter");

                // Subscribe to our own sensor topic - receiving our own publications
                // keeps the hardware watchdog fed (%P expands topicPrefix)
                NetClient.autoSubscribe("%P/stats");

                // Note: onlineMessage is automatically persisted to lastWillTopic.
                // Set onlineMessage to nullptr to disable this behavior.

                // Let's take this clock, if it's present
                NetClient.autoSubscribe("unix_time/unix_time");

                break;
            }

            // We get a MQTT_MESSAGE_RECEIVED each time we receive a message from
            // an MQTT server.  The "retained" flag tells us whether the message is
            // fresh, or saved from prior to when we connected.

            // WATCHDOG TIMER:  NetClient will reboot the device if no messages are
            // received for 60 seconds, so either subscribe to something (like a clock)
            // that produces frequent messages (or send periodic messages to ourselves).
            // The payload can be binary or text.  A null terminator is automatically
            // added for text convenience.  The lifetime of the topic and payload buffer
            // is until you check NetClient.available() for the next message.  The
            // memory gets reused at that time, so if you need the payload for later,
            // avoid a crash and make a copy of the needed information.

            case EventType::MQTT_MESSAGE_RECEIVED: {
                const auto& event = client_event.mqtt_message_received_event;
                Serial.printf("[MQTT] Message on '%s'%s: %s\n",
                              event.topic,
                              event.retained ? " [retained]" : "",
                              event.payload);

                // Example of how to process incoming command messages from MQTT.
                // Not taking commands?  Then this portion can be deleted.
                // Reminder, incoming messages only arrive if subscribed to.

                // Process messages on topics ending with "/command"
                auto topic = String(event.topic);
                if (topic.endsWith("/command")) {
                    Serial.printf("Received command: %s\n", event.payload);

                    // A restart command would only make sense if it was fresh
                    if (event.retained==false) {   // and not retained from earlier.
                        if (strcmp(event.payload, "restart") == 0) {
                            Serial.println("Restarting...");
                            ESP.restart();
                        } else if (strcmp(event.payload, "status") == 0) {
                            publishSensorData();
                        }
                    }

                }
                break;
            }

            // We get MQTT_SERVER_CONNECTION_LOST if we lose contact with the MQTT
            // server, and a NETWORK_CONNECTION_LOST might come next if that was the
            // root cause.

            case EventType::MQTT_SERVER_CONNECTION_LOST: {
                const auto& event = client_event.mqtt_server_connection_lost_event;
                Serial.println("[MQTT] Connection lost");
                mqttConnected = false;
                break;

                // Reminder: reconnection efforts to MQTT are automatic, you don't need
                // to do anything here, this event is informative only.

            }

            // NETWORK_CONNECTION_LOST refers to a lost WiFi connection, but may
            // also be reported if an Ethernet adapter reports losing link.
            case EventType::NETWORK_CONNECTION_LOST: {
                Serial.println("[Network] Connection lost");
                break;

                // Reminder: reconnection efforts to WiFi are automatic, you don't need
                // to do anything here, this event is informative only.

            }


            // Status color is offered as an optional way to display network connection
            // progress on a device that has a single LED (or this can be used to color
            // an on-screen indicator).  Since we don't know what device is being used,
            // these are placeholders you can implement.
            case EventType::STATUS_COLOR_CHANGE: {
                const auto& event = client_event.status_color_change_event;
                Serial.printf("[Status] Color: %s\n", toString(event.color));
                switch (event.color) {
                    case StatusColor::CYAN:
                        // set LED to CYAN to indicate initializing
                        break;
                    case StatusColor::RED:
                        // set LED to RED to indicate no network connection (yet)
                        break;
                    case StatusColor::YELLOW:
                        // set LED to YELLOW to indicate network up, MQTT connecting
                        break;
                    case StatusColor::GREEN:
                        // set LED to GREEN to indicate MQTT connected
                        break;
                }
                break;
            }

            default:
                break;
        }
    }

    // Update 64-bit uptime counter (unsigned subtraction handles millis() rollover)
    static unsigned long lastUptimeMillis = 0;
    static uint64_t uptimeMs = 0;
    static unsigned long lastUptimePublish = 0;
    unsigned long now = millis();
    uptimeMs += (unsigned long)(now - lastUptimeMillis);
    lastUptimeMillis = now;

    // Application logic - publish sensor data when connected.
    if (mqttConnected) {
        publishSensorData();

        // Update 64-bit connected-time counter
        connectedMs += (unsigned long)(now - lastConnectedMillis);
        lastConnectedMillis = now;

        // Publish uptime and connected-time every 20 seconds
        if (now - lastUptimePublish >= 20000) {
            lastUptimePublish = now;
            lastConnectedPublish = now;
            NetClient.publishf("%P/uptime-seconds", "%llu", uptimeMs / 1000);
            NetClient.publishf("%P/connected-seconds", "%llu", connectedMs / 1000);
        }
    }

    // UI logic -- this would be a great place to put LVGL's tick, or whatever
    // else keeps any screen updated user interface running.

    // By the way, NetClient's methods (such as subscribe, publish, persist, etc.)
    // return as quickly as possible, never waiting for the commands to complete.
    // Instead they are put into a queue, to be processed by NetClient on its own
    // task and thread.  Note that if any queues overflow, a reboot is triggered.
    // But publishing or persisting updated values for the same topic will replace
    // any queue entries for those topics rather than growing the queue.
}

void publishSensorData() {

    static unsigned long lastCountTime = 0;
    static unsigned long lastSensorTime = 0;
    static uint32_t count = 0;
    unsigned long now = millis();


    // PUT YOUR SENSOR READING HERE!


    // COUNTER DEMONSTRATION (delete this for a real sensor)

    // Publish count every second (using persistf for printf-style formatting).
    // For NetClient, persist means "publish, and republish periodically" (default 50sec)
    if (now - lastCountTime >= 1000) {
        lastCountTime = now;
        count++;
        NetClient.persistf("%P/counter", "%lu", count);
    }

    // Publish sensor data every 5 seconds
    // %P expands to topicPrefix (e.g., "devices/AABBCCDDEEFF")
    if (now - lastSensorTime >= 5000) {
        lastSensorTime = now;
        char payload[128];

        // Report link speed for Ethernet, signal strength for WiFi
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

        // By the way, persist() publishes immediately if the payload
        // changed, but skips immediate publication if there was no change.
        // If no change, it waits out the 50-second timer before republishing.
        NetClient.persist("%P/stats", payload);
        Serial.printf("Persisted: %s\n", payload);
    }

}
