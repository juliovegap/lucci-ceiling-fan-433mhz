// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2025-2026 Erikxson


// tools/sniffer/main.cpp
/*
  ============================================================
  CC1101 OOK DIRECT SNIFFER + GUIDED LEARN + EXPORT (P_* ARRAYS)
  ============================================================

  FUNCTION / WORKFLOW
  -------------------
  This firmware runs two phases in one program:

  1) GUIDED LEARN (learn a hash per button)
     - You press OFF, SPEED1, ... SPEED6, ROTATION in the order the program asks for.
     - Each button press forms a "session" (edge-capture) which is split into frames.
     - The program selects the "best frame" and counts how many times the same hash appears in the session: hits.
     - For a session to be accepted for learning, it must satisfy:
         a) hits >= MIN_HITS_REQUIRED (default 8, adjustable with I/i)
         b) the same hash must be seen in at least 3 of the last 5 accepted sessions (stability)
     - When a button is learned, its hash is stored in learnedCmd[].

  2) EXPORT (capture P_* arrays)
     - When all 8 buttons are learned: press buttons in any order.
     - For each session, the program identifies which button it was via the learned hash.
     - If hits >= MIN_HITS_REQUIRED, the program can store a "capture" (P_* array).
     - A capture replaces an earlier one if:
         a) it has more hits, or
         b) same hits but stronger peakRSSI.
     - When all 8 are captured, you can print a complete patterns.h.

  IMPORTANT: No Enter needed
  --------------------------
  All control is done via single key presses directly in the Serial Monitor.

  KEYS (COMMANDS)
  --------------
  h  = help + show configuration
  r  = reset guided learn (start over from OFF)
  p  = print learned table (hashes)
  C  = clear export captures (clear stored P_* arrays)
  x  = dump export status (OK/MISSING + hits/peak/len)
  a  = dump all P_* arrays (for those captured)
  X  = export complete patterns.h block (requires captures)

  KEYS (ADJUST FILTERS/THRESHOLDS) – no Enter
  -------------------------------------------
  Principle: Uppercase = increase, lowercase = decrease.

  S / s : RSSI_START_DBM         (+/- 1 dB)
  G / g : IDLE_GATE_MIN_RSSI_DBM (+/- 1 dB)
  E / e : EDGE_BURST_START       (+/- 10)
  T / t : END_GAP_US             (+/- 250 µs)
  F / f : FRAME_GAP_US           (+/- 100 µs)
  M / m : MIN_FRAME_LEN          (+/- 1)
  N / n : MAX_FRAME_LEN          (+/- 5)   (clamped to 1..200)
  K / k : COOLDOWN_MS            (+/- 50 ms)
  I / i : MIN_HITS_REQUIRED      (+/- 1)   (clamped to 1..32)
  D     : toggle debug heartbeat on/off

  HOW THE SIGNAL IS CAPTURED
  --------------------------
  - CC1101 runs in OOK + promiscuous + receiveDirectAsync().
  - Interrupt on GDO0 captures edge times (dt in µs) and stores them with sign (level info).
  - In idle, edges are captured only if RSSI >= IDLE_GATE_MIN_RSSI_DBM (noise gate).
  - A session starts at strong RSSI + edge activity and ends after silence (END_GAP_US).

  SAFETY / LEGAL
  --------------
  Use only on equipment you own or have permission to test.

*/

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>

// =====================================================
// PIN CONFIG (ESP32 + CC1101)
// =====================================================
static constexpr int PIN_SCK  = 18;
static constexpr int PIN_MISO = 19;
static constexpr int PIN_MOSI = 23;
static constexpr int PIN_CS   = 5;
static constexpr int PIN_GDO0 = 4;

// =====================================================
// RADIO PARAMETERS
// =====================================================
static constexpr float RF_FREQ_MHZ   = 434.05f;
static constexpr float RX_BW_KHZ     = 135.0f;
static constexpr float BITRATE_KBPS  = 4.8f;
static constexpr float FREQDEV_KHZ   = 5.0f;

