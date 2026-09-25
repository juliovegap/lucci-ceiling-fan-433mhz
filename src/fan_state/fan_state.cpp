// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson


#include "fan_state.h"
#include "radio/tx_power.h"

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

// ============================================================================
// ESP32: NVS-backed storage via the Preferences library (original behavior).
// ============================================================================
#if defined(ARDUINO_ARCH_ESP32)
#include <Preferences.h>

static Preferences prefs;

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

// ============================================================================
// ESP8266: no NVS/Preferences API is available, so state is kept in the
// emulated EEPROM (one flash sector) instead, guarded by a magic number +
// version byte + CRC-8 so a blank/foreign/corrupted sector is detected and
// safely replaced with defaults rather than read as garbage.
//
// Caveat (documented in README "Known limitations"): unlike ESP32's NVS,
// this EEPROM library has no wear leveling - EEPROM.commit() erases and
// rewrites the whole sector every time. That's fine for occasional changes,
// but a script that hammers MQTT tuning topics could wear the flash sector
// out over time. If that matters for your setup, look at a wear-leveled
// backend such as the ESP_EEPROM library as a drop-in upgrade.
// ============================================================================
#elif defined(ARDUINO_ARCH_ESP8266)
#include <EEPROM.h>

namespace {

constexpr uint32_t kMagic     = 0x31435546UL; // "FUC1" - arbitrary, just needs to not collide with blank flash (0xFFFFFFFF) or zeroed flash (0x00000000)
constexpr uint8_t  kVersion   = 1;
constexpr int      kEepromLen = 32; // bytes reserved; well under the 4KB sector EEPROM.begin() maps

#pragma pack(push, 1)
struct Blob {
  uint32_t magic;
  uint8_t  version;
  uint8_t  power_on;
  uint8_t  speed;
  uint8_t  dir_reverse;
  uint8_t  desired_reverse;
  uint8_t  invert;
  int8_t   tx_power_dbm;
  uint8_t  tx_repeat;
  uint32_t tx_gap_us;
  uint16_t rot_delay_ms;
  uint8_t  crc;
};
#pragma pack(pop)

uint8_t crc8(const uint8_t* data, size_t len) {
  uint8_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t b = 0; b < 8; b++) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

} // namespace

bool FanStateStore::load(FanState& s) {
  EEPROM.begin(kEepromLen);
  Blob b{};
  EEPROM.get(0, b);
  EEPROM.end();

  bool valid = (b.magic == kMagic) && (b.version == kVersion) &&
               (crc8(reinterpret_cast<const uint8_t*>(&b), sizeof(Blob) - 1) == b.crc);

  if (!valid) {
    // Blank chip, foreign firmware's data, or corrupted sector: fall back to
    // struct defaults (already set by FanState's in-class initializers) and
    // persist them so the next boot reads back a valid blob.
    return save(s);
  }

  s.power_on         = b.power_on != 0;
  s.speed             = clampU8(b.speed, 1, 6);
  s.dir_reverse       = b.dir_reverse != 0;
  s.desired_reverse   = b.desired_reverse != 0;
  s.invert            = b.invert != 0;

  s.tx_power_dbm = isAllowedTxPwr(b.tx_power_dbm) ? b.tx_power_dbm : 0;
  s.tx_repeat    = clampU8(b.tx_repeat, 1, 12);
  s.tx_gap_us    = clampU32(b.tx_gap_us, 1000UL, 30000UL);
  s.rot_delay_ms = clampU16(b.rot_delay_ms, 0, 5000);

  return true;
}

bool FanStateStore::save(const FanState& s) {
  Blob b{};
  b.magic           = kMagic;
  b.version         = kVersion;
  b.power_on        = s.power_on ? 1 : 0;
  b.speed           = s.speed;
  b.dir_reverse     = s.dir_reverse ? 1 : 0;
  b.desired_reverse = s.desired_reverse ? 1 : 0;
  b.invert          = s.invert ? 1 : 0;
  b.tx_power_dbm    = s.tx_power_dbm;
  b.tx_repeat       = s.tx_repeat;
  b.tx_gap_us       = s.tx_gap_us;
  b.rot_delay_ms    = s.rot_delay_ms;
  b.crc             = crc8(reinterpret_cast<const uint8_t*>(&b), sizeof(Blob) - 1);

  EEPROM.begin(kEepromLen);
  EEPROM.put(0, b);
  bool ok = EEPROM.commit();
  EEPROM.end();
  return ok;
}

#endif // platform selection
