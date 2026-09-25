// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson

#include "mqtt.h"
#include <string.h>
#include <strings.h>  // strcasecmp
#include <stdlib.h>

#include "radio/tx_power.h"

static HaMqtt* g_self = nullptr;

void HaMqtt::begin(
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
) {
  wifiSsid_ = wifiSsid;
  wifiPass_ = wifiPass;

  mqttHost_ = mqttHost;
  mqttPort_ = mqttPort;
  mqttUser_ = mqttUser;
  mqttPasswd_ = mqttPasswd;

  baseTopic_ = baseTopic;
  devName_ = devName;
  devMfr_ = devMfr;
  devModel_ = devModel;
  swVer_ = swVer;

  initTopics_();

  mqtt_.setServer(mqttHost_, mqttPort_);
  mqtt_.setBufferSize(1536);

  g_self = this;
  mqtt_.setCallback(HaMqtt::mqttCallbackThunk_);

  wifiEnsure_();
  mqttEnsure_();
}

void HaMqtt::setHandler(MqttCmdHandler h) { handler_ = h; }

void HaMqtt::initTopics_() {
  // availability
  snprintf(rt_.t_avail, sizeof(rt_.t_avail), "%s/availability", baseTopic_);

  // controls
  snprintf(rt_.t_power_set,   sizeof(rt_.t_power_set),   "%s/power/set",   baseTopic_);
  snprintf(rt_.t_power_state, sizeof(rt_.t_power_state), "%s/power/state", baseTopic_);

  snprintf(rt_.t_speed_set,   sizeof(rt_.t_speed_set),   "%s/speed/set",   baseTopic_);
  snprintf(rt_.t_speed_state, sizeof(rt_.t_speed_state), "%s/speed/state", baseTopic_);

  snprintf(rt_.t_airflow_set,   sizeof(rt_.t_airflow_set),   "%s/airflow/set",   baseTopic_);
  snprintf(rt_.t_airflow_state, sizeof(rt_.t_airflow_state), "%s/airflow/state", baseTopic_);

  snprintf(rt_.t_sync_press, sizeof(rt_.t_sync_press), "%s/airflow/sync/press", baseTopic_);

  // base telemetry
  snprintf(rt_.t_tele_rssi,     sizeof(rt_.t_tele_rssi),     "%s/tele/wifi_rssi", baseTopic_);
  snprintf(rt_.t_tele_uptime_s, sizeof(rt_.t_tele_uptime_s), "%s/tele/uptime_s",  baseTopic_);

  // debug topics
  snprintf(rt_.t_dbg_tx_count,       sizeof(rt_.t_dbg_tx_count),       "%s/dbg/tx_count",         baseTopic_);
  snprintf(rt_.t_dbg_last_tx_age_s,  sizeof(rt_.t_dbg_last_tx_age_s),  "%s/dbg/last_tx_age_s",    baseTopic_);
  snprintf(rt_.t_dbg_pending_rot,    sizeof(rt_.t_dbg_pending_rot),    "%s/dbg/pending_rotation", baseTopic_);
  snprintf(rt_.t_dbg_rot_delay_ms,   sizeof(rt_.t_dbg_rot_delay_ms),   "%s/dbg/rot_delay_ms",     baseTopic_);
  snprintf(rt_.t_dbg_invert,         sizeof(rt_.t_dbg_invert),         "%s/dbg/invert",           baseTopic_);

  // tuning topics
  snprintf(rt_.t_tune_txpwr_set,   sizeof(rt_.t_tune_txpwr_set),   "%s/tune/tx_power_dbm/set",  baseTopic_);
  snprintf(rt_.t_tune_txpwr_state, sizeof(rt_.t_tune_txpwr_state), "%s/tune/tx_power_dbm/state",baseTopic_);

  snprintf(rt_.t_tune_txrep_set,   sizeof(rt_.t_tune_txrep_set),   "%s/tune/tx_repeat/set",     baseTopic_);
  snprintf(rt_.t_tune_txrep_state, sizeof(rt_.t_tune_txrep_state), "%s/tune/tx_repeat/state",   baseTopic_);

  snprintf(rt_.t_tune_txgap_set,   sizeof(rt_.t_tune_txgap_set),   "%s/tune/tx_gap_us/set",     baseTopic_);
  snprintf(rt_.t_tune_txgap_state, sizeof(rt_.t_tune_txgap_state), "%s/tune/tx_gap_us/state",   baseTopic_);

  snprintf(rt_.t_tune_rdly_set,   sizeof(rt_.t_tune_rdly_set),   "%s/tune/rot_delay_ms/set",    baseTopic_);
  snprintf(rt_.t_tune_rdly_state, sizeof(rt_.t_tune_rdly_state), "%s/tune/rot_delay_ms/state",  baseTopic_);

  snprintf(rt_.t_tune_invert_set,   sizeof(rt_.t_tune_invert_set),   "%s/tune/invert/set",       baseTopic_);
  snprintf(rt_.t_tune_invert_state, sizeof(rt_.t_tune_invert_state), "%s/tune/invert/state",     baseTopic_);

  // discovery (controls)
  snprintf(rt_.ha_power_switch_cfg,   sizeof(rt_.ha_power_switch_cfg),   "homeassistant/switch/%s_power/config",   baseTopic_);
  snprintf(rt_.ha_speed_select_cfg,   sizeof(rt_.ha_speed_select_cfg),   "homeassistant/select/%s_speed/config",   baseTopic_);
  snprintf(rt_.ha_airflow_select_cfg, sizeof(rt_.ha_airflow_select_cfg), "homeassistant/select/%s_airflow/config", baseTopic_);
  snprintf(rt_.ha_sync_button_cfg,    sizeof(rt_.ha_sync_button_cfg),    "homeassistant/button/%s_sync_airflow/config", baseTopic_);

  // discovery (base sensors)
  snprintf(rt_.ha_rssi_cfg,   sizeof(rt_.ha_rssi_cfg),   "homeassistant/sensor/%s_wifi_rssi/config", baseTopic_);
  snprintf(rt_.ha_uptime_cfg, sizeof(rt_.ha_uptime_cfg), "homeassistant/sensor/%s_uptime/config",    baseTopic_);

  // discovery (debug sensors)
  snprintf(rt_.ha_dbg_tx_count_cfg,      sizeof(rt_.ha_dbg_tx_count_cfg),      "homeassistant/sensor/%s_dbg_tx_count/config",      baseTopic_);
  snprintf(rt_.ha_dbg_last_tx_age_cfg,   sizeof(rt_.ha_dbg_last_tx_age_cfg),   "homeassistant/sensor/%s_dbg_last_tx_age/config",   baseTopic_);
  snprintf(rt_.ha_dbg_pending_rot_cfg,   sizeof(rt_.ha_dbg_pending_rot_cfg),   "homeassistant/binary_sensor/%s_dbg_pending_rot/config", baseTopic_);
  snprintf(rt_.ha_dbg_rot_delay_cfg,     sizeof(rt_.ha_dbg_rot_delay_cfg),     "homeassistant/sensor/%s_dbg_rot_delay/config",     baseTopic_);
  snprintf(rt_.ha_dbg_invert_cfg,        sizeof(rt_.ha_dbg_invert_cfg),        "homeassistant/binary_sensor/%s_dbg_invert/config", baseTopic_);

  // discovery (tuning)
  snprintf(rt_.ha_tune_txpwr_cfg,   sizeof(rt_.ha_tune_txpwr_cfg),   "homeassistant/select/%s_tune_txpwr/config", baseTopic_);
  snprintf(rt_.ha_tune_txrep_cfg,   sizeof(rt_.ha_tune_txrep_cfg),   "homeassistant/number/%s_tune_txrep/config", baseTopic_);
  snprintf(rt_.ha_tune_txgap_cfg,   sizeof(rt_.ha_tune_txgap_cfg),   "homeassistant/number/%s_tune_txgap/config", baseTopic_);
  snprintf(rt_.ha_tune_rdly_cfg,    sizeof(rt_.ha_tune_rdly_cfg),    "homeassistant/number/%s_tune_rdly/config",  baseTopic_);
  snprintf(rt_.ha_tune_invert_cfg,  sizeof(rt_.ha_tune_invert_cfg),  "homeassistant/switch/%s_tune_invert/config",baseTopic_);

  // legacy cleanup
  snprintf(rt_.ha_fan_legacy_cfg, sizeof(rt_.ha_fan_legacy_cfg), "homeassistant/fan/%s/config", baseTopic_);
}