// =====================================================
// RUNTIME CONFIG (adjustable via key presses)
// =====================================================
static float    RSSI_START_DBM          = -70.0f;  // S/s
static float    IDLE_GATE_MIN_RSSI_DBM  = -70.0f;  // G/g
static uint16_t EDGE_BURST_START        = 220;     // E/e
static uint32_t END_GAP_US              = 9000;    // T/t
static uint32_t FRAME_GAP_US            = 2000;    // F/f
static uint16_t MIN_FRAME_LEN           = 25;      // M/m
static uint16_t MAX_FRAME_LEN           = 200;     // N/n (<=200)
static uint32_t COOLDOWN_MS             = 700;     // K/k
static uint8_t  MIN_HITS_REQUIRED       = 8;       // I/i
static bool     DBG_HEARTBEAT           = false;   // D toggle

static constexpr uint32_t DBG_EVERY_MS  = 1000;

// =====================================================
// BUFFERS / CAP LIMITS
// =====================================================
static constexpr uint16_t MAX_EDGES      = 8192;
static constexpr uint16_t MAX_FRAME_CAP  = 200;   // capPulse dimension

// =====================================================
// EDGE BUFFER (ISR)
// =====================================================
static volatile int32_t  g_edges[MAX_EDGES];
static volatile uint16_t g_edgeCount = 0;
static volatile uint32_t g_lastEdgeUs = 0;
static volatile bool     g_captureEnabled = false;

// Copy after session
static int32_t  g_copy[MAX_EDGES];
static uint16_t g_copyCount = 0;

// CC1101 (GDO2/RST not connected -> RADIOLIB_NC)
CC1101 radio = new Module(PIN_CS, PIN_GDO0, RADIOLIB_NC, RADIOLIB_NC);

// =====================================================
// GUIDED LEARN + EXPORT STATE
// =====================================================
static const char* LABELS[8] = {
  "OFF", "SPEED1", "SPEED2", "SPEED3", "SPEED4", "SPEED5", "SPEED6", "ROTATION"
};

static uint8_t  learnIndex = 0;
static uint32_t learnedCmd[8] = {0};
static bool     learnedOk[8]  = {false};

// Rolling window for “3 matching” acceptance
static uint32_t recentCmd[5] = {0};
static uint8_t  recentPos = 0;

// Export storage
static int16_t   capPulse[8][MAX_FRAME_CAP];
static uint16_t  capLen[8]  = {0};
static float     capPeak[8] = {-120,-120,-120,-120,-120,-120,-120,-120};
static uint8_t   capHits[8] = {0};
static bool      capOk[8]   = {false};

// =====================================================
// Hash helpers
// =====================================================
static uint32_t fnv1a32_update(uint32_t h, uint8_t b) {
  h ^= b;
  h *= 16777619u;
  return h;
}

static uint16_t median_u16(uint16_t* a, uint16_t n) {
  for (uint16_t i = 0; i < n; i++) {
    for (uint16_t j = i + 1; j < n; j++) {
      if (a[j] < a[i]) {
        uint16_t t = a[i];
        a[i] = a[j];
        a[j] = t;
      }
    }
  }
  return a[n / 2];
}

// Hash a frame via symbolization: short/long/gap + level bit.
static uint32_t hash_frame_seq(const int32_t* d, uint16_t len, uint16_t* outUnit) {
  static constexpr uint16_t MAX_SYM = 120;

  uint16_t mags[200];
  uint16_t magsN = 0;

  for (uint16_t i = 0; i < len && magsN < 200; i++) {
    uint32_t m = (uint32_t)abs(d[i]);
    if (m >= 60 && m <= 500) mags[magsN++] = (uint16_t)m;
  }

  uint16_t unit = 0;
  if (magsN >= 7) unit = median_u16(mags, magsN);
  else unit = 180;
  if (outUnit) *outUnit = unit;

  float thrLong = unit * 1.6f;

  uint32_t h = 0x811C9DC5u;
  uint16_t used = 0;

  for (uint16_t i = 0; i < len && used < MAX_SYM; i++) {
    int32_t v = d[i];
    uint32_t m = (uint32_t)abs(v);

    uint8_t sym = 0;
    if (m > FRAME_GAP_US) sym = 3;
    else if (m > (uint32_t)thrLong) sym = 2;
    else sym = 1;

    uint8_t lvl = (v < 0) ? 1 : 0;
    uint8_t packed = (uint8_t)((sym & 0x03) | (lvl ? 0x04 : 0x00));
    h = fnv1a32_update(h, packed);
    used++;
  }

  // mix in length
  h = fnv1a32_update(h, (uint8_t)(len & 0xFF));
  h = fnv1a32_update(h, (uint8_t)((len >> 8) & 0xFF));
  return h;
}

