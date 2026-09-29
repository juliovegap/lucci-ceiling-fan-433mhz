// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson
//
// LUCCI 433 MHz - FREE OOK SNIFFER / DECODER
//
// Protocol learned from RAW captures:
//   - repeated frames
//   - sync/gap around 6.1 ms
//   - Manchester-like OOK symbols:
//       SHORT ~= 380 us
//       LONG  ~= 1130 us
//   - current observed frame: 29 bits
//
// IMPORTANT:
// This program does NOT assume 8 predefined buttons.
// Every different decoded code is stored as a new pattern.
// The timing decoder is deliberately tolerant.
//
// ESP32 + CC1101:
//   SCK  = GPIO18
//   MISO = GPIO19
//   MOSI = GPIO23
//   CS   = GPIO5
//   GDO0 = GPIO4

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// =====================================================
// PIN CONFIG
// =====================================================

#if defined(ARDUINO_ARCH_ESP8266)

static constexpr int PIN_SCK  = 14;
static constexpr int PIN_MISO = 12;
static constexpr int PIN_MOSI = 13;
static constexpr int PIN_CS   = 15;
static constexpr int PIN_GDO0 = 5;

#else

static constexpr int PIN_SCK  = 18;
static constexpr int PIN_MISO = 19;
static constexpr int PIN_MOSI = 23;
static constexpr int PIN_CS   = 5;
static constexpr int PIN_GDO0 = 4;

#endif

// =====================================================
// RADIO
// =====================================================

static constexpr float RF_FREQ_MHZ  = 434.05f;
static constexpr float RX_BW_KHZ    = 250.0f;
static constexpr float BITRATE_KBPS = 4.8f;
static constexpr float FREQDEV_KHZ  = 5.0f;

// =====================================================
// PROTOCOL TIMING
// =====================================================

// Observed from RAW capture:
// sync ~= 6120-6160 us
// short ~= 350-410 us
// long  ~= 1100-1160 us

static constexpr uint32_t SYNC_MIN_US = 5000;
static constexpr uint32_t SYNC_MAX_US = 7500;

static constexpr uint32_t SHORT_MIN_US = 250;
static constexpr uint32_t SHORT_MAX_US = 600;

static constexpr uint32_t LONG_MIN_US = 850;
static constexpr uint32_t LONG_MAX_US = 1400;

// A valid symbol is two consecutive durations.
// Expected:
//   SHORT + LONG  => one bit
//   LONG  + SHORT => the other bit
static constexpr uint8_t MIN_BITS = 8;
static constexpr uint8_t MAX_BITS = 64;

// Number of repeated decoded frames required before
// considering the capture reliable.
static constexpr uint8_t MIN_REPEATS = 2;

// Similar timing tolerance when comparing repetitions.
static constexpr uint8_t MAX_BAD_SYMBOLS = 3;

// -----------------------------------------------------
// Capture trigger validation
// -----------------------------------------------------
// Do NOT start a capture just because several RF edges
// arrive. In idle, interference can easily create many
// edges. We only enter the capture state after finding
// the protocol's long sync gap followed by a sufficiently
// long, valid Manchester-like symbol sequence.
//
// The real remote frame observed here is 29 bits, so 16
// valid bits is deliberately conservative while still
// leaving some tolerance for a partial frame.
static constexpr uint8_t START_MIN_BITS = 16;

// Only inspect the most recent part of the pre-trigger
// buffer. This avoids repeatedly scanning a growing noise
// buffer while the receiver is idle.
static constexpr uint16_t START_SCAN_EDGES = 512;

// Once a valid protocol frame has been detected, keep
// capturing until this much silence is observed.

// =====================================================
// CAPTURE
// =====================================================

#if defined(ARDUINO_ARCH_ESP8266)
static constexpr uint16_t MAX_EDGES = 4096;
#else
static constexpr uint16_t MAX_EDGES = 8192;
#endif

static constexpr uint32_t END_GAP_US = 15000;
static constexpr uint32_t MAX_SESSION_US = 1200000;

static volatile int32_t g_edges[MAX_EDGES];
static volatile uint16_t g_edgeCount = 0;
static volatile uint32_t g_lastEdgeUs = 0;
static volatile bool g_overflow = false;

static int32_t g_copy[MAX_EDGES];
static uint16_t g_copyCount = 0;

// =====================================================
// RADIO
// =====================================================

