// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson

#include <Arduino.h>
#include <WiFi.h>

#include "credentials.h"
#include "device_config.h"

#include "radio/radio_tx.h"
#include "fan_state/fan_state.h"
#include "mqtt/mqtt.h"

// ==============================
// Tuning
// ==============================
static constexpr uint32_t TELEMETRY_MS = 30UL * 1000UL; // more frequent during beta

// ==============================
// Global objects
// ==============================
static RadioTx        radioTx;
static FanStateStore  store;
static FanState       st;
static HaMqtt         ha;

static uint32_t lastTeleMs = 0;
static uint32_t txCount    = 0;
static uint32_t lastTxMs   = 0;

// Pending direction apply after boot
static bool     pendingRotation     = false;
static uint32_t pendingRotationAtMs = 0;

// ==============================
// Helpers
// ==============================
static const char* airflowStr() {
  // Keep payload ASCII-only to avoid encoding issues in MQTT/HA.
  return st.desired_reverse ? "Down" : "Up";
}

static void publishAllState() {
  ha.publishPowerState(st.power_on, true);
  ha.publishSpeedState(st.speed, true);
  ha.publishAirflowState(airflowStr(), true);
}

static void publishDebugAndTune(bool retain = true) {
  uint32_t now = millis();
  uint32_t upS = now / 1000UL;

  uint32_t lastAgeS = 0;
  if (lastTxMs > 0 && now >= lastTxMs) lastAgeS = (now - lastTxMs) / 1000UL;

  ha.publishDebug(txCount, lastAgeS, pendingRotation, st.rot_delay_ms, st.invert, retain);
  ha.publishTuneState(st.tx_power_dbm, st.tx_repeat, st.tx_gap_us, st.rot_delay_ms, st.invert, retain);

  long rssi = ha.wifiConnected() ? WiFi.RSSI() : 0;
  ha.publishTelemetry(rssi, upS, retain);
}

// ==============================
// Direction scheduling
// ==============================
static void scheduleRotationIfNeededOnStart() {
  if (st.desired_reverse != st.dir_reverse) {
    pendingRotation = true;
    pendingRotationAtMs = millis() + (uint32_t)st.rot_delay_ms;
  } else {
    pendingRotation = false;
  }

  // Publish immediately so you can see "pending" in debug
  if (ha.isConnected()) {
    publishDebugAndTune(true);
  }
}

// ==============================
// RF send helpers
// ==============================
static void rfSendOff_() {
  radioTx.send(RadioTx::CmdId::Off);
  txCount++;
  lastTxMs = millis();
}

static void rfSendSpeed_(uint8_t s) {
  RadioTx::CmdId c = RadioTx::CmdId::Speed1;
  switch (s) {
    case 1: c = RadioTx::CmdId::Speed1; break;
    case 2: c = RadioTx::CmdId::Speed2; break;
    case 3: c = RadioTx::CmdId::Speed3; break;
    case 4: c = RadioTx::CmdId::Speed4; break;
    case 5: c = RadioTx::CmdId::Speed5; break;
    case 6: c = RadioTx::CmdId::Speed6; break;
    default: return;
  }
  radioTx.send(c);
  txCount++;
  lastTxMs = millis();
}

static void rfSendRotationPulse_() {
  radioTx.send(RadioTx::CmdId::Rotation);
  txCount++;
  lastTxMs = millis();
}

// ==============================
// State apply
// ==============================
static void applyPower(bool on, bool persist) {
  if (on == st.power_on) {
    if (st.power_on) scheduleRotationIfNeededOnStart();
    return;
  }

  if (!on) {
    rfSendOff_();
    st.power_on = false;
    pendingRotation = false;

    if (persist) store.save(st);
    publishAllState();
    publishDebugAndTune(true);
    return;
  }

  st.power_on = true;
  rfSendSpeed_(st.speed);

  if (persist) store.save(st);
  publishAllState();
  scheduleRotationIfNeededOnStart();
}

static void applySpeed(uint8_t s, bool persist) {
  if (s < 1 || s > 6) return;

  st.speed = s;

  if (st.power_on) {
    rfSendSpeed_(st.speed);
  }

  if (persist) store.save(st);
  publishAllState();
  publishDebugAndTune(true);
}