// =====================================================
// UI / status
// =====================================================
static void print_config() {
  Serial.println();
  Serial.println("=== CONFIG ===");
  Serial.printf("S/s RSSI_START_DBM         = %.1f dBm\n", RSSI_START_DBM);
  Serial.printf("G/g IDLE_GATE_MIN_RSSI_DBM = %.1f dBm\n", IDLE_GATE_MIN_RSSI_DBM);
  Serial.printf("E/e EDGE_BURST_START       = %u\n", (unsigned)EDGE_BURST_START);
  Serial.printf("T/t END_GAP_US             = %lu\n", (unsigned long)END_GAP_US);
  Serial.printf("F/f FRAME_GAP_US           = %lu\n", (unsigned long)FRAME_GAP_US);
  Serial.printf("M/m MIN_FRAME_LEN          = %u\n", (unsigned)MIN_FRAME_LEN);
  Serial.printf("N/n MAX_FRAME_LEN          = %u (cap=%u)\n", (unsigned)MAX_FRAME_LEN, (unsigned)MAX_FRAME_CAP);
  Serial.printf("K/k COOLDOWN_MS            = %lu\n", (unsigned long)COOLDOWN_MS);
  Serial.printf("I/i MIN_HITS_REQUIRED      = %u\n", (unsigned)MIN_HITS_REQUIRED);
  Serial.printf("D   DBG_HEARTBEAT          = %u\n", DBG_HEARTBEAT ? 1 : 0);
  Serial.println("==============");
}

static void print_help() {
  Serial.println();
  Serial.println("Commands (direct, no Enter):");
  Serial.println("  h  = help + config");
  Serial.println("  r  = reset learn");
  Serial.println("  p  = print learned table (hashes)");
  Serial.println("  C  = clear export captures");
  Serial.println("  x  = export status (OK/MISSING)");
  Serial.println("  a  = dump all P_* (captured)");
  Serial.println("  X  = export complete patterns.h block");
  Serial.println();
  Serial.println("Adjust (Uppercase=increase, lowercase=decrease):");
  Serial.println("  S/s  RSSI_START_DBM         +/- 1 dB");
  Serial.println("  G/g  IDLE_GATE_MIN_RSSI_DBM +/- 1 dB");
  Serial.println("  E/e  EDGE_BURST_START       +/- 10");
  Serial.println("  T/t  END_GAP_US             +/- 250 us");
  Serial.println("  F/f  FRAME_GAP_US           +/- 100 us");
  Serial.println("  M/m  MIN_FRAME_LEN          +/- 1");
  Serial.println("  N/n  MAX_FRAME_LEN          +/- 5 (<=200)");
  Serial.println("  K/k  COOLDOWN_MS            +/- 50 ms");
  Serial.println("  I/i  MIN_HITS_REQUIRED      +/- 1 (1..32)");
  Serial.println("  D    toggle DBG_HEARTBEAT");
  print_config();
}

static void print_learned_table() {
  Serial.println();
  Serial.println("=== LEARNED COMMANDS ===");
  for (uint8_t i = 0; i < 8; i++) {
    Serial.printf("%-9s : %s0x%08lX\n",
                  LABELS[i],
                  learnedOk[i] ? "" : "(MISSING) ",
                  (unsigned long)learnedCmd[i]);
  }
  Serial.println("========================");
}

static void print_export_status() {
  Serial.println();
  Serial.println("=== EXPORT STATUS ===");
  for (uint8_t i = 0; i < 8; i++) {
    if (!capOk[i]) Serial.printf("%-9s : MISSING\n", LABELS[i]);
    else Serial.printf("%-9s : OK (len=%u, peak=%.1f dBm, hits=%u)\n",
                       LABELS[i], (unsigned)capLen[i], capPeak[i], (unsigned)capHits[i]);
  }
  Serial.println("=====================");
}

static void dump_one_capture(uint8_t i) {
  if (!capOk[i]) return;

  Serial.println();
  Serial.printf("// %s (hash 0x%08lX)\n", LABELS[i], (unsigned long)learnedCmd[i]);
  Serial.printf("const int16_t P_%s[] PROGMEM = {\n", LABELS[i]);

  for (uint16_t k = 0; k < capLen[i]; k++) {
    Serial.printf("%d", (int)capPulse[i][k]);
    if (k + 1 < capLen[i]) Serial.print(", ");
    if ((k % 8) == 7) Serial.println();
  }
  Serial.println("\n};");
  Serial.printf("const uint16_t P_%s_LEN = %u;\n", LABELS[i], (unsigned)capLen[i]);
}

