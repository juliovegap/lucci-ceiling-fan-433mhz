// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson

#pragma once
#include <stdint.h>

// CC1101 output power levels supported by RadioLib's setOutputPower() for
// this radio/board combination.
//
// This used to be copy-pasted (identically) into radio_tx.cpp, fan_state.cpp
// and mqtt.cpp. That's a real bug risk: if the allowed power table is ever
// changed, it's easy to update two of the three copies and leave the third
// one stale (e.g. MQTT would accept a value that fan_state then silently
// clamps back to 0 dBm, or vice versa). Keeping a single inline definition
// here means all three call sites can never disagree.
inline bool isAllowedTxPwr(int8_t p) {
  switch (p) {
    case -30: case -20: case -15: case -10: case 0: case 5: case 7: case 10:
      return true;
    default:
      return false;
  }
}
