// WiFi credentials
const char* WIFI_SSID = "YourWiFiSSID";
const char* WIFI_PASSWORD = "YourWiFiPassword";

// MQTT Broker configuration
const char* MQTT_SERVER = "mqtt.local";
const char* MQTT_USER = "username";
const char* MQTT_PASSWORD = "password";

// CA Certificate for TLS (set to nullptr to skip certificate verification)
const char* CA_CERT = R"(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
...your certificate here...
-----END CERTIFICATE-----
)";

// ArduinoOTA password - empty string disables over-the-network firmware updates
const char* OTA_PASSWORD = "";
