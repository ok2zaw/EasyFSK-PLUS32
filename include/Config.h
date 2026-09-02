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
} // namespace CfgLimits

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