static void dump_all_captures() {
  for (uint8_t i = 0; i < 8; i++) dump_one_capture(i);
  print_export_status();
}

static void clear_all_captures() {
  for (uint8_t i = 0; i < 8; i++) {
    capOk[i] = false;
    capLen[i] = 0;
    capPeak[i] = -120.0f;
    capHits[i] = 0;
  }
  Serial.println("CAPTURE CLEAR: all export arrays cleared.");
}

static void reset_learning() {
  learnIndex = 0;
  for (uint8_t i = 0; i < 8; i++) {
    learnedCmd[i] = 0;
    learnedOk[i] = false;
  }
  for (uint8_t i = 0; i < 5; i++) recentCmd[i] = 0;
  recentPos = 0;

  Serial.println();
  Serial.println("LEARN reset.");
  Serial.printf("Press %s now...\n", LABELS[learnIndex]);
}

// Accept a hash only when seen 3 times in the last 5 accepted sessions.
static bool learn_accept(uint32_t cmd) {
  recentCmd[recentPos] = cmd;
  recentPos = (recentPos + 1) % 5;

  uint8_t cnt = 0;
  for (uint8_t i = 0; i < 5; i++) if (recentCmd[i] == cmd) cnt++;
  return (cnt >= 3);
}

// =====================================================
// Interrupt: edge capture (GDO0)
// =====================================================
void IRAM_ATTR onGdo0Change() {
  if (!g_captureEnabled) return;

  uint32_t now = micros();
  uint32_t dt  = now - g_lastEdgeUs;
  g_lastEdgeUs = now;

  uint16_t idx = g_edgeCount;
  if (idx >= MAX_EDGES) return;

  int32_t sdt = (int32_t)dt;
  if (digitalRead(PIN_GDO0) == HIGH) sdt = -sdt;

  g_edges[idx] = sdt;
  g_edgeCount = idx + 1;
}

// =====================================================
// Process capture: find "best" frame and hits
// =====================================================
struct BestFrame {
  uint32_t hash = 0;
  uint16_t unit = 0;
  uint16_t len  = 0;
  uint8_t  hits = 0;
  uint16_t start = 0;
};

static BestFrame find_best_frame() {
  struct Frame {
    uint16_t start;
    uint16_t end;
    uint32_t hash;
    uint16_t unit;
  };

  Frame frames[32];
  uint8_t frameN = 0;

  const uint16_t maxFrameLen = (MAX_FRAME_LEN > MAX_FRAME_CAP) ? MAX_FRAME_CAP : MAX_FRAME_LEN;

  uint16_t i = 0;
  while (i < g_copyCount && frameN < 32) {
    while (i < g_copyCount && (uint32_t)abs(g_copy[i]) > FRAME_GAP_US) i++;
    if (i >= g_copyCount) break;

    uint16_t start = i;
    while (i < g_copyCount && (uint32_t)abs(g_copy[i]) <= FRAME_GAP_US) i++;
    uint16_t end = i;

    uint16_t len = end - start;
    if (len >= MIN_FRAME_LEN && len <= maxFrameLen) {
      uint16_t unit = 0;
      uint32_t h = hash_frame_seq(&g_copy[start], len, &unit);
      frames[frameN++] = { start, end, h, unit };
    }
  }

  BestFrame best;
  if (frameN == 0) return best;

  for (uint8_t a = 0; a < frameN; a++) {
    uint8_t hits = 0;
    for (uint8_t b = 0; b < frameN; b++) {
      if (frames[b].hash == frames[a].hash) hits++;
    }
    uint16_t len = frames[a].end - frames[a].start;

    if (hits > best.hits ||
        (hits == best.hits && len > best.len) ||
        (hits == best.hits && len == best.len && frames[a].unit > best.unit)) {
      best.hits  = hits;
      best.hash  = frames[a].hash;
      best.unit  = frames[a].unit;
      best.len   = len;
      best.start = frames[a].start;
    }
  }

  return best;
}

static int find_cmd_index(uint32_t hash) {
  for (int i = 0; i < 8; i++) {
    if (learnedOk[i] && learnedCmd[i] == hash) return i;
  }
  return -1;
}

