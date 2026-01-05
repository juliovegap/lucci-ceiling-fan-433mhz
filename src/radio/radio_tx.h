// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson

#pragma once
#include <Arduino.h>

class RadioTx {
public:
  // Kommando-id internt (oberoende av patterns.h)
  enum class CmdId : uint8_t {
    Off = 0,
    Speed1,
    Speed2,
    Speed3,
    Speed4,
    Speed5,
    Speed6,
    Rotation,
    Max
  };

  bool begin();

  // Runtime tuning
  bool setTxPowerDbm(int8_t dbm);    // -30,-20,-15,-10,0,5,7,10
  void setRepeat(uint8_t repeatN);   // 1..12
  void setGapUs(uint32_t gapUs);     // 1000..30000

  // Signalform
  void setInvert(bool inv);

  // Skicka kommando
  void send(CmdId c);

private:
  bool     invert_  = false;
  uint8_t  repeat_  = 6;
  uint32_t gap_us_  = 10000;
  int8_t   pwr_dbm_ = 0;
};