CC1101 radio = new Module(
  PIN_CS,
  PIN_GDO0,
  RADIOLIB_NC,
  RADIOLIB_NC
);

// =====================================================
// PATTERN DATABASE
// =====================================================

static constexpr uint8_t MAX_PATTERNS = 64;

struct Pattern {
  bool valid = false;

  uint64_t code = 0;
  uint8_t bits = 0;

  uint16_t seen = 0;
  uint16_t bestRepeats = 0;

  float bestRSSI = -120.0f;

  uint32_t lastSeenMs = 0;
};

static Pattern patterns[MAX_PATTERNS];

// =====================================================
// ISR
// =====================================================

void IRAM_ATTR onGdo0Change() {

  uint32_t now = micros();
  uint32_t previous = g_lastEdgeUs;

  g_lastEdgeUs = now;

  uint16_t idx = g_edgeCount;

  if (idx >= MAX_EDGES) {
    g_overflow = true;
    return;
  }

  uint32_t dt = now - previous;

  int32_t signedDt = (int32_t)dt;

#if defined(ARDUINO_ARCH_ESP32)
  if (GPIO.in & (1UL << PIN_GDO0)) {
    signedDt = -signedDt;
  }
#else
  if (digitalRead(PIN_GDO0) == HIGH) {
    signedDt = -signedDt;
  }
#endif

  g_edges[idx] = signedDt;
  g_edgeCount = idx + 1;
}

// =====================================================
// COPY CAPTURE
// =====================================================

static uint16_t copyCapture() {

  noInterrupts();

  uint16_t n = g_edgeCount;

  if (n > MAX_EDGES) {
    n = MAX_EDGES;
  }

  for (uint16_t i = 0; i < n; i++) {
    g_copy[i] = g_edges[i];
  }

  g_edgeCount = 0;
  g_lastEdgeUs = micros();
  g_overflow = false;

  interrupts();

  return n;
}

// =====================================================
// TIMING CLASSIFICATION
// =====================================================

enum PulseClass : uint8_t {
  P_INVALID = 0,
  P_SHORT,
  P_LONG,
  P_SYNC
};

static PulseClass classifyPulse(uint32_t us) {

  if (us >= SYNC_MIN_US && us <= SYNC_MAX_US) {
    return P_SYNC;
  }

  if (us >= SHORT_MIN_US && us <= SHORT_MAX_US) {
    return P_SHORT;
  }

  if (us >= LONG_MIN_US && us <= LONG_MAX_US) {
    return P_LONG;
  }

  return P_INVALID;
}

// =====================================================
// CODE FRAME
// =====================================================

struct DecodedFrame {

  bool valid = false;

  uint64_t code = 0;
  uint8_t bits = 0;

  uint16_t start = 0;
  uint16_t end = 0;

  uint16_t badSymbols = 0;
};

static bool appendBit(
  DecodedFrame& frame,
  bool bit
) {

  if (frame.bits >= MAX_BITS) {
    return false;
  }

  frame.code <<= 1;

  if (bit) {
    frame.code |= 1ULL;
  }

  frame.bits++;

  return true;
}

// =====================================================
// FIND NEXT SYNC
// =====================================================

static int findSync(
  const int32_t* d,
  uint16_t n,
  int start
) {

  for (int i = start; i < (int)n; i++) {

    uint32_t us = abs(d[i]);

    if (
      us >= SYNC_MIN_US &&
      us <= SYNC_MAX_US
    ) {
      return i;
    }
  }

  return -1;
}

// =====================================================
// DECODE FRAME AFTER SYNC
// =====================================================
//
// One bit is represented by two durations:
//
//   SHORT + LONG  -> 0
//   LONG  + SHORT -> 1
//
// The sign is intentionally NOT used to decode the bit.
// The duration pair is what carries the information.
//
// This makes the decoder robust to an inverted GDO0 polarity.
// =====================================================

