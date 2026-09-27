#include "Config.h"
#include <LittleFS.h>
#include <cstdio>

static const char *CONFIG_PATH = "/config.json";
static const char *CONFIG_TEMP_PATH = "/config.tmp";

// --- small validation helpers -----------------------------------------------

static bool isValidCallsign(const char *s, size_t len) {
  if (len > CfgLimits::CALLSIGN_MAX_LEN) return false;
  for (size_t i = 0; i < len; i++) {
    if (s[i] < 0x20 || s[i] > 0x7E) return false; // printable ASCII, matches AVR eeLoad()
  }
  return true;
}

static bool isValidBaudRate(float b, float &sanitized) {
  // Matches the AVR's three selectable rates exactly (COMMAND_45BAUD/
  // 50BAUD/75BAUD). Compare with a small epsilon since 45.45 isn't exact
  // in binary floating point.
  const float candidates[] = {45.45f, 50.0f, 75.0f};
  for (float c : candidates) {
    if (fabsf(b - c) < 0.01f) {
      sanitized = c;
      return true;
    }
  }
  return false;
}

static bool isValidTimingMs(long v) {
  return v >= CfgLimits::TIMING_MIN_MS && v <= CfgLimits::TIMING_MAX_MS;
}

// Minimal dotted-quad IPv4 syntax check -- good enough to catch typos
// before they get persisted; ETH.config() will be the final authority at
// apply time.
static bool isValidIPv4(const char *s) {
  if (s == nullptr || s[0] == '\0') return false;
  int octets = 0, val = -1, digits = 0;
  for (const char *p = s;; p++) {
    if (*p >= '0' && *p <= '9') {
      val = (val < 0 ? 0 : val) * 10 + (*p - '0');
      digits++;
      if (val > 255 || digits > 3) return false;
    } else if (*p == '.' || *p == '\0') {
      if (digits == 0) return false;
      octets++;
      val = -1;
      digits = 0;
      if (*p == '\0') break;
      if (octets >= 4) return false;
    } else {
      return false;
    }
  }
  return octets == 4;
}

static bool isValidCwSpeedWpm(long v) {
  return v >= CfgLimits::CW_SPEED_MIN_WPM && v <= CfgLimits::CW_SPEED_MAX_WPM;
}

static bool isValidHostname(const char *s, size_t len) {
  if (len == 0 || len > CfgLimits::HOSTNAME_MAX_LEN) return false;
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') ||
        (s[0] >= '0' && s[0] <= '9'))) {
    return false;
  }
  char last = s[len - 1];
  if (!((last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z') ||
        (last >= '0' && last <= '9'))) {
    return false;
  }
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-';
    if (!ok) return false;
  }
  return true;
}

// --- public API --------------------------------------------------------------

void configToJson(const Config &cfg, JsonObject out) {
  out["callsign"] = cfg.callsign;
  out["baudRate"] = cfg.baudRate;
  out["polarity"] = cfg.markHigh ? "markHigh" : "markLow";
  out["pttLeadMs"] = cfg.pttLeadMs;
  out["pttTailMs"] = cfg.pttTailMs;
  out["paLeadMs"] = cfg.paLeadMs;
  out["paTailMs"] = cfg.paTailMs;
  out["liveLcdText"] = cfg.liveLcdText;
  out["uart2Mode"] = (cfg.uart2Mode == Uart2Mode::Cw) ? "cw" : "fsk2";
  out["cwSpeedWpm"] = cfg.cwSpeedWpm;

  JsonObject net = out["network"].to<JsonObject>();
  net["hostname"] = cfg.network.hostname;
  net["dhcp"] = cfg.network.dhcp;
  net["staticIp"] = cfg.network.staticIp;
  net["gateway"] = cfg.network.gateway;
  net["subnet"] = cfg.network.subnet;
  net["dns"] = cfg.network.dns;
}