static bool should_replace_capture(uint8_t idx, const BestFrame& bf, float peakRssi) {
  if (!capOk[idx]) return true;
  if (bf.hits > capHits[idx]) return true;
  if (bf.hits == capHits[idx] && peakRssi > capPeak[idx]) return true;
  return false;
}

static void store_capture(uint8_t idx, const BestFrame& bf, float peakRssi) {
  capLen[idx]  = bf.len;
  capPeak[idx] = peakRssi;
  capHits[idx] = bf.hits;
  capOk[idx]   = true;

  for (uint16_t k = 0; k < bf.len && k < MAX_FRAME_CAP; k++) {
    capPulse[idx][k] = (int16_t)g_copy[bf.start + k];
  }
}

// =====================================================
// Export: patterns.h block
// =====================================================
static void export_patterns_h() {
  Serial.println();
  Serial.println("/* ===== patterns.h (AUTO-EXPORT) ===== */");
  Serial.println("#pragma once");
  Serial.println("#include <Arduino.h>");
  Serial.println("#include <pgmspace.h>");
  Serial.println();

  Serial.println("enum Cmd : uint8_t {");
  Serial.println("  CMD_OFF = 0,");
  Serial.println("  CMD_SPEED1,");
  Serial.println("  CMD_SPEED2,");
  Serial.println("  CMD_SPEED3,");
  Serial.println("  CMD_SPEED4,");
  Serial.println("  CMD_SPEED5,");
  Serial.println("  CMD_SPEED6,");
  Serial.println("  CMD_ROTATION,");
  Serial.println("  CMD_MAX");
  Serial.println("};");
  Serial.println();

  for (uint8_t i = 0; i < 8; i++) dump_one_capture(i);

  Serial.println();
  Serial.println("static const char* CMD_NAME[CMD_MAX] = {");
  Serial.println("  \"OFF\",\"SPEED1\",\"SPEED2\",\"SPEED3\",\"SPEED4\",\"SPEED5\",\"SPEED6\",\"ROTATION\"");
  Serial.println("};");
  Serial.println();

  Serial.println("static const int16_t* CMD_PAT[CMD_MAX] = {");
  Serial.println("  P_OFF, P_SPEED1, P_SPEED2, P_SPEED3, P_SPEED4, P_SPEED5, P_SPEED6, P_ROTATION");
  Serial.println("};");
  Serial.println();

  Serial.println("static const uint16_t CMD_LEN[CMD_MAX] = {");
  Serial.println("  P_OFF_LEN, P_SPEED1_LEN, P_SPEED2_LEN, P_SPEED3_LEN, P_SPEED4_LEN, P_SPEED5_LEN, P_SPEED6_LEN, P_ROTATION_LEN");
  Serial.println("};");
  Serial.println("/* ===== end patterns.h ===== */");
  Serial.println();

  bool all = true;
  for (uint8_t i = 0; i < 8; i++) if (!capOk[i]) all = false;
  if (!all) {
    Serial.println("NOTE: Not all 8 are captured yet (run 'x' for status).");
  }
}

// =====================================================
// Adjustments (key-driven, no Enter)
// =====================================================
static void clamp_all_() {
  if (MAX_FRAME_LEN < 1) MAX_FRAME_LEN = 1;
  if (MAX_FRAME_LEN > MAX_FRAME_CAP) MAX_FRAME_LEN = MAX_FRAME_CAP;

  if (MIN_FRAME_LEN < 1) MIN_FRAME_LEN = 1;
  if (MIN_FRAME_LEN > MAX_FRAME_LEN) MIN_FRAME_LEN = MAX_FRAME_LEN;

  if (EDGE_BURST_START < 1) EDGE_BURST_START = 1;

  if (END_GAP_US < 1000) END_GAP_US = 1000;
  if (FRAME_GAP_US < 500) FRAME_GAP_US = 500;

  if (MIN_HITS_REQUIRED < 1) MIN_HITS_REQUIRED = 1;
  if (MIN_HITS_REQUIRED > 32) MIN_HITS_REQUIRED = 32;
}

