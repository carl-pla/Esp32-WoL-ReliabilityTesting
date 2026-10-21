#pragma once

#include <Arduino.h>

// Copy this file to secrets.h and replace all example values.
// Never commit secrets.h. Rotate any credentials that were committed before.

// Wi-Fi
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// MQTT broker (hostname only, no scheme/path)
const char* MQTT_HOST = "YOUR_MQTT_BROKER_HOSTNAME";
const uint16_t MQTT_PORT = 8883;
const char* MQTT_CLIENT_ID = "esp32-wol-your-device-id";
const char* MQTT_USER = "YOUR_ESP32_MQTT_USERNAME";
const char* MQTT_PASSWORD = "YOUR_ESP32_MQTT_PASSWORD";

// Topics: match these exactly with the phone client and broker ACLs.
const char* COMMAND_TOPIC = "example/home/esp32/wake";
const char* STATUS_TOPIC = "example/home/esp32/status";
const char* LOG_TOPIC = "example/home/esp32/log";
const char* AVAILABILITY_TOPIC = "example/home/esp32/availability";

// Target system.
const char* SERVER_MAC = "AA:BB:CC:DD:EE:FF";
const char* TARGET_NAME = "example-server";
const char* TARGET_IP = "192.168.178.10";
const uint16_t TARGET_CHECK_PORT = 8006; // e.g. Proxmox web UI; choose an enabled service.

// Root CA certificate for the MQTT broker, in PEM format.
// Replace the placeholder with the complete CA certificate chain root as required.
const char* ROOT_CA = R"EOF(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_THE_COMPLETE_ROOT_CA_CERTIFICATE
-----END CERTIFICATE-----
)EOF";
