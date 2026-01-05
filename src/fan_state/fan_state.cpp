// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson


#include "fan_state.h"
#include <Preferences.h>

static Preferences prefs;

static uint8_t clampU8(uint8_t v, uint8_t lo, uint8_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static uint16_t clampU16(uint16_t v, uint16_t lo, uint16_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static bool isAllowedTxPwr(int8_t p) {
  switch (p) {
    case -30: case -20: case -15: case -10: case 0: case 5: case 7: case 10:
      return true;
    default:
      return false;
  }
}

bool FanStateStore::load(FanState& s) {
  // Try read-only first
  if (!prefs.begin("lucci", true)) {
    prefs.end();

    // Create namespace if missing
    if (!prefs.begin("lucci", false)) {
      prefs.end();
      return false;
    }

    // Write defaults
    prefs.putBool("pwr", s.power_on);
    prefs.putUChar("spd", s.speed);
    prefs.putBool("dirrev", s.dir_reverse);
    prefs.putBool("desrev", s.desired_reverse);
    prefs.putBool("invert", s.invert);

    prefs.putChar("txpwr", s.tx_power_dbm);
    prefs.putUChar("txrep", s.tx_repeat);
    prefs.putULong("txgap", s.tx_gap_us);
    prefs.putUShort("rdly", s.rot_delay_ms);

    prefs.end();

    if (!prefs.begin("lucci", true)) {
      prefs.end();
      return false;
    }
  }

  // Backward compatibility:
  // - older versions stored "speed" (0..6) without "pwr"/"spd"
  uint8_t oldSpeed = prefs.getUChar("speed", 255);
  bool hasNew = prefs.isKey("pwr") && prefs.isKey("spd");

  if (hasNew) {
    s.power_on = prefs.getBool("pwr", false);
    s.speed    = prefs.getUChar("spd", 1);
  } else if (oldSpeed != 255) {
    s.power_on = (oldSpeed != 0);
    s.speed    = (oldSpeed == 0) ? 1 : oldSpeed;
  }

  s.speed = clampU8(s.speed, 1, 6);

  s.dir_reverse     = prefs.getBool("dirrev", false);
  s.desired_reverse = prefs.getBool("desrev", s.dir_reverse);
  s.invert          = prefs.getBool("invert", false);

  // Tuning
  int8_t pwr = (int8_t)prefs.getChar("txpwr", 0);
  s.tx_power_dbm = isAllowedTxPwr(pwr) ? pwr : 0;

  s.tx_repeat    = clampU8(prefs.getUChar("txrep", 6), 1, 12);
  s.tx_gap_us    = clampU32(prefs.getULong("txgap", 10000UL), 1000UL, 30000UL);
  s.rot_delay_ms = clampU16(prefs.getUShort("rdly", 800), 0, 5000);

  prefs.end();
  return true;
}

bool FanStateStore::save(const FanState& s) {
  if (!prefs.begin("lucci", false)) {
    prefs.end();
    return false;
  }

  prefs.putBool("pwr", s.power_on);
  prefs.putUChar("spd", s.speed);

  // Also write legacy "speed" for compatibility (0..6)
  prefs.putUChar("speed", s.power_on ? s.speed : 0);

  prefs.putBool("dirrev", s.dir_reverse);
  prefs.putBool("desrev", s.desired_reverse);
  prefs.putBool("invert", s.invert);

  // Tuning
  prefs.putChar("txpwr", s.tx_power_dbm);
  prefs.putUChar("txrep", s.tx_repeat);
  prefs.putULong("txgap", s.tx_gap_us);
  prefs.putUShort("rdly", s.rot_delay_ms);

  prefs.end();
  return true;
}