static DecodedFrame decodeAfterSync(
  const int32_t* d,
  uint16_t n,
  int syncIndex
) {

  DecodedFrame frame;

  frame.start = syncIndex;

  int i = syncIndex + 1;

  while (
    i + 1 < (int)n &&
    frame.bits < MAX_BITS
  ) {

    uint32_t a = abs(d[i]);
    uint32_t b = abs(d[i + 1]);

    PulseClass ca = classifyPulse(a);
    PulseClass cb = classifyPulse(b);

    // Another sync means this frame has ended.
    if (
      ca == P_SYNC ||
      cb == P_SYNC
    ) {
      break;
    }

    if (
      ca == P_SHORT &&
      cb == P_LONG
    ) {

      if (!appendBit(frame, false)) {
        break;
      }

      i += 2;
      continue;
    }

    if (
      ca == P_LONG &&
      cb == P_SHORT
    ) {

      if (!appendBit(frame, true)) {
        break;
      }

      i += 2;
      continue;
    }

    // Ignore small acquisition glitches if possible.
    // We do not immediately abandon the frame.
    frame.badSymbols++;

    if (frame.badSymbols > MAX_BAD_SYMBOLS) {
      break;
    }

    i++;
  }

  frame.end = i;

  frame.valid =
    frame.bits >= MIN_BITS &&
    frame.badSymbols <= MAX_BAD_SYMBOLS;

  return frame;
}

// =====================================================
// DECODE ALL REPETITIONS
// =====================================================

static uint8_t decodeCapture(
  const int32_t* d,
  uint16_t n,
  DecodedFrame* frames,
  uint8_t maxFrames
) {

  uint8_t count = 0;

  int pos = 0;

  while (
    count < maxFrames &&
    pos < (int)n
  ) {

    int sync = findSync(d, n, pos);

    if (sync < 0) {
      break;
    }

    DecodedFrame f =
      decodeAfterSync(
        d,
        n,
        sync
      );

    if (f.valid) {

      frames[count++] = f;

      // Move beyond this frame.
      pos = max(
        sync + 1,
        (int)f.end
      );

    } else {

      pos = sync + 1;
    }
  }

  return count;
}

// =====================================================
// PRINT CODE
// =====================================================

static void printCode(
  uint64_t code,
  uint8_t bits
) {

  Serial.print("0b");

  if (bits == 0) {
    Serial.print("0");
    return;
  }

  for (int i = bits - 1; i >= 0; i--) {
    Serial.print(
      (code & (1ULL << i))
        ? '1'
        : '0'
    );
  }
}

// =====================================================
// COMPARE DECODED FRAMES
// =====================================================

static uint8_t frameDifference(
  const DecodedFrame& a,
  const DecodedFrame& b
) {

  if (a.bits != b.bits) {
    return 255;
  }

  uint8_t different = 0;

  for (uint8_t i = 0; i < a.bits; i++) {

    uint8_t shift =
      a.bits - 1 - i;

    bool ba =
      (a.code & (1ULL << shift)) != 0;

    bool bb =
      (b.code & (1ULL << shift)) != 0;

    if (ba != bb) {
      different++;
    }
  }

  return different;
}

// =====================================================
// PATTERN DATABASE
// =====================================================

static int findPattern(
  uint64_t code,
  uint8_t bits
) {

  for (uint8_t i = 0; i < MAX_PATTERNS; i++) {

    if (!patterns[i].valid) {
      continue;
    }

    if (
      patterns[i].bits == bits &&
      patterns[i].code == code
    ) {
      return i;
    }
  }

  return -1;
}

static int createPattern(
  uint64_t code,
  uint8_t bits,
  uint16_t repeats,
  float rssi
) {

  for (uint8_t i = 0; i < MAX_PATTERNS; i++) {

    if (patterns[i].valid) {
      continue;
    }

    patterns[i].valid = true;
    patterns[i].code = code;
    patterns[i].bits = bits;
    patterns[i].seen = 1;
    patterns[i].bestRepeats = repeats;
    patterns[i].bestRSSI = rssi;
    patterns[i].lastSeenMs = millis();

    return i;
  }

  return -1;
}

static void updatePattern(
  uint8_t p,
  uint16_t repeats,
  float rssi
) {

  patterns[p].seen++;

  if (
    repeats >
    patterns[p].bestRepeats
  ) {
    patterns[p].bestRepeats =
      repeats;
  }

  if (
    rssi >
    patterns[p].bestRSSI
  ) {
    patterns[p].bestRSSI =
      rssi;
  }

  patterns[p].lastSeenMs =
    millis();
}

// =====================================================
// PRINT ONE PATTERN
// =====================================================