void HaMqtt::wifiEnsure_() {
  if (WiFi.isConnected()) return;

  uint32_t now = millis();
  if (now - lastWifiTryMs_ < 3000) return;
  lastWifiTryMs_ = now;

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid_, wifiPass_);
}

void HaMqtt::mqttEnsure_() {
  if (!WiFi.isConnected()) return;
  if (mqtt_.connected()) return;

  uint32_t now = millis();
  if (now - lastMqttTryMs_ < MQTT_RETRY_MS) return;
  lastMqttTryMs_ = now;

  Serial.print("MQTT: connecting... ");

  bool hasUser = (mqttUser_ && mqttUser_[0] != '\0');
  bool ok = false;

  if (hasUser) {
    ok = mqtt_.connect(baseTopic_, mqttUser_, mqttPasswd_, rt_.t_avail, 1, true, "offline");
  } else {
    ok = mqtt_.connect(baseTopic_, rt_.t_avail, 1, true, "offline");
  }

  if (!ok) {
    Serial.printf("FAIL rc=%d\n", mqtt_.state());
    return;
  }

  Serial.println("OK");

  publishAvailability(true, true);

  publishLegacyCleanup_();
  publishDiscovery_();

  // subscribe controls
  mqtt_.subscribe(rt_.t_power_set, 1);
  mqtt_.subscribe(rt_.t_speed_set, 1);
  mqtt_.subscribe(rt_.t_airflow_set, 1);
  mqtt_.subscribe(rt_.t_sync_press, 1);

  // subscribe tuning
  mqtt_.subscribe(rt_.t_tune_txpwr_set, 1);
  mqtt_.subscribe(rt_.t_tune_txrep_set, 1);
  mqtt_.subscribe(rt_.t_tune_txgap_set, 1);
  mqtt_.subscribe(rt_.t_tune_rdly_set, 1);
  mqtt_.subscribe(rt_.t_tune_invert_set, 1);
}