static void handle_key(char c) {
  // Commands
  if (c == 'h') { print_help(); return; }
  if (c == 'r') { reset_learning(); return; }
  if (c == 'p') { print_learned_table(); return; }
  if (c == 'C') { clear_all_captures(); return; }
  if (c == 'x') { print_export_status(); return; }
  if (c == 'a') { dump_all_captures(); return; }
  if (c == 'X') { export_patterns_h(); return; }
  if (c == 'D') { DBG_HEARTBEAT = !DBG_HEARTBEAT; Serial.printf("DBG_HEARTBEAT=%u\n", DBG_HEARTBEAT ? 1 : 0); return; }

  // Adjustments (Uppercase increase, lowercase decrease)
  switch (c) {
    case 'S': RSSI_START_DBM += 1.0f; Serial.printf("RSSI_START_DBM=%.1f\n", RSSI_START_DBM); break;
    case 's': RSSI_START_DBM -= 1.0f; Serial.printf("RSSI_START_DBM=%.1f\n", RSSI_START_DBM); break;

    case 'G': IDLE_GATE_MIN_RSSI_DBM += 1.0f; Serial.printf("IDLE_GATE_MIN_RSSI_DBM=%.1f\n", IDLE_GATE_MIN_RSSI_DBM); break;
    case 'g': IDLE_GATE_MIN_RSSI_DBM -= 1.0f; Serial.printf("IDLE_GATE_MIN_RSSI_DBM=%.1f\n", IDLE_GATE_MIN_RSSI_DBM); break;

    case 'E': EDGE_BURST_START = (uint16_t)(EDGE_BURST_START + 10); Serial.printf("EDGE_BURST_START=%u\n", (unsigned)EDGE_BURST_START); break;
    case 'e': EDGE_BURST_START = (EDGE_BURST_START > 10) ? (uint16_t)(EDGE_BURST_START - 10) : 1; Serial.printf("EDGE_BURST_START=%u\n", (unsigned)EDGE_BURST_START); break;

    case 'T': END_GAP_US += 250; Serial.printf("END_GAP_US=%lu\n", (unsigned long)END_GAP_US); break;
    case 't': END_GAP_US = (END_GAP_US > 250) ? (END_GAP_US - 250) : 1000; Serial.printf("END_GAP_US=%lu\n", (unsigned long)END_GAP_US); break;

    case 'F': FRAME_GAP_US += 100; Serial.printf("FRAME_GAP_US=%lu\n", (unsigned long)FRAME_GAP_US); break;
    case 'f': FRAME_GAP_US = (FRAME_GAP_US > 100) ? (FRAME_GAP_US - 100) : 500; Serial.printf("FRAME_GAP_US=%lu\n", (unsigned long)FRAME_GAP_US); break;

    case 'M': MIN_FRAME_LEN = (uint16_t)(MIN_FRAME_LEN + 1); Serial.printf("MIN_FRAME_LEN=%u\n", (unsigned)MIN_FRAME_LEN); break;
    case 'm': MIN_FRAME_LEN = (MIN_FRAME_LEN > 1) ? (uint16_t)(MIN_FRAME_LEN - 1) : 1; Serial.printf("MIN_FRAME_LEN=%u\n", (unsigned)MIN_FRAME_LEN); break;

    case 'N': MAX_FRAME_LEN = (uint16_t)(MAX_FRAME_LEN + 5); Serial.printf("MAX_FRAME_LEN=%u\n", (unsigned)MAX_FRAME_LEN); break;
    case 'n': MAX_FRAME_LEN = (MAX_FRAME_LEN > 5) ? (uint16_t)(MAX_FRAME_LEN - 5) : 1; Serial.printf("MAX_FRAME_LEN=%u\n", (unsigned)MAX_FRAME_LEN); break;

    case 'K': COOLDOWN_MS += 50; Serial.printf("COOLDOWN_MS=%lu\n", (unsigned long)COOLDOWN_MS); break;
    case 'k': COOLDOWN_MS = (COOLDOWN_MS > 50) ? (COOLDOWN_MS - 50) : 0; Serial.printf("COOLDOWN_MS=%lu\n", (unsigned long)COOLDOWN_MS); break;

    case 'I': MIN_HITS_REQUIRED++; Serial.printf("MIN_HITS_REQUIRED=%u\n", (unsigned)MIN_HITS_REQUIRED); break;
    case 'i': MIN_HITS_REQUIRED = (MIN_HITS_REQUIRED > 1) ? (uint8_t)(MIN_HITS_REQUIRED - 1) : 1; Serial.printf("MIN_HITS_REQUIRED=%u\n", (unsigned)MIN_HITS_REQUIRED); break;

    default:
      return;
  }

  clamp_all_();
}

