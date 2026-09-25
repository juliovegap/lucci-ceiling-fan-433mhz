// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson

#pragma once
#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP8266)
  #include <ESP8266WiFi.h>
#else
  #include <WiFi.h>
#endif

#include <PubSubClient.h>

enum class MqttCmdType : uint8_t {
  SetPower,           // ON/OFF
  SetSpeed,           // 1..6
  SetDirection,       // Up/Down (dir_reverse=true means "Down")
  SyncRotationPulse,  // send ROTATION once; DO NOT change stored state

  // ===== Beta tuning =====
  SetTxPowerDbm,      // select
  SetTxRepeat,        // number
  SetTxGapUs,         // number
  SetRotDelayMs,      // number
  SetInvert           // switch
};

struct MqttCommand {
  MqttCmdType type;

  // Power / speed / direction
  bool power_on = false;
  uint8_t speed = 1;

  // true = Down, false = Up
  bool dir_reverse = false;

  // Tuning payloads
  int8_t   tx_power_dbm = 0;
  uint8_t  tx_repeat = 6;
  uint32_t tx_gap_us = 10000;
  uint16_t rot_delay_ms = 800;
  bool invert = false;
};

using MqttCmdHandler = void (*)(const MqttCommand& cmd);

struct MqttRuntime {
  // availability
  char t_avail[96];

  // controls
  char t_power_set[96];
  char t_power_state[96];

  char t_speed_set[96];
  char t_speed_state[96];

  char t_airflow_set[96];
  char t_airflow_state[96];

  char t_sync_press[96];

  // telemetry (base)
  char t_tele_rssi[96];
  char t_tele_uptime_s[96];

  // ===== Debug topics (read-only) =====
  char t_dbg_tx_count[96];
  char t_dbg_last_tx_age_s[96];
  char t_dbg_pending_rot[96];
  char t_dbg_rot_delay_ms[96];
  char t_dbg_invert[96];

  // ===== Tuning topics (rw) =====
  char t_tune_txpwr_set[96];
  char t_tune_txpwr_state[96];

  char t_tune_txrep_set[96];
  char t_tune_txrep_state[96];

  char t_tune_txgap_set[96];
  char t_tune_txgap_state[96];

  char t_tune_rdly_set[96];
  char t_tune_rdly_state[96];

  char t_tune_invert_set[96];
  char t_tune_invert_state[96];

  // discovery (controls)
  char ha_power_switch_cfg[180];
  char ha_speed_select_cfg[180];
  char ha_airflow_select_cfg[180];
  char ha_sync_button_cfg[210];

  // discovery (base sensors)
  char ha_rssi_cfg[160];
  char ha_uptime_cfg[160];

  // discovery (debug sensors)
  char ha_dbg_tx_count_cfg[200];
  char ha_dbg_last_tx_age_cfg[220];
  char ha_dbg_pending_rot_cfg[230];
  char ha_dbg_rot_delay_cfg[220];
  char ha_dbg_invert_cfg[220];

  // discovery (tuning)
  char ha_tune_txpwr_cfg[230];
  char ha_tune_txrep_cfg[230];
  char ha_tune_txgap_cfg[230];
  char ha_tune_rdly_cfg[230];
  char ha_tune_invert_cfg[230];

  // legacy cleanup
  char ha_fan_legacy_cfg[170];
};

class HaMqtt {
public:
  void begin(
    const char* wifiSsid,
    const char* wifiPass,
    const char* mqttHost,
    uint16_t mqttPort,
    const char* mqttUser,
    const char* mqttPasswd,
    const char* baseTopic,
    const char* devName,
    const char* devMfr,
    const char* devModel,
    const char* swVer
  );

  void setHandler(MqttCmdHandler h);
  void loop();

  const MqttRuntime& topics() const { return rt_; }

  bool isConnected() { return mqtt_.connected(); }
  bool wifiConnected() const { return WiFi.isConnected(); }

  // publish (controls)
  void publishAvailability(bool online, bool retain = true);
  void publishPowerState(bool on, bool retain = true);
  void publishSpeedState(uint8_t speed, bool retain = true);
  void publishAirflowState(const char* airflow, bool retain = true);

  // publish (base sensors)
  void publishTelemetry(long wifiRssi, uint32_t uptimeS, bool retain = true);

  // publish (debug)
  void publishDebug(uint32_t txCount, uint32_t lastTxAgeS, bool pendingRot, uint16_t rotDelayMs, bool invert, bool retain = true);

  // publish (tuning state)
  void publishTuneState(int8_t txPwrDbm, uint8_t txRep, uint32_t txGapUs, uint16_t rotDelayMs, bool invert, bool retain = true);

private:
  WiFiClient wifiClient_;
  PubSubClient mqtt_{wifiClient_};
  MqttCmdHandler handler_ = nullptr;

  MqttRuntime rt_{};

  const char* wifiSsid_ = nullptr;
  const char* wifiPass_ = nullptr;

  const char* mqttHost_ = nullptr;
  uint16_t mqttPort_ = 1883;
  const char* mqttUser_ = nullptr;
  const char* mqttPasswd_ = nullptr;

  const char* baseTopic_ = nullptr;
  const char* devName_ = nullptr;
  const char* devMfr_ = nullptr;
  const char* devModel_ = nullptr;
  const char* swVer_ = nullptr;

  uint32_t lastWifiTryMs_ = 0;

  // Bug fix: mqttEnsure_() used to be called from loop() with no rate limit
  // at all (unlike wifiEnsure_(), just below, which already throttles WiFi
  // reconnect attempts). If the broker actively refused connections this
  // turned into a tight reconnect loop. Mirrors the existing WiFi throttle.
  static constexpr uint32_t MQTT_RETRY_MS = 5000;
  uint32_t lastMqttTryMs_ = 0;

  void initTopics_();
  void wifiEnsure_();
  void mqttEnsure_();
  void publishDiscovery_();
  void publishLegacyCleanup_();

  static void mqttCallbackThunk_(char* topic, byte* payload, unsigned int length);
  void mqttCallback_(char* topic, byte* payload, unsigned int length);

  void publish_(const char* topic, const char* payload, bool retain);
};
