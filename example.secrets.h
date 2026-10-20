#pragma once

#include <Arduino.h>
#include <IPAddress.h>

// ============================================================
// PERSONAL CONFIGURATION TEMPLATE
// ============================================================
//
// 1. Copy this file to secrets.h.
// 2. Replace all example values with your own configuration.
// 3. Keep secrets.h out of version control.
//
// This template does not contain working credentials.
// The example network settings must be replaced before use.
//
// ============================================================
// WI-FI
// ============================================================

// Name of your Wi-Fi network.
const char* WIFI_SSID = "YOUR_WIFI_SSID";

// Password of your Wi-Fi network.
// Never commit your real password.
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ============================================================
// MQTT BROKER
// ============================================================

// Broker hostname without a URL scheme or path.
// Example format: broker.example.com
//
// EMQX Cloud is one option. Another compatible MQTT broker
// can be used with the appropriate connection settings.
const char* MQTT_HOST = "YOUR_MQTT_BROKER_HOSTNAME";

// Broker port.
// 8883 is a common port for MQTT over TLS.
// Use the actual TLS port specified by your broker.
const uint16_t MQTT_PORT = 8883;

// Use a unique client ID for each ESP32 deployment.
const char* MQTT_CLIENT_ID = "esp32-wol-example";

// MQTT credentials assigned to this ESP32.
const char* MQTT_USER = "YOUR_MQTT_USERNAME";
const char* MQTT_PASSWORD = "YOUR_MQTT_PASSWORD";

// ============================================================
// MQTT TOPICS
// ============================================================

// Replace the example prefix with your own topic structure.
//
// Use the same topics in:
// - the ESP32 configuration;
// - the remote MQTT client;
// - the broker's authorization rules.
//
// Do not include quotation marks or apostrophes inside the
// topic unless you deliberately want them in the topic name.

// Incoming wake commands.
const char* COMMAND_TOPIC =
    "example/home/esp32/wake";

// Status events associated with individual wake commands.
const char* STATUS_TOPIC =
    "example/home/esp32/status";

// Structured diagnostic messages.
const char* LOG_TOPIC =
    "example/home/esp32/log";

// ============================================================
// TARGET DEVICE
// ============================================================


// MAC address of the target's Wake-on-LAN-capable Ethernet adapter.
// This is an example value. Replace it with the actual MAC.
const char* SERVER_MAC = "02:00:00:00:00:01";

// Broadcast address of the target's local network.
// This example assumes the subnet 192.168.50.0/24.
//
// Replace it with the correct broadcast address for your subnet.
IPAddress BROADCAST_IP(192, 168, 50, 255);

// Target IP address used for the post-wake reachability check.
// Configure a DHCP reservation or another stable IP assignment.
const char* TARGET_IP = "192.168.50.10";

// A TCP port that is reachable when the target has started.
//
// Examples:
// 22   = SSH, if enabled
// 3389 = Remote Desktop, if enabled
// 8006 = Proxmox web interface
//
// An incorrect or blocked port can cause TIMEOUT even when
// the target is powered on.
const uint16_t TARGET_CHECK_PORT = 22;

// ============================================================
// TLS CERTIFICATE VALIDATION
// ============================================================

// Insert the complete PEM-encoded root CA certificate needed
// to validate your MQTT broker's server certificate.
//
// A public root CA certificate is not a secret.
// Do not place private keys in this field.
// Do not disable TLS verification to bypass configuration errors.
//
// The placeholder below is not a valid certificate.
const char* ROOT_CA = R"EOF(
-----BEGIN CERTIFICATE-----
REPLACE_WITH_THE_COMPLETE_ROOT_CA_CERTIFICATE
-----END CERTIFICATE-----
)EOF";