static void printPattern(
  uint8_t p
) {

  if (
    p >= MAX_PATTERNS ||
    !patterns[p].valid
  ) {
    return;
  }

  Serial.printf(
    "\nPATTERN #%u\n",
    (unsigned)(p + 1)
  );

  Serial.print("  code     : ");
  printCode(
    patterns[p].code,
    patterns[p].bits
  );
  Serial.println();

  Serial.printf(
    "  hex      : 0x%llX\n",
    (unsigned long long)patterns[p].code
  );

  Serial.printf(
    "  bits     : %u\n",
    (unsigned)patterns[p].bits
  );

  Serial.printf(
    "  seen     : %u sessions\n",
    (unsigned)patterns[p].seen
  );

  Serial.printf(
    "  repeats  : %u\n",
    (unsigned)patterns[p].bestRepeats
  );

  Serial.printf(
    "  bestRSSI : %.1f dBm\n",
    patterns[p].bestRSSI
  );
}

// =====================================================
// PRINT ALL PATTERNS
// =====================================================

static void printPatterns() {

  Serial.println();
  Serial.println(
    "================ PATTERNS ================"
  );

  uint8_t count = 0;

  for (uint8_t i = 0; i < MAX_PATTERNS; i++) {

    if (!patterns[i].valid) {
      continue;
    }

    printPattern(i);
    count++;
  }

  Serial.printf(
    "\nTotal patterns: %u\n",
    (unsigned)count
  );

  Serial.println(
    "==========================================="
  );
}

// =====================================================
// CLEAR PATTERNS
// =====================================================

static void clearPatterns() {

  for (uint8_t i = 0; i < MAX_PATTERNS; i++) {
    patterns[i] = Pattern{};
  }

  Serial.println("PATTERNS CLEARED.");
}

// =====================================================
// RAW DUMP
// =====================================================

static void dumpRaw() {

  if (g_copyCount == 0) {
    Serial.println("RAW: no capture available.");
    return;
  }

  Serial.printf(
    "\n================ RAW (%u edges) ================\n",
    (unsigned)g_copyCount
  );

  for (uint16_t i = 0; i < g_copyCount; i++) {

    Serial.printf(
      "%ld",
      (long)g_copy[i]
    );

    if (i + 1 < g_copyCount) {
      Serial.print(", ");
    }

    if ((i % 10) == 9) {
      Serial.println();
    }
  }

  Serial.println();
  Serial.println(
    "================================================="
  );
}

// =====================================================
// ANALYZE CAPTURE
// =====================================================

static void analyzeCapture(
  float rssi
) {

  DecodedFrame frames[32];

  uint8_t frameCount =
    decodeCapture(
      g_copy,
      g_copyCount,
      frames,
      32
    );

  Serial.println();
  Serial.println(
    "================ DECODE ================"
  );

  Serial.printf(
    "raw edges : %u\n",
    (unsigned)g_copyCount
  );

  Serial.printf(
    "frames    : %u\n",
    (unsigned)frameCount
  );

  if (frameCount == 0) {

    Serial.println(
      "No valid decoded frames."
    );

    Serial.println(
      "Use W to inspect RAW."
    );

    Serial.println(
      "======================================="
    );

    return;
  }

  // ---------------------------------------------------
  // Find the most repeated decoded code.
  // ---------------------------------------------------

  uint8_t bestIndex = 0;
  uint8_t bestRepeats = 0;

  for (uint8_t i = 0; i < frameCount; i++) {

    uint8_t repeats = 0;

    for (uint8_t j = 0; j < frameCount; j++) {

      if (
        frameDifference(
          frames[i],
          frames[j]
        ) <= 0
      ) {
        repeats++;
      }
    }

    if (repeats > bestRepeats) {
      bestRepeats = repeats;
      bestIndex = i;
    }
  }

  DecodedFrame& best =
    frames[bestIndex];

  Serial.printf(
    "best repeats : %u/%u\n",
    (unsigned)bestRepeats,
    (unsigned)frameCount
  );

  Serial.print(
    "decoded code : "
  );

  printCode(
    best.code,
    best.bits
  );

  Serial.println();

  Serial.printf(
    "hex          : 0x%llX\n",
    (unsigned long long)best.code
  );

  Serial.printf(
    "bits         : %u\n",
    (unsigned)best.bits
  );

  // ---------------------------------------------------
  // Print all decoded repetitions.
  // This is useful while developing the protocol.
  // ---------------------------------------------------

  Serial.println();
  Serial.println("Decoded repetitions:");

  for (uint8_t i = 0; i < frameCount; i++) {

    Serial.printf(
      "  #%02u  ",
      (unsigned)(i + 1)
    );

    printCode(
      frames[i].code,
      frames[i].bits
    );

    Serial.printf(
      "  bad=%u\n",
      (unsigned)frames[i].badSymbols
    );
  }

  // ---------------------------------------------------
  // Pattern database
  // ---------------------------------------------------

  int p =
    findPattern(
      best.code,
      best.bits
    );

  if (p >= 0) {

    updatePattern(
      (uint8_t)p,
      bestRepeats,
      rssi
    );

    Serial.printf(
      "\nPATTERN MATCH: #%d seen=%u repeats=%u\n",
      p + 1,
      (unsigned)patterns[p].seen,
      (unsigned)bestRepeats
    );

  } else {

    p =
      createPattern(
        best.code,
        best.bits,
        bestRepeats,
        rssi
      );

    if (p >= 0) {

      Serial.printf(
        "\nNEW PATTERN: #%d\n",
        p + 1
      );

      printPattern(
        (uint8_t)p
      );

    } else {

      Serial.println(
        "\nPATTERN TABLE FULL."
      );
    }
  }

  Serial.println(
    "========================================"
  );
}

