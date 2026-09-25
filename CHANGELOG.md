The format is based on "Keep a Changelog", and this project follows semantic versioning where practical.

## [0.5.0] 2026-09-26
### Added
- **ESP8266 support for the main TX + MQTT firmware** (new `[env:nodemcu]` in `platformio.ini`). The sniffer tool remains ESP32-only.
- `src/radio/tx_power.h`: single shared TX-power allow-list used by `radio_tx.cpp`, `fan_state.cpp` and `mqtt.cpp` (previously duplicated identically in all three).
- ESP8266 `FanStateStore` backend using `EEPROM` (magic + version + CRC-8 validated), since `Preferences`/NVS is ESP32-only.
- README: ESP8266 wiring table, `-e`/multi-env build instructions, and a "Known limitations" section.

### Fixed
- `HaMqtt::mqttEnsure_()` had no reconnect throttle at all, unlike the WiFi reconnect path in the same file — a broker actively refusing connections could be retried on every single `loop()` iteration. Now throttled to 1 attempt / 5s.
- README build commands referenced a `tools/sniffer/` folder that doesn't exist in this repo (the actual folder is `tools/sniffer-433/`); all references corrected.
- `sendPatternPGM()` now calls `yield()` once per repeat, in the idle inter-frame gap only (never mid-waveform), so a high repeat-count + long-gap TX doesn't starve the WiFi stack / watchdog — most relevant on ESP8266's single core.

## [0.4.0] 2026-01-05 (public release)
### Added
- CC1101 OOK **direct sniffer** with session-based edge capture (GDO0 interrupt) and automatic frame selection.
- **Guided Learn** workflow (OFF → SPEED1..6 → ROTATION) with stability rule (same hash in ≥3 of last 5 accepted sessions).
- **Export capture** workflow that stores best P_* pattern per command (prefers higher hits, then stronger peak RSSI).
- One-key Serial UI (no Enter) for help/status/reset/export and runtime tuning of thresholds.
- `patterns.h` **auto-export** generator (P_* arrays + enums + lookup tables).
- `radio/RadioTx` module for **direct OOK transmit** using captured PROGMEM patterns.
- Runtime TX tuning: output power, repeat count, inter-repeat gap, and optional invert.
- Repository docs: `README.md` (setup/workflow) and `CHANGELOG.md` (release history).
- `tools/` included in repo (project tooling and utilities).
- `.gitattributes` to enforce LF line endings for common source/docs files.
- `credentials.example.h` template for local Wi-Fi/MQTT configuration.
- Removed unused `lib/` placeholder directory (PlatformIO default).
- Added repository images under `images/` (CC1101, ESP32, fan, remote) for documentation.

### Changed
- Project structure refined into clearer modules (sniffer vs TX vs MQTT/app logic).

## [0.3.0] - 2025-12-28
### Added
- Initial CC1101 OOK project baseline.
- Initial build/run setup (PlatformIO-based workflow).
- Initial MQTT + device scaffolding.

### Notes
- License: GPL-3.0-or-later (see LICENSE).