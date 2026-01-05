// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson

#include "radio_tx.h"

#include <SPI.h>
#include <RadioLib.h>
#include <pgmspace.h>

#include "patterns/patterns.h" // måste ge P_OFF..P_ROTATION + *_LEN

// PIN + RF params
static constexpr int PIN_SCK  = 18;
static constexpr int PIN_MISO = 19;
static constexpr int PIN_MOSI = 23;
static constexpr int PIN_CS   = 5;
static constexpr int PIN_GDO0 = 4;

static constexpr float RF_FREQ_MHZ   = 434.05f;
static constexpr float RX_BW_KHZ     = 135.0f;
static constexpr float BITRATE_KBPS  = 4.8f;
static constexpr float FREQDEV_KHZ   = 5.0f;

static CC1101 radio = new Module(PIN_CS, PIN_GDO0, RADIOLIB_NC, RADIOLIB_NC);

static inline bool isAllowedTxPwr(int8_t p) {
  switch (p) {
    case -30: case -20: case -15: case -10: case 0: case 5: case 7: case 10:
      return true;
    default:
      return false;
  }
}

static inline void writeLevel(bool high, bool invert) {
  if (invert) high = !high;
  digitalWrite(PIN_GDO0, high ? HIGH : LOW);
}

static void sendPatternPGM(const int16_t* pat, uint16_t len, uint8_t repeatN, uint32_t gapUs, bool invert) {
  writeLevel(false, invert);
  delayMicroseconds(200);

  for (uint8_t r = 0; r < repeatN; r++) {
    for (uint16_t i = 0; i < len; i++) {
      int16_t v = (int16_t)pgm_read_word(&pat[i]);
      uint16_t us = (uint16_t)abs(v);

      bool levelAfterEdge = (v < 0);
      bool levelDuring = !levelAfterEdge;

      writeLevel(levelDuring, invert);
      delayMicroseconds(us);
      writeLevel(levelAfterEdge, invert);
    }

    writeLevel(false, invert);
    delayMicroseconds((uint16_t)gapUs);
  }

  writeLevel(false, invert);
}

static const char* cmdName_(RadioTx::CmdId c) {
  switch (c) {
    case RadioTx::CmdId::Off:      return "OFF";
    case RadioTx::CmdId::Speed1:   return "SPEED1";
    case RadioTx::CmdId::Speed2:   return "SPEED2";
    case RadioTx::CmdId::Speed3:   return "SPEED3";
    case RadioTx::CmdId::Speed4:   return "SPEED4";
    case RadioTx::CmdId::Speed5:   return "SPEED5";
    case RadioTx::CmdId::Speed6:   return "SPEED6";
    case RadioTx::CmdId::Rotation: return "ROTATION";
    default:                       return "?";
  }
}

static bool getPattern_(RadioTx::CmdId c, const int16_t*& pat, uint16_t& len) {
  switch (c) {
    case RadioTx::CmdId::Off:      pat = P_OFF;      len = P_OFF_LEN;      return true;
    case RadioTx::CmdId::Speed1:   pat = P_SPEED1;   len = P_SPEED1_LEN;   return true;
    case RadioTx::CmdId::Speed2:   pat = P_SPEED2;   len = P_SPEED2_LEN;   return true;
    case RadioTx::CmdId::Speed3:   pat = P_SPEED3;   len = P_SPEED3_LEN;   return true;
    case RadioTx::CmdId::Speed4:   pat = P_SPEED4;   len = P_SPEED4_LEN;   return true;
    case RadioTx::CmdId::Speed5:   pat = P_SPEED5;   len = P_SPEED5_LEN;   return true;
    case RadioTx::CmdId::Speed6:   pat = P_SPEED6;   len = P_SPEED6_LEN;   return true;
    case RadioTx::CmdId::Rotation: pat = P_ROTATION; len = P_ROTATION_LEN; return true;
    default: return false;
  }
}

bool RadioTx::begin() {
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  int16_t state = radio.begin(RF_FREQ_MHZ, BITRATE_KBPS, FREQDEV_KHZ, RX_BW_KHZ, pwr_dbm_, 16);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin() misslyckades, kod: %d\n", state);
    return false;
  }

  state = radio.setOOK(true);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("setOOK(true) misslyckades, kod: %d\n", state);
    return false;
  }

  state = radio.transmitDirect();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("transmitDirect() misslyckades, kod: %d\n", state);
    return false;
  }

  pinMode(PIN_GDO0, OUTPUT);
  digitalWrite(PIN_GDO0, LOW);

  Serial.printf("RF: %.2f MHz OOK bitrate=%.1f kbps dev=%.1f kHz bw=%.1f kHz\n",
                RF_FREQ_MHZ, BITRATE_KBPS, FREQDEV_KHZ, RX_BW_KHZ);
  return true;
}

bool RadioTx::setTxPowerDbm(int8_t dbm) {
  if (!isAllowedTxPwr(dbm)) return false;

  int16_t state = radio.setOutputPower(dbm);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("setOutputPower(%d) misslyckades, kod: %d\n", (int)dbm, state);
    return false;
  }

  pwr_dbm_ = dbm;
  return true;
}

void RadioTx::setRepeat(uint8_t repeatN) {
  if (repeatN < 1) repeatN = 1;
  if (repeatN > 12) repeatN = 12;
  repeat_ = repeatN;
}

void RadioTx::setGapUs(uint32_t gapUs) {
  if (gapUs < 1000) gapUs = 1000;
  if (gapUs > 30000) gapUs = 30000;
  gap_us_ = gapUs;
}

void RadioTx::setInvert(bool inv) {
  invert_ = inv;
}

void RadioTx::send(CmdId c) {
  const int16_t* pat = nullptr;
  uint16_t len = 0;
  if (!getPattern_(c, pat, len)) return;

  Serial.printf("TX: %s repeat=%u invert=%d gap=%lu us pwr=%d dBm\n",
                cmdName_(c), (unsigned)repeat_, invert_ ? 1 : 0,
                (unsigned long)gap_us_, (int)pwr_dbm_);

  sendPatternPGM(pat, len, repeat_, gap_us_, invert_);
  Serial.println("TX: done");
}