static bool configMerge(JsonVariantConst in, Config &out, JsonObject errors,
                        bool commitValidFieldsOnError) {
  Config result = out; // start from current values; only overwrite fields present in `in`
  bool ok = true;

  JsonVariantConst callsign = in["callsign"];
  if (!callsign.isNull()) {
    if (callsign.is<const char *>()) {
      const char *cs = callsign.as<const char *>();
      size_t len = strlen(cs);
      if (isValidCallsign(cs, len)) {
        strncpy(result.callsign, cs, sizeof(result.callsign) - 1);
        result.callsign[sizeof(result.callsign) - 1] = '\0';
      } else {
        errors["callsign"] = "must be at most 6 printable ASCII characters";
        ok = false;
      }
    } else {
      errors["callsign"] = "must be a string";
      ok = false;
    }
  }

  JsonVariantConst baudRate = in["baudRate"];
  if (!baudRate.isNull()) {
    float sanitized;
    if (!baudRate.is<float>()) {
      errors["baudRate"] = "must be numeric";
      ok = false;
    } else if (isValidBaudRate(baudRate.as<float>(), sanitized)) {
      result.baudRate = sanitized;
    } else {
      errors["baudRate"] = "must be one of 45.45, 50, 75";
      ok = false;
    }
  }

  JsonVariantConst polarity = in["polarity"];
  if (!polarity.isNull()) {
    if (polarity.is<const char *>()) {
      const char *pol = polarity.as<const char *>();
      if (strcmp(pol, "markHigh") == 0) {
        result.markHigh = true;
      } else if (strcmp(pol, "markLow") == 0) {
        result.markHigh = false;
      } else {
        errors["polarity"] = "must be \"markHigh\" or \"markLow\"";
        ok = false;
      }
    } else {
      errors["polarity"] = "must be a string";
      ok = false;
    }
  }

  struct TimingField {
    const char *jsonKey;
    uint16_t Config::*member;
  };
  const TimingField timingFields[] = {
      {"pttLeadMs", &Config::pttLeadMs},
      {"pttTailMs", &Config::pttTailMs},
      {"paLeadMs", &Config::paLeadMs},
      {"paTailMs", &Config::paTailMs},
  };
  for (const auto &f : timingFields) {
    if (!in[f.jsonKey].isNull()) {
      JsonVariantConst timing = in[f.jsonKey];
      if (!timing.is<long>()) {
        errors[f.jsonKey] = "must be an integer from 0-9999";
        ok = false;
      } else {
        long v = timing.as<long>();
        if (isValidTimingMs(v)) {
          result.*(f.member) = static_cast<uint16_t>(v);
        } else {
          errors[f.jsonKey] = "must be 0-9999";
          ok = false;
        }
      }
    }
  }

  JsonVariantConst liveLcdText = in["liveLcdText"];
  if (!liveLcdText.isNull()) {
    if (liveLcdText.is<bool>()) {
      result.liveLcdText = liveLcdText.as<bool>();
    } else {
      errors["liveLcdText"] = "must be a boolean";
      ok = false;
    }
  }

  JsonVariantConst uart2Mode = in["uart2Mode"];
  if (!uart2Mode.isNull()) {
    if (uart2Mode.is<const char *>()) {
      const char *m = uart2Mode.as<const char *>();
      if (strcmp(m, "fsk2") == 0) {
        result.uart2Mode = Uart2Mode::Fsk2;
      } else if (strcmp(m, "cw") == 0) {
        result.uart2Mode = Uart2Mode::Cw;
      } else {
        errors["uart2Mode"] = "must be \"fsk2\" or \"cw\"";
        ok = false;
      }
    } else {
      errors["uart2Mode"] = "must be a string";
      ok = false;
    }
  }

  JsonVariantConst cwSpeedWpm = in["cwSpeedWpm"];
  if (!cwSpeedWpm.isNull()) {
    if (!cwSpeedWpm.is<long>()) {
      errors["cwSpeedWpm"] = "must be an integer from 5-99";
      ok = false;
    } else {
      long v = cwSpeedWpm.as<long>();
      if (isValidCwSpeedWpm(v)) {
        result.cwSpeedWpm = static_cast<uint8_t>(v);
      } else {
        errors["cwSpeedWpm"] = "must be 5-99";
        ok = false;
      }
    }
  }

  JsonVariantConst net = in["network"];
  if (!net.isNull()) {
    if (!net.is<JsonObjectConst>()) {
      errors["network"] = "must be an object";
      ok = false;
    } else {
      JsonVariantConst hostname = net["hostname"];
      if (!hostname.isNull()) {
        if (hostname.is<const char *>()) {
          const char *h = hostname.as<const char *>();
          size_t len = strlen(h);
          if (isValidHostname(h, len)) {
            strncpy(result.network.hostname, h, sizeof(result.network.hostname) - 1);
            result.network.hostname[sizeof(result.network.hostname) - 1] = '\0';
          } else {
            errors["network.hostname"] = "1-31 chars, alphanumeric ends, letters/digits/hyphen only";
            ok = false;
          }
        } else {
          errors["network.hostname"] = "must be a string";
          ok = false;
        }
      }
      JsonVariantConst dhcp = net["dhcp"];
      if (!dhcp.isNull()) {
        if (dhcp.is<bool>()) {
          result.network.dhcp = dhcp.as<bool>();
        } else {
          errors["network.dhcp"] = "must be a boolean";
          ok = false;
        }
      }
      // Static-IP fields are only *required* to be valid when dhcp is false;
      // when dhcp is true they're still validated if present (so garbage
      // never gets persisted), just not required.
      const struct {
        const char *jsonKey;
        char *field;
        size_t fieldSize;
      } ipFields[] = {
          {"staticIp", result.network.staticIp, sizeof(result.network.staticIp)},
          {"gateway", result.network.gateway, sizeof(result.network.gateway)},
          {"subnet", result.network.subnet, sizeof(result.network.subnet)},
          {"dns", result.network.dns, sizeof(result.network.dns)},
      };
      for (const auto &f : ipFields) {
        JsonVariantConst address = net[f.jsonKey];
        if (address.isNull()) continue;
        if (!address.is<const char *>()) {
          char key[32];
          snprintf(key, sizeof(key), "network.%s", f.jsonKey);
          errors[key] = "must be a string containing a valid IPv4 address";
          ok = false;
          continue;
        }
        const char *v = address.as<const char *>();
        if (isValidIPv4(v)) {
          strncpy(f.field, v, f.fieldSize - 1);
          f.field[f.fieldSize - 1] = '\0';
        } else {
          char key[32];
          snprintf(key, sizeof(key), "network.%s", f.jsonKey);
          errors[key] = "must be a valid IPv4 address";
          ok = false;
        }
      }
    }
  }

  if (ok || commitValidFieldsOnError) {
    out = result;
  }
  return ok;
}