// =====================================================
// Setup / Loop
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("CC1101 OOK export-sniffer + GUIDED LEARN (key-driven, no Enter)");
  Serial.println("Only for equipment you own / have permission to test.");
  Serial.printf("FREQ: %.2f MHz (OOK)\n", RF_FREQ_MHZ);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);

  int16_t state = radio.begin(RF_FREQ_MHZ, BITRATE_KBPS, FREQDEV_KHZ, RX_BW_KHZ, 10, 16);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.begin() failed, code: %d\n", state);
    while (true) delay(1000);
  }

  radio.setOOK(true);
  radio.setPromiscuousMode(true);

  state = radio.receiveDirectAsync();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("receiveDirectAsync() failed, code: %d\n", state);
    while (true) delay(1000);
  }

  pinMode(PIN_GDO0, INPUT);

  g_edgeCount = 0;
  g_lastEdgeUs = micros();
  g_captureEnabled = false;

  attachInterrupt(digitalPinToInterrupt(PIN_GDO0), onGdo0Change, CHANGE);

  clamp_all_();
  print_help();
  Serial.printf("Press %s now...\n", LABELS[learnIndex]);
}

void loop() {
  // --- key handling (direct, no Enter) ---
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') continue; // ignore line endings entirely
    handle_key(c);
  }

  static bool inSession = false;
  static uint16_t lastCount = 0;
  static uint32_t lastTick = 0;
  static uint32_t cooldownUntil = 0;
  static float peakRssi = -120.0f;
  static uint32_t lastEdgeLocalUs = 0;
  static uint32_t dbgLastMs = 0;

  const uint32_t nowMs = millis();
  if (nowMs - lastTick < 50) return;
  lastTick = nowMs;

  float rssi = radio.getRSSI();
  if (rssi > peakRssi) peakRssi = rssi;

  // noise gate
  if (inSession) g_captureEnabled = true;
  else g_captureEnabled = (rssi >= IDLE_GATE_MIN_RSSI_DBM);

  // atomic read of edge status
  uint16_t ccount;
  uint32_t lastEdgeUs;
  noInterrupts();
  ccount = g_edgeCount;
  lastEdgeUs = g_lastEdgeUs;
  interrupts();

  const uint16_t dEdges = (ccount >= lastCount) ? (ccount - lastCount) : 0;
  lastCount = ccount;

  if (DBG_HEARTBEAT && (nowMs - dbgLastMs >= DBG_EVERY_MS)) {
    Serial.printf("DBG: RSSI=%.1f peak=%.1f inSession=%d capEn=%d edges=%u dEdges/50ms=%u cooldown=%d learnStep=%u(%s) minHits=%u\n",
                  rssi, peakRssi, inSession ? 1 : 0, g_captureEnabled ? 1 : 0,
                  (unsigned)ccount, (unsigned)dEdges,
                  (nowMs < cooldownUntil) ? 1 : 0,
                  (unsigned)learnIndex, (learnIndex < 8) ? LABELS[learnIndex] : "DONE",
                  (unsigned)MIN_HITS_REQUIRED);
    dbgLastMs = nowMs;
  }

  if (nowMs < cooldownUntil) return;

  // -----------------------
  // Start session
  // -----------------------
  if (!inSession) {
    const bool startByRssi  = (rssi >= RSSI_START_DBM && dEdges > 10);
    const bool startByEdges = (dEdges >= EDGE_BURST_START && rssi >= RSSI_START_DBM);

    if (startByRssi || startByEdges) {
      inSession = true;
      g_captureEnabled = true;
      peakRssi = rssi;
      lastEdgeLocalUs = lastEdgeUs;

      // reset edge buffer so the session contains only this press
      noInterrupts();
      g_edgeCount = 0;
      g_lastEdgeUs = micros();
      interrupts();
      lastCount = 0;

      Serial.printf("SESSION START: rssi=%.1f dBm dEdges=%u (by:%s)\n",
                    rssi, (unsigned)dEdges, startByRssi ? "RSSI" : "EDGES");
    }
    return;
  }

  // -----------------------
  // Session running -> end it
  // -----------------------
  lastEdgeLocalUs = lastEdgeUs;
  const bool timedOut = ((uint32_t)(micros() - lastEdgeLocalUs) > END_GAP_US);

  bool bufferFull = false;
  noInterrupts();
  bufferFull = (g_edgeCount >= (MAX_EDGES - 1));
  interrupts();

  if (!timedOut && !bufferFull) return;

  // copy buffer + reset
  noInterrupts();
  g_copyCount = g_edgeCount;
  if (g_copyCount > MAX_EDGES) g_copyCount = MAX_EDGES;
  for (uint16_t i = 0; i < g_copyCount; i++) g_copy[i] = g_edges[i];

  g_edgeCount = 0;
  g_lastEdgeUs = micros();
  interrupts();
  lastCount = 0;

  inSession = false;
  cooldownUntil = millis() + COOLDOWN_MS;
  g_captureEnabled = false;

  BestFrame bf = find_best_frame();
  if (bf.hash == 0) {
    Serial.printf("SESSION END: no valid frames (edges=%u)%s\n",
                  (unsigned)g_copyCount, bufferFull ? " [FORCED]" : "");
    peakRssi = -120.0f;
    return;
  }

  Serial.printf("SESSION END: edges=%u peakRSSI=%.1f BEST=0x%08lX hits=%u unit~%u len=%u%s\n",
                (unsigned)g_copyCount, peakRssi,
                (unsigned long)bf.hash,
                (unsigned)bf.hits,
                (unsigned)bf.unit,
                (unsigned)bf.len,
                bufferFull ? " [FORCED]" : "");

  // -----------------------
  // GUIDED LEARN
  // -----------------------
  if (learnIndex < 8) {
    if (bf.hits < MIN_HITS_REQUIRED) {
      Serial.printf("Learn: too few hits (%u/%u) for %s (press again)\n",
                    (unsigned)bf.hits, (unsigned)MIN_HITS_REQUIRED, LABELS[learnIndex]);
      peakRssi = -120.0f;
      return;
    }

    if (learn_accept(bf.hash)) {
      learnedCmd[learnIndex] = bf.hash;
      learnedOk[learnIndex] = true;

      Serial.printf("LEARNED %-9s = 0x%08lX\n", LABELS[learnIndex], (unsigned long)bf.hash);

      for (uint8_t i = 0; i < 5; i++) recentCmd[i] = 0;
      recentPos = 0;

      learnIndex++;
      if (learnIndex < 8) {
        Serial.println();
        Serial.printf("Press %s now...\n", LABELS[learnIndex]);
      } else {
        print_learned_table();
        Serial.println("ALL 8 LEARNED. You can now export by pressing buttons in any order.");
        Serial.println("Press 'x' for status, 'a' for arrays, 'X' for patterns.h.");
      }
    } else {
      Serial.printf("Learn: needs 3 accepted sessions for %s (press the same button more times)\n",
                    LABELS[learnIndex]);
    }

    peakRssi = -120.0f;
    return;
  }

  // -----------------------
  // EXPORT
  // -----------------------
  const int idx = find_cmd_index(bf.hash);
  if (idx < 0) {
    Serial.printf("NO MATCH: SIG=0x%08lX (not in learned table)\n", (unsigned long)bf.hash);
    peakRssi = -120.0f;
    return;
  }

  Serial.printf("MATCH: %s (hash 0x%08lX)\n", LABELS[idx], (unsigned long)bf.hash);

  if (bf.hits < MIN_HITS_REQUIRED) {
    Serial.printf("EXPORT: ignoring %s due to too few hits (%u/%u)\n",
                  LABELS[idx], (unsigned)bf.hits, (unsigned)MIN_HITS_REQUIRED);
    peakRssi = -120.0f;
    return;
  }

  if (should_replace_capture((uint8_t)idx, bf, peakRssi)) {
    store_capture((uint8_t)idx, bf, peakRssi);
    Serial.printf("CAPTURED %s: len=%u (best frame)\n", LABELS[idx], (unsigned)bf.len);
    dump_one_capture((uint8_t)idx);

    bool all = true;
    for (uint8_t i2 = 0; i2 < 8; i2++) if (!capOk[i2]) all = false;
    if (all) {
      Serial.println();
      Serial.println("ALL 8 COMMANDS CAPTURED.");
      print_export_status();
      Serial.println("Press 'X' for the complete patterns.h block.");
    }
  }

  peakRssi = -120.0f;
}