// =====================================================
// HELP
// =====================================================

static void printHelp() {

  Serial.println();
  Serial.println(
    "=============================================="
  );
  Serial.println(
    "       LUCCI 433 MHz FREE OOK SNIFFER"
  );
  Serial.println(
    "=============================================="
  );

  Serial.println();
  Serial.println(
    "No predefined buttons."
  );

  Serial.println(
    "Press any remote key any number of times."
  );

  Serial.println();
  Serial.println("COMMANDS:");

  Serial.println(
    "  h = help"
  );

  Serial.println(
    "  P = print detected patterns"
  );

  Serial.println(
    "  R = clear detected patterns"
  );

  Serial.println(
    "  W = dump last RAW capture"
  );

  Serial.println();
  Serial.println("PROTOCOL:");

  Serial.printf(
    "  sync  : %lu-%lu us\n",
    (unsigned long)SYNC_MIN_US,
    (unsigned long)SYNC_MAX_US
  );

  Serial.printf(
    "  short : %lu-%lu us\n",
    (unsigned long)SHORT_MIN_US,
    (unsigned long)SHORT_MAX_US
  );

  Serial.printf(
    "  long  : %lu-%lu us\n",
    (unsigned long)LONG_MIN_US,
    (unsigned long)LONG_MAX_US
  );

  Serial.printf(
    "  bits  : %u-%u\n",
    (unsigned)MIN_BITS,
    (unsigned)MAX_BITS
  );

  Serial.println();
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println();
  Serial.println(
    "LUCCI 433 MHz OOK SNIFFER"
  );

#if defined(ARDUINO_ARCH_ESP8266)

  SPI.begin();

#else

  SPI.begin(
    PIN_SCK,
    PIN_MISO,
    PIN_MOSI,
    PIN_CS
  );

#endif

  int16_t state =
    radio.begin(
      RF_FREQ_MHZ,
      BITRATE_KBPS,
      FREQDEV_KHZ,
      RX_BW_KHZ,
      10,
      16
    );

  if (
    state != RADIOLIB_ERR_NONE
  ) {

    Serial.printf(
      "radio.begin() failed: %d\n",
      state
    );

    while (true) {
      delay(1000);
    }
  }

  state =
    radio.setOOK(true);

  if (
    state != RADIOLIB_ERR_NONE
  ) {

    Serial.printf(
      "setOOK() failed: %d\n",
      state
    );
  }

  state =
    radio.setPromiscuousMode(true);

  if (
    state != RADIOLIB_ERR_NONE
  ) {

    Serial.printf(
      "setPromiscuousMode() failed: %d\n",
      state
    );
  }

  state =
    radio.receiveDirectAsync();

  if (
    state != RADIOLIB_ERR_NONE
  ) {

    Serial.printf(
      "receiveDirectAsync() failed: %d\n",
      state
    );

    while (true) {
      delay(1000);
    }
  }

  pinMode(
    PIN_GDO0,
    INPUT
  );

  noInterrupts();

  g_edgeCount = 0;
  g_lastEdgeUs = micros();
  g_overflow = false;

  interrupts();

  attachInterrupt(
    digitalPinToInterrupt(PIN_GDO0),
    onGdo0Change,
    CHANGE
  );

  printHelp();

  Serial.println();
  Serial.println(
    "READY - press any remote button."
  );
}