static void applyDesiredDirection(bool wantReverse, bool persist) {
  st.desired_reverse = wantReverse;

  if (persist) store.save(st);
  publishAllState();
  publishDebugAndTune(true);

  if (!st.power_on) {
    pendingRotation = false;
    return;
  }

  if (st.desired_reverse != st.dir_reverse) {
    rfSendRotationPulse_();
    st.dir_reverse = st.desired_reverse;

    if (persist) store.save(st);
    publishAllState();
    publishDebugAndTune(true);
  }
}

// Sync: send ROTATION without changing stored state
static void applySyncRotationPulse() {
  if (!st.power_on) return;
  rfSendRotationPulse_();
  publishDebugAndTune(true);
}

// ==============================
// Tuning apply
// ==============================
static void applyTxPowerDbm(int8_t dbm) {
  if (st.tx_power_dbm == dbm) return;
  if (!radioTx.setTxPowerDbm(dbm)) return;
  st.tx_power_dbm = dbm;
  store.save(st);
  publishDebugAndTune(true);
}

static void applyTxRepeat(uint8_t rep) {
  if (rep < 1) rep = 1;
  if (rep > 12) rep = 12;
  st.tx_repeat = rep;
  radioTx.setRepeat(rep);
  store.save(st);
  publishDebugAndTune(true);
}

static void applyTxGapUs(uint32_t gapUs) {
  if (gapUs < 1000) gapUs = 1000;
  if (gapUs > 30000) gapUs = 30000;
  st.tx_gap_us = gapUs;
  radioTx.setGapUs(gapUs);
  store.save(st);
  publishDebugAndTune(true);
}

static void applyRotDelayMs(uint16_t ms) {
  if (ms > 5000) ms = 5000;
  st.rot_delay_ms = ms;
  store.save(st);
  publishDebugAndTune(true);
}

static void applyInvert(bool inv) {
  st.invert = inv;
  radioTx.setInvert(inv);
  store.save(st);
  publishDebugAndTune(true);
}

// ==============================
// MQTT handler
// ==============================
static void onMqttCommand(const MqttCommand& cmd) {
  switch (cmd.type) {
    case MqttCmdType::SetPower:
      applyPower(cmd.power_on, true);
      return;

    case MqttCmdType::SetSpeed:
      applySpeed(cmd.speed, true);
      return;

    case MqttCmdType::SetDirection:
      applyDesiredDirection(cmd.dir_reverse, true);
      return;

    case MqttCmdType::SyncRotationPulse:
      applySyncRotationPulse();
      return;

    case MqttCmdType::SetTxPowerDbm:
      applyTxPowerDbm(cmd.tx_power_dbm);
      return;

    case MqttCmdType::SetTxRepeat:
      applyTxRepeat(cmd.tx_repeat);
      return;

    case MqttCmdType::SetTxGapUs:
      applyTxGapUs(cmd.tx_gap_us);
      return;

    case MqttCmdType::SetRotDelayMs:
      applyRotDelayMs(cmd.rot_delay_ms);
      // If fan is ON and pending exists: recompute target time
      if (pendingRotation && st.power_on) {
        pendingRotationAtMs = millis() + (uint32_t)st.rot_delay_ms;
        publishDebugAndTune(true);
      }
      return;

    case MqttCmdType::SetInvert:
      applyInvert(cmd.invert);
      return;
  }
}

