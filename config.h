#pragma once

#include <Arduino.h>

// ============================================================
// FIRMWARE
// ============================================================

// Update this value when publishing a new firmware version.
// The version number alone does not indicate production readiness.
const char* FIRMWARE_VERSION = "1.1.0";

// ============================================================
// WAKE-ON-LAN TRANSMISSION
// ============================================================

// Destination UDP port for the magic packet.
const uint16_t WOL_PORT = 9;

// Number of magic-packet send attempts per accepted command.
const uint8_t WOL_ATTEMPTS = 3;

// Delay between send attempts, in milliseconds.
const uint16_t WOL_ATTEMPT_DELAY_MS = 250;

// ============================================================
// TARGET REACHABILITY CHECK
// ============================================================

// Delay before starting target-service checks.
const unsigned long PC_CHECK_DELAY_MS = 15000UL;

// Overall verification deadline measured from command start.
// This includes the initial delay above.
const unsigned long PC_CHECK_TIMEOUT_MS = 60000UL;

// Interval between target-service checks.
const unsigned long PC_CHECK_INTERVAL_MS = 3000UL;

// ============================================================
// MQTT RECONNECT
// ============================================================

// Initial interval between MQTT reconnect attempts.
const unsigned long MQTT_RECONNECT_MIN_MS = 2000UL;

// Maximum interval for exponential reconnect backoff.
const unsigned long MQTT_RECONNECT_MAX_MS = 60000UL;