// =====================================================
// PROTOCOL START DETECTION
// =====================================================
//
// A burst is considered real only when the captured edge
// history already contains:
//   SYNC (about 6.1 ms)
//   + at least START_MIN_BITS valid Manchester-like bits
//
// This function is deliberately independent of RSSI.
// RSSI is useful as information, but it is not a reliable
// trigger because local 433 MHz interference can be strong.
//
// IMPORTANT:
// This function is called while interrupts are disabled by
// the caller, so the ISR cannot modify g_edges halfway
// through the inspection.
// =====================================================

static bool hasValidProtocolStart(uint16_t count) {

  if (count < 3) {
    return false;
  }

  uint16_t first = 0;

  if (count > START_SCAN_EDGES) {
    first = count - START_SCAN_EDGES;
  }

  for (uint16_t i = first; i < count; i++) {

    uint32_t us = abs(g_edges[i]);

    if (
      us < SYNC_MIN_US ||
      us > SYNC_MAX_US
    ) {
      continue;
    }

    DecodedFrame frame =
      decodeAfterSync(
        (const int32_t*)g_edges,
        count,
        i
      );

    if (
      frame.valid &&
      frame.bits >= START_MIN_BITS
    ) {
      return true;
    }
  }

  return false;
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  static bool capturing = false;
  static uint32_t captureStartUs = 0;

  uint32_t now = micros();

  uint16_t count;
  uint32_t lastEdge;
  bool overflow;

  noInterrupts();

  count = g_edgeCount;
  lastEdge = g_lastEdgeUs;
  overflow = g_overflow;

  interrupts();

  // ---------------------------------------------------
  // Wait for the actual protocol, not just RF activity.
  //
  // In idle there can be plenty of 433 MHz edges. The old
  // implementation started a capture after only 8 edges,
  // which made the sniffer "capture by itself".
  //
  // We now wait until the edge history contains a real
  // Lucci-like sync gap followed by a valid symbol stream.
  // ---------------------------------------------------

  if (!capturing) {

    // If the idle buffer becomes full because of continuous
    // interference, discard it and start looking again.
    if (count >= MAX_EDGES - 1) {

      noInterrupts();

      g_edgeCount = 0;
      g_lastEdgeUs = micros();
      g_overflow = false;

      interrupts();

      delay(1);
      return;
    }

    bool protocolDetected = false;

    // Protect the edge buffer while hasValidProtocolStart()
    // inspects it. The scan is short (at most 512 edges).
    noInterrupts();

    uint16_t scanCount = g_edgeCount;

    if (scanCount >= START_MIN_BITS * 2 + 1) {
      protocolDetected =
        hasValidProtocolStart(scanCount);
    }

    interrupts();

    if (protocolDetected) {

      capturing = true;
      captureStartUs = now;

      Serial.printf(
        "\nCAPTURE START: protocol detected (edges=%u)\n",
        (unsigned)scanCount
      );
    }

    delay(1);
    return;
  }

  // ---------------------------------------------------
  // Capture until there is a long idle period.
  // ---------------------------------------------------

  bool timedOut =
    (uint32_t)(
      now - lastEdge
    ) > END_GAP_US;

  bool maxTime =
    (uint32_t)(
      now - captureStartUs
    ) > MAX_SESSION_US;

  bool full =
    count >= MAX_EDGES - 1;

  if (
    !timedOut &&
    !maxTime &&
    !full
  ) {

    delay(1);
    return;
  }

  // ---------------------------------------------------
  // Copy capture.
  // ---------------------------------------------------

  g_copyCount =
    copyCapture();

  capturing = false;
  Serial.printf(
    "\nCAPTURE END edges=%u",
    (unsigned)g_copyCount
  );

  if (timedOut) {
    Serial.print(" reason=GAP");
  }

  if (maxTime) {
    Serial.print(" reason=MAX_TIME");
  }

  if (full || overflow) {
    Serial.print(" reason=BUFFER_FULL");
  }

  Serial.println();

  float rssi =
    radio.getRSSI();

  Serial.printf(
    "RSSI: %.1f dBm\n",
    rssi
  );

  analyzeCapture(rssi);

  Serial.println();
  Serial.println(
    "Press W for RAW or P for patterns."
  );
  Serial.println(
    "READY - press another key."
  );

  delay(100);
}

// =====================================================
// END
// =====================================================
