#pragma once

#include <Arduino.h>

// Firmware version.
const char* FIRMWARE_VERSION = "1.2.0";

// Wake-on-LAN transmission.
const uint16_t WOL_PORT = 9;
const uint8_t WOL_ATTEMPTS = 3;
// Spread packets out so delayed/buffered broadcasts still get retries.
const uint16_t WOL_ATTEMPT_DELAY_MS = 1500;

// Target reachability verification.
const unsigned long PC_CHECK_DELAY_MS = 15000UL;
const unsigned long PC_CHECK_TIMEOUT_MS = 60000UL;
const unsigned long PC_CHECK_INTERVAL_MS = 3000UL;
const unsigned long TCP_CONNECT_TIMEOUT_MS = 1200UL;

// Command abuse protection and one-entry queue.
const unsigned long COMMAND_COOLDOWN_MS = 10000UL;

// Wi-Fi / SNTP.
const unsigned long SNTP_SYNC_TIMEOUT_MS = 20000UL;
const unsigned long SNTP_RETRY_INTERVAL_MS = 60000UL;

// MQTT reconnect backoff.
const unsigned long MQTT_RECONNECT_MIN_MS = 2000UL;
const unsigned long MQTT_RECONNECT_MAX_MS = 60000UL;
