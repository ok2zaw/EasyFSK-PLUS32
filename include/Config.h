#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Persisted configuration: LittleFS + JSON replacement for the AVR
// original's raw EEPROM layout (EE_* addresses / eeLoad() in
// TinyFSK_ZAW_01.cpp). Field names, ranges and defaults below are taken
// directly from that source, not invented -- see the design doc
// (claude/esp32-port-design-decisions.md) "Web UI: routes and JSON shapes"
// section for the full rationale.
// ---------------------------------------------------------------------------

namespace CfgLimits {
constexpr uint8_t CALLSIGN_MAX_LEN = 6;   // AVR CALLSIGN_MAX_LEN
constexpr uint16_t TIMING_MIN_MS = 0;     // AVR TIMING_MIN_MS
constexpr uint16_t TIMING_MAX_MS = 9999;  // AVR TIMING_MAX_MS
constexpr uint8_t HOSTNAME_MAX_LEN = 31;
constexpr uint8_t IP_STR_MAX_LEN = 15; // "255.255.255.255"
constexpr uint8_t CW_SPEED_MIN_WPM = 5;  // matches Winkey's own speed range
constexpr uint8_t CW_SPEED_MAX_WPM = 99;
constexpr uint8_t TCI_HOST_MAX_LEN = 15;
constexpr uint16_t TCI_PORT_MIN = 1;
constexpr uint16_t TCI_PORT_MAX = 65535;
constexpr uint16_t TCI_TONE_MIN_HZ = 300;
constexpr uint16_t TCI_TONE_MAX_HZ = 3000;
constexpr uint16_t TCI_SAMPLES_MIN = 100;
constexpr uint16_t TCI_SAMPLES_MAX = 2048;
constexpr uint16_t TCI_BUFFER_TIMEOUT_MIN_MS = 50;
constexpr uint16_t TCI_BUFFER_TIMEOUT_MAX_MS = 500;
} // namespace CfgLimits

// UART2's mode -- see the design doc's "UART1/UART2 split" (2026-09-27).
// UART1 (the original UART0/J1 link) is unconditionally FSK/RTTY control
// and has no mode field at all; this only ever affects UART2.
enum class Uart2Mode : uint8_t { Fsk2, Cw };

// Selects which receive monitor/demodulator and future TUNE-LED tuning
// display are active. This is independent of the UART2 transmit/input mode.
enum class RxMonitorMode : uint8_t { Rtty, Cw };

struct NetworkConfig {
  char hostname[CfgLimits::HOSTNAME_MAX_LEN + 1] = "easyfsk";
  bool dhcp = true;
  // Only meaningful when dhcp == false; still stored (but ignored) when
  // dhcp == true, per the design doc's decision to keep both DHCP and
  // static-IP fields present in the JSON at all times.
  char staticIp[CfgLimits::IP_STR_MAX_LEN + 1] = "192.168.1.50";
  char gateway[CfgLimits::IP_STR_MAX_LEN + 1] = "192.168.1.1";
  char subnet[CfgLimits::IP_STR_MAX_LEN + 1] = "255.255.255.0";
  char dns[CfgLimits::IP_STR_MAX_LEN + 1] = "192.168.1.1";
};

struct TciConfig {
  bool enabled = false;
  char serverIp[CfgLimits::TCI_HOST_MAX_LEN + 1] = "192.168.1.100";
  uint16_t port = 50001;
  uint8_t receiver = 0;
  uint16_t markHz = 2125;
  uint16_t spaceHz = 2295;
  uint32_t sampleRate = 48000;
  uint8_t sampleFormat = 0; // TCI INT16
  uint8_t channels = 1;
  uint16_t samples = 480;
  uint16_t bufferTimeoutMs = 50;
};

struct Config {
  // --- Fields with a direct AVR EEPROM equivalent ---
  char callsign[CfgLimits::CALLSIGN_MAX_LEN + 1] = ""; // display/reporting only, not used by the TX engine
  float baudRate = 45.45f;  // one of 45.45 / 50.0 / 75.0 -- AVR default (blank EEPROM) is 45.45
  bool markHigh = true;     // FSK polarity; AVR default (blank EEPROM) is markHigh
  uint16_t pttLeadMs = 150; // AVR compiled-in default
  uint16_t pttTailMs = 25;  // AVR compiled-in default
  uint16_t paLeadMs = 80;   // AVR compiled-in default
  uint16_t paTailMs = 80;   // AVR compiled-in default
  bool liveLcdText = true;  // AVR default (blank EEPROM) is enabled

  // --- New for the ESP32 port ---
  NetworkConfig network;
  TciConfig tci;

  // --- New for CW keying / Winkey emulation (added 2026-09-27) ---
  // Settable from the Configuration page (a toggle) and from the encoder
  // pushbutton's long-press gesture -- both just go through
  // ConfigStore::applyAndSave() like any other field, which is what keeps
  // this from changing mid-transmission (see the design doc's "no free
  // GPIO"/pin-swap and CW/Winkey sections).
  Uart2Mode uart2Mode = Uart2Mode::Fsk2;
  RxMonitorMode rxMonitorMode = RxMonitorMode::Rtty;
  // Power-on/default CW speed: loaded into the CW engine at boot and
  // whenever this stored value itself changes. The Winkey host's speed
  // command (0x02) changes only the RUNNING speed and is never written back
  // here -- that would mean a flash write per logger speed change. See the
  // design doc's CW/Winkey "not yet decided" list (2026-09-28 entry).
  uint8_t cwSpeedWpm = 20; // Winkey's own typical factory-default speed
};

// Loads /config.json from LittleFS into `out`. A missing/corrupt/blank
// file, or any individual field that is missing or out of range, falls
// back to the compiled-in default *for that field only* -- same
// defensive-fallback discipline as the AVR eeLoad(), just JSON-shaped
// instead of raw EEPROM bytes. Always succeeds (returns false only to
// indicate "no file existed / this is a first boot", which is not an
// error -- `out` is still fully populated with defaults either way).
bool configLoad(Config &out);

// Validates `in` field-by-field against the same ranges the AVR firmware
// already enforces (see CfgLimits above). Fields absent from `in` are
// left at whatever is currently in `out` (partial input is fine -- but
// per the design doc, the Configuration page always submits the whole
// form, so in practice `in` is expected to be complete).
//
// On success, returns true and `out` holds the sanitized merged config
// (ready to configSave()). On failure, returns false; `errors` holds one
// human-readable message per invalid field (JSON field name -> message)
// and `out` is left unchanged from whatever the caller passed in.
bool configValidate(JsonVariantConst in, Config &out, JsonObject errors);

// Persists `cfg` to /config.json on LittleFS. Caller is responsible for
// the "not during active TX" gating rule from the design doc -- this
// function has no idea what `ptt` is.
bool configSave(const Config &cfg);

// Serializes `cfg` into `out` (used for GET /api/config, GET
// /api/config/backup, and the POST /api/config success response).
void configToJson(const Config &cfg, JsonObject out);
