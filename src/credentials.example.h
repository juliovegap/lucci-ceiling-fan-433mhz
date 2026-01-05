#pragma once
#include <stdint.h>

// ============================================================================
// 1) Rename this file to:  src/credentials.h
// 2) Fill in your own credentials.
// ============================================================================

// ------------------------------
// Wi-Fi
// ------------------------------
// Your Wi-Fi SSID (network name) and password.
static const char* WIFI_SSID     = "YOUR_WIFI_SSID";
static const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ------------------------------
// MQTT
// ------------------------------
// MQTT broker hostname or IP (e.g. "192.168.1.10") and port (usually 1883).
static const char* MQTT_HOST     = "MQTT_BROKER_IP";
static const uint16_t MQTT_PORT  = 1883;

// If you do not use authentication, set these to empty strings "".
static const char* MQTT_USER     = "MQTT_USER";
static const char* MQTT_PASSWORD = "MQTT_PASSWORD";