bool configValidate(JsonVariantConst in, Config &out, JsonObject errors) {
  // Interactive/API updates are transactional: one invalid field rejects
  // the whole candidate and leaves the active configuration untouched.
  return configMerge(in, out, errors, false);
}

bool configLoad(Config &out) {
  out = Config(); // start from compiled-in defaults for every field

  if (!LittleFS.exists(CONFIG_PATH)) {
    return false; // first boot -- not an error, defaults already in `out`
  }

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) {
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    // Corrupt file -- same "fall back to defaults" behavior as the AVR's
    // blank/garbage-EEPROM handling.
    return false;
  }

  // Reuse configValidate() so load-time and save-time enforce identical
  // rules. Anything invalid/missing in the file simply keeps the default
  // already sitting in `out` for that one field -- errors are discarded
  // here (there's no user to show them to at boot time).
  JsonDocument errDoc;
  JsonObject errs = errDoc.to<JsonObject>();
  // A persisted file is recovered field by field. Valid values survive even
  // if a different field is corrupt; only invalid fields retain the defaults
  // assigned at the start of configLoad().
  configMerge(doc.as<JsonVariantConst>(), out, errs, true);
  return true;
}

bool configSave(const Config &cfg) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  configToJson(cfg, root);

  // Write a complete temporary file first, then atomically replace the live
  // path. Arduino-ESP32's FS::rename() maps to POSIX rename(), so the old
  // config survives write/close/rename failures instead of being truncated.
  File f = LittleFS.open(CONFIG_TEMP_PATH, "w");
  if (!f) {
    return false;
  }
  size_t expected = measureJson(doc);
  size_t written = serializeJson(doc, f);
  f.close();
  if (written != expected) {
    LittleFS.remove(CONFIG_TEMP_PATH);
    return false;
  }
  if (!LittleFS.rename(CONFIG_TEMP_PATH, CONFIG_PATH)) {
    LittleFS.remove(CONFIG_TEMP_PATH);
    return false;
  }
  return true;
}