void HaMqtt::publish_(const char* topic, const char* payload, bool retain) {
  mqtt_.publish(topic, payload, retain);
}

void HaMqtt::publishAvailability(bool online, bool retain) {
  publish_(rt_.t_avail, online ? "online" : "offline", retain);
}

void HaMqtt::publishPowerState(bool on, bool retain) {
  publish_(rt_.t_power_state, on ? "ON" : "OFF", retain);
}

void HaMqtt::publishSpeedState(uint8_t speed, bool retain) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", (unsigned)speed);
  publish_(rt_.t_speed_state, buf, retain);
}

void HaMqtt::publishAirflowState(const char* airflow, bool retain) {
  publish_(rt_.t_airflow_state, airflow, retain);
}

void HaMqtt::publishTelemetry(long wifiRssi, uint32_t uptimeS, bool retain) {
  char buf[32];

  snprintf(buf, sizeof(buf), "%ld", wifiRssi);
  publish_(rt_.t_tele_rssi, buf, retain);

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)uptimeS);
  publish_(rt_.t_tele_uptime_s, buf, retain);
}

void HaMqtt::publishDebug(uint32_t txCount, uint32_t lastTxAgeS, bool pendingRot, uint16_t rotDelayMs, bool invert, bool retain) {
  char buf[32];

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)txCount);
  publish_(rt_.t_dbg_tx_count, buf, retain);

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)lastTxAgeS);
  publish_(rt_.t_dbg_last_tx_age_s, buf, retain);

  publish_(rt_.t_dbg_pending_rot, pendingRot ? "ON" : "OFF", retain);

  snprintf(buf, sizeof(buf), "%u", (unsigned)rotDelayMs);
  publish_(rt_.t_dbg_rot_delay_ms, buf, retain);

  publish_(rt_.t_dbg_invert, invert ? "ON" : "OFF", retain);
}

void HaMqtt::publishTuneState(int8_t txPwrDbm, uint8_t txRep, uint32_t txGapUs, uint16_t rotDelayMs, bool invert, bool retain) {
  char buf[32];

  snprintf(buf, sizeof(buf), "%d", (int)txPwrDbm);
  publish_(rt_.t_tune_txpwr_state, buf, retain);

  snprintf(buf, sizeof(buf), "%u", (unsigned)txRep);
  publish_(rt_.t_tune_txrep_state, buf, retain);

  snprintf(buf, sizeof(buf), "%lu", (unsigned long)txGapUs);
  publish_(rt_.t_tune_txgap_state, buf, retain);

  snprintf(buf, sizeof(buf), "%u", (unsigned)rotDelayMs);
  publish_(rt_.t_tune_rdly_state, buf, retain);

  publish_(rt_.t_tune_invert_state, invert ? "ON" : "OFF", retain);
}