// ==============================
// Serial help
// ==============================
static void printHelp() {
  Serial.println();
  Serial.println("Serial:");
  Serial.println("  0..7  -> OFF..ROTATION (direct RF)");
  Serial.println("  i     -> toggle invert");
  Serial.println("  h     -> help");
  Serial.println();

  Serial.println("MQTT controls:");
  const auto& t = ha.topics();
  Serial.printf("  %s (ON/OFF)\n", t.t_power_set);
  Serial.printf("  %s (1..6)\n", t.t_speed_set);
  Serial.printf("  %s (Up/Down)\n", t.t_airflow_set);
  Serial.printf("  %s (PRESS)\n", t.t_sync_press);

  Serial.println("MQTT tuning:");
  Serial.printf("  %s (-30..10)\n", t.t_tune_txpwr_set);
  Serial.printf("  %s (1..12)\n", t.t_tune_txrep_set);
  Serial.printf("  %s (1000..30000)\n", t.t_tune_txgap_set);
  Serial.printf("  %s (0..5000)\n", t.t_tune_rdly_set);
  Serial.printf("  %s (ON/OFF)\n", t.t_tune_invert_set);
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("Lucci CC1101 TX + MQTT/HA (debug package)");

  // 1) load state
  store.load(st);

  // 2) init radio
  if (!radioTx.begin()) {
    Serial.println("Radio init FAIL. Halting.");
    while (true) delay(1000);
  }

  // 3) apply stored tuning to radio
  radioTx.setInvert(st.invert);
  radioTx.setRepeat(st.tx_repeat);
  radioTx.setGapUs(st.tx_gap_us);
  radioTx.setTxPowerDbm(st.tx_power_dbm); // ignored if RadioLib returns error

  Serial.printf("TX cfg: pwr=%d dBm repeat=%u gap=%lu us invert=%d rot_delay=%u ms\n",
                (int)st.tx_power_dbm, (unsigned)st.tx_repeat, (unsigned long)st.tx_gap_us,
                st.invert ? 1 : 0, (unsigned)st.rot_delay_ms);

  // 4) init MQTT/HA
  ha.begin(
    WIFI_SSID, WIFI_PASSWORD,
    MQTT_HOST, MQTT_PORT,
    MQTT_USER, MQTT_PASSWORD,
    BASE_TOPIC,
    DEV_NAME, DEV_MFR, DEV_MODEL, SW_VER
  );
  ha.setHandler(onMqttCommand);

  // 5) publish initial state + debug/tune (no RF)
  publishAllState();
  publishDebugAndTune(true);

  Serial.printf("Boot state: power=%d speed=%u airflow=%s\n",
                st.power_on ? 1 : 0, st.speed, airflowStr());

  printHelp();
}

void loop() {
  ha.loop();

  // Pending direction apply after boot
  if (pendingRotation && st.power_on) {
    uint32_t now = millis();
    if ((int32_t)(now - pendingRotationAtMs) >= 0) {
      rfSendRotationPulse_();
      st.dir_reverse = st.desired_reverse;

      store.save(st);
      publishAllState();

      pendingRotation = false;
      publishDebugAndTune(true);
    }
  }

  // Periodic telemetry + debug
  uint32_t now = millis();
  if (ha.isConnected() && (now - lastTeleMs >= TELEMETRY_MS)) {
    lastTeleMs = now;
    publishDebugAndTune(true);
  }

  // Serial direct RF (debug)
  if (!Serial.available()) return;

  char c = (char)Serial.read();

  if (c == 'h' || c == 'H') { printHelp(); return; }

  if (c == 'i' || c == 'I') {
    applyInvert(!st.invert);
    Serial.printf("Invert = %d\n", st.invert ? 1 : 0);
    return;
  }

  if (c >= '0' && c <= '7') {
    RadioTx::CmdId cmd = (RadioTx::CmdId)(c - '0');
    radioTx.send(cmd);

    if (cmd == RadioTx::CmdId::Off) {
      st.power_on = false;
      pendingRotation = false;
    } else if (cmd >= RadioTx::CmdId::Speed1 && cmd <= RadioTx::CmdId::Speed6) {
      st.power_on = true;
      st.speed = (uint8_t)(1 + ((uint8_t)cmd - (uint8_t)RadioTx::CmdId::Speed1));
      scheduleRotationIfNeededOnStart();
    } else if (cmd == RadioTx::CmdId::Rotation) {
      // Assume the real device toggled direction; sync both
      st.dir_reverse = !st.dir_reverse;
      st.desired_reverse = st.dir_reverse;
    }

    txCount++;
    lastTxMs = millis();

    store.save(st);
    publishAllState();
    publishDebugAndTune(true);
    return;
  }
}
