// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson

#pragma once
#include <Arduino.h>

struct FanState {
  // Power (separate from speed)
  bool power_on = false;

  // Last selected speed 1..6 (kept even when power_on=false)
  uint8_t speed = 1;

  // Last "assumed physical" direction (updated when we actually send ROTATION)
  // true = Down, false = Up
  bool dir_reverse = false;

  // User intent (can be changed even when the fan is OFF)
  bool desired_reverse = false;

  // OOK invert for RF
  bool invert = false;

  // ===== Tuning (beta) =====
  int8_t   tx_power_dbm = 0;        // -30,-20,-15,-10,0,5,7,10
  uint8_t  tx_repeat    = 6;        // 1..12
  uint32_t tx_gap_us    = 10000;    // 1000..30000
  uint16_t rot_delay_ms = 800;      // 0..5000
};

class FanStateStore {
public:
  bool load(FanState& s);
  bool save(const FanState& s);
};