void HaMqtt::publishLegacyCleanup_() {
  // Remove old FAN discovery (extra start/stop)
  publish_(rt_.ha_fan_legacy_cfg, "", true);
}

void HaMqtt::publishDiscovery_() {
  // ===== Controls: Power switch =====
  {
    char cfg[1200];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"Fan\","
        "\"unique_id\":\"%s_power\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"icon\":\"mdi:fan\","
        "\"device\":{"
          "\"identifiers\":[\"%s\"],"
          "\"name\":\"%s\","
          "\"manufacturer\":\"%s\","
          "\"model\":\"%s\","
          "\"sw_version\":\"%s\""
        "}"
      "}",
      baseTopic_,
      rt_.t_avail,
      rt_.t_power_set,
      rt_.t_power_state,
      baseTopic_,
      devName_,
      devMfr_,
      devModel_,
      swVer_
    );
    publish_(rt_.ha_power_switch_cfg, cfg, true);
  }

  // ===== Controls: Speed select =====
  {
    char cfg[900];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"Fan speed\","
        "\"unique_id\":\"%s_speed\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"options\":[\"1\",\"2\",\"3\",\"4\",\"5\",\"6\"],"
        "\"icon\":\"mdi:speedometer\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_avail,
      rt_.t_speed_set,
      rt_.t_speed_state,
      baseTopic_
    );
    publish_(rt_.ha_speed_select_cfg, cfg, true);
  }

  // ===== Controls: Airflow direction select =====
  {
    char cfg[900];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"Airflow direction\","
        "\"unique_id\":\"%s_airflow\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"options\":[\"Up\",\"Down\"],"
        "\"icon\":\"mdi:swap-vertical\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_avail,
      rt_.t_airflow_set,
      rt_.t_airflow_state,
      baseTopic_
    );
    publish_(rt_.ha_airflow_select_cfg, cfg, true);
  }

  // ===== Controls: Sync direction button =====
  {
    char cfg[900];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"Sync airflow direction\","
        "\"unique_id\":\"%s_sync_airflow\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"command_topic\":\"%s\","
        "\"payload_press\":\"PRESS\","
        "\"icon\":\"mdi:sync\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_avail,
      rt_.t_sync_press,
      baseTopic_
    );
    publish_(rt_.ha_sync_button_cfg, cfg, true);
  }

  // ===== Base sensor: WiFi RSSI =====
  {
    char cfg[700];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"WiFi RSSI\","
        "\"unique_id\":\"%s_wifi_rssi\","
        "\"state_topic\":\"%s\","
        "\"unit_of_measurement\":\"dBm\","
        "\"device_class\":\"signal_strength\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_tele_rssi,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_rssi_cfg, cfg, true);
  }

  // ===== Base sensor: Uptime (seconds) =====
  {
    char cfg[700];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"Uptime\","
        "\"unique_id\":\"%s_uptime\","
        "\"state_topic\":\"%s\","
        "\"unit_of_measurement\":\"s\","
        "\"device_class\":\"duration\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_tele_uptime_s,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_uptime_cfg, cfg, true);
  }

  // ===== Debug sensors (diagnostic, disabled_by_default) =====
  const char* diagPrefix = "\"entity_category\":\"diagnostic\",\"enabled_by_default\":false,";

  // TX count
  {
    char cfg[900];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"DBG TX count\","
        "\"unique_id\":\"%s_dbg_tx_count\","
        "\"state_topic\":\"%s\","
        "%s"
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:counter\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_dbg_tx_count,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_dbg_tx_count_cfg, cfg, true);
  }

  // Last TX age
  {
    char cfg[900];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"DBG Last TX age\","
        "\"unique_id\":\"%s_dbg_last_tx_age\","
        "\"state_topic\":\"%s\","
        "%s"
        "\"unit_of_measurement\":\"s\","
        "\"device_class\":\"duration\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:timer-outline\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_dbg_last_tx_age_s,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_dbg_last_tx_age_cfg, cfg, true);
  }

  // Pending rotation (binary)
  {
    char cfg[950];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"DBG Pending rotation\","
        "\"unique_id\":\"%s_dbg_pending_rot\","
        "\"state_topic\":\"%s\","
        "%s"
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:rotate-right\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_dbg_pending_rot,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_dbg_pending_rot_cfg, cfg, true);
  }

  // Rotation delay (ms)
  {
    char cfg[950];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"DBG Rotation delay\","
        "\"unique_id\":\"%s_dbg_rot_delay\","
        "\"state_topic\":\"%s\","
        "%s"
        "\"unit_of_measurement\":\"ms\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:timer-cog-outline\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_dbg_rot_delay_ms,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_dbg_rot_delay_cfg, cfg, true);
  }

  // Invert (binary)
  {
    char cfg[950];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"DBG Invert OOK\","
        "\"unique_id\":\"%s_dbg_invert\","
        "\"state_topic\":\"%s\","
        "%s"
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:swap-horizontal\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_dbg_invert,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_dbg_invert_cfg, cfg, true);
  }

  // ===== Tuning entities (diagnostic, disabled_by_default) =====

  // TX power dBm (select)
  {
    char cfg[1200];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"TUNE TX power (dBm)\","
        "\"unique_id\":\"%s_tune_txpwr\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"options\":[\"-30\",\"-20\",\"-15\",\"-10\",\"0\",\"5\",\"7\",\"10\"],"
        "%s"
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:signal\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_tune_txpwr_set,
      rt_.t_tune_txpwr_state,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_tune_txpwr_cfg, cfg, true);
  }

  // TX repeat (number)
  {
    char cfg[1200];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"TUNE TX repeat\","
        "\"unique_id\":\"%s_tune_txrep\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"min\":1,"
        "\"max\":12,"
        "\"step\":1,"
        "\"mode\":\"box\","
        "%s"
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:repeat\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_tune_txrep_set,
      rt_.t_tune_txrep_state,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_tune_txrep_cfg, cfg, true);
  }

  // TX gap us (number)
  {
    char cfg[1200];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"TUNE TX gap (us)\","
        "\"unique_id\":\"%s_tune_txgap\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"min\":1000,"
        "\"max\":30000,"
        "\"step\":500,"
        "\"mode\":\"box\","
        "%s"
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:timer\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_tune_txgap_set,
      rt_.t_tune_txgap_state,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_tune_txgap_cfg, cfg, true);
  }

  // Rotation delay ms (number)
  {
    char cfg[1200];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"TUNE Rotation delay (ms)\","
        "\"unique_id\":\"%s_tune_rdly\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"min\":0,"
        "\"max\":5000,"
        "\"step\":50,"
        "\"mode\":\"box\","
        "%s"
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:timer-cog\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_tune_rdly_set,
      rt_.t_tune_rdly_state,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_tune_rdly_cfg, cfg, true);
  }

  // Invert (switch)
  {
    char cfg[1200];
    snprintf(
      cfg, sizeof(cfg),
      "{"
        "\"name\":\"TUNE Invert OOK\","
        "\"unique_id\":\"%s_tune_invert\","
        "\"command_topic\":\"%s\","
        "\"state_topic\":\"%s\","
        "\"payload_on\":\"ON\","
        "\"payload_off\":\"OFF\","
        "%s"
        "\"availability_topic\":\"%s\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:swap-horizontal\","
        "\"device\":{\"identifiers\":[\"%s\"]}"
      "}",
      baseTopic_,
      rt_.t_tune_invert_set,
      rt_.t_tune_invert_state,
      diagPrefix,
      rt_.t_avail,
      baseTopic_
    );
    publish_(rt_.ha_tune_invert_cfg, cfg, true);
  }
}

void HaMqtt::mqttCallbackThunk_(char* topic, byte* payload, unsigned int length) {
  if (g_self) g_self->mqttCallback_(topic, payload, length);
}

void HaMqtt::mqttCallback_(char* topic, byte* payload, unsigned int length) {
  char msg[128];
  unsigned int n = (length >= sizeof(msg)) ? (sizeof(msg) - 1) : length;
  memcpy(msg, payload, n);
  msg[n] = '\0';

  // Trim tail whitespace/newlines
  while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == ' ' || msg[n - 1] == '\t')) {
    msg[n - 1] = '\0';
    n--;
  }

  Serial.printf("MQTT RX: %s => %s\n", topic, msg);

  if (!handler_) return;

  // Controls: power
  if (strcmp(topic, rt_.t_power_set) == 0) {
    if (strcasecmp(msg, "ON") == 0 || strcmp(msg, "1") == 0) {
      MqttCommand c; c.type = MqttCmdType::SetPower; c.power_on = true; handler_(c); return;
    }
    if (strcasecmp(msg, "OFF") == 0 || strcmp(msg, "0") == 0) {
      MqttCommand c; c.type = MqttCmdType::SetPower; c.power_on = false; handler_(c); return;
    }
    return;
  }

  // Controls: speed
  if (strcmp(topic, rt_.t_speed_set) == 0) {
    if (strlen(msg) == 1 && msg[0] >= '1' && msg[0] <= '6') {
      MqttCommand c; c.type = MqttCmdType::SetSpeed; c.speed = (uint8_t)(msg[0] - '0'); handler_(c); return;
    }
    return;
  }

  // Controls: airflow direction (new: Up/Down, also accept old Swedish values)
  if (strcmp(topic, rt_.t_airflow_set) == 0) {
    if (strcasecmp(msg, "Up") == 0 || strcasecmp(msg, "Upward") == 0 || strcasecmp(msg, "Uppat") == 0 || strcasecmp(msg, "Uppåt") == 0) {
      MqttCommand c; c.type = MqttCmdType::SetDirection; c.dir_reverse = false; handler_(c); return;
    }
    if (strcasecmp(msg, "Down") == 0 || strcasecmp(msg, "Downward") == 0 || strcasecmp(msg, "Nedat") == 0 || strcasecmp(msg, "Nedåt") == 0) {
      MqttCommand c; c.type = MqttCmdType::SetDirection; c.dir_reverse = true; handler_(c); return;
    }
    return;
  }

  // Controls: sync press
  if (strcmp(topic, rt_.t_sync_press) == 0) {
    if (strcasecmp(msg, "PRESS") == 0 || msg[0] == '\0') {
      MqttCommand c; c.type = MqttCmdType::SyncRotationPulse; handler_(c); return;
    }
    return;
  }

  // Tuning: TX power
  if (strcmp(topic, rt_.t_tune_txpwr_set) == 0) {
    int v = atoi(msg);
    int8_t p = (int8_t)v;
    if (!isAllowedTxPwr(p)) return;
    MqttCommand c; c.type = MqttCmdType::SetTxPowerDbm; c.tx_power_dbm = p; handler_(c); return;
  }

  // Tuning: repeat
  if (strcmp(topic, rt_.t_tune_txrep_set) == 0) {
    int v = atoi(msg);
    if (v < 1) v = 1;
    if (v > 12) v = 12;
    MqttCommand c; c.type = MqttCmdType::SetTxRepeat; c.tx_repeat = (uint8_t)v; handler_(c); return;
  }

  // Tuning: gap us
  if (strcmp(topic, rt_.t_tune_txgap_set) == 0) {
    long v = atol(msg);
    if (v < 1000) v = 1000;
    if (v > 30000) v = 30000;
    MqttCommand c; c.type = MqttCmdType::SetTxGapUs; c.tx_gap_us = (uint32_t)v; handler_(c); return;
  }

  // Tuning: rot delay ms
  if (strcmp(topic, rt_.t_tune_rdly_set) == 0) {
    int v = atoi(msg);
    if (v < 0) v = 0;
    if (v > 5000) v = 5000;
    MqttCommand c; c.type = MqttCmdType::SetRotDelayMs; c.rot_delay_ms = (uint16_t)v; handler_(c); return;
  }

  // Tuning: invert
  if (strcmp(topic, rt_.t_tune_invert_set) == 0) {
    bool on  = (strcasecmp(msg, "ON") == 0 || strcmp(msg, "1") == 0 || strcasecmp(msg, "true") == 0);
    bool off = (strcasecmp(msg, "OFF") == 0 || strcmp(msg, "0") == 0 || strcasecmp(msg, "false") == 0);
    if (!on && !off) return;
    MqttCommand c; c.type = MqttCmdType::SetInvert; c.invert = on; handler_(c); return;
  }
}

void HaMqtt::loop() {
  wifiEnsure_();
  mqttEnsure_();
  mqtt_.loop();
}
