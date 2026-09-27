#include "SerialControl.h"
#include "TxManager.h"
#include "ConfigStore.h"
#include "Config.h"
#include <Arduino.h>
#include <ArduinoJson.h>

namespace SerialControl {

namespace {

constexpr char TX_ON = '[';
constexpr char TX_END = ']';
constexpr char TX_ABORT = '\\';
constexpr char COMMAND_ESCAPE = '~';

enum class Mode { Normal, Config, NumericEntry, CallsignEntry };
Mode s_mode = Mode::Normal;

char s_numericCommand = 0;
long s_numericValue = 0;
bool s_numericHasDigits = false;

char s_callsignBuf[CfgLimits::CALLSIGN_MAX_LEN + 1];
uint8_t s_callsignLen = 0;
bool s_callsignHasChars = false;

void printConfig() {
  Config cfg = ConfigStore::get();
  Serial.print(F("\n--- EasyFSK-PLUS32 configuration ---\n"));
  Serial.print(F("  Baud rate:     "));
  Serial.println(cfg.baudRate);
  Serial.print(F("  Polarity:      "));
  Serial.println(cfg.markHigh ? "mark = HIGH" : "mark = LOW");
  Serial.print(F("  PTT lead:      "));
  Serial.print(cfg.pttLeadMs);
  Serial.println(F("ms"));
  Serial.print(F("  PTT tail:      "));
  Serial.print(cfg.pttTailMs);
  Serial.println(F("ms"));
  Serial.print(F("  PA lead:       "));
  Serial.print(cfg.paLeadMs);
  Serial.println(F("ms"));
  Serial.print(F("  PA tail:       "));
  Serial.print(cfg.paTailMs);
  Serial.println(F("ms"));
  Serial.print(F("  Callsign:      "));
  Serial.println(cfg.callsign[0] != '\0' ? cfg.callsign : "(not set)");
  Serial.print(F("  Live LCD text: "));
  Serial.println(cfg.liveLcdText ? "on" : "off");
}

// Applies one field, mirroring the AVR original's single-key config
// commands ('0'/'1' polarity, '4'/'5'/'7' baud, 'D'/'d' live-LCD toggle).
template <typename T>
void applySingleField(const char *key, const T &value) {
  JsonDocument doc;
  doc[key] = value;
  JsonDocument errDoc;
  JsonObject errs = errDoc.to<JsonObject>();
  if (!ConfigStore::applyAndSave(doc.as<JsonVariantConst>(), errs)) {
    Serial.print(F("\nNot applied: "));
    for (JsonPair p : errs) {
      Serial.println(p.value().as<const char *>());
    }
  }
}

void exitConfigMode() {
  s_mode = Mode::Normal;
  printConfig();
}

void handleConfigCommand(uint8_t b) {
  switch (b) {
    case '0': applySingleField("polarity", "markHigh"); exitConfigMode(); break;
    case '1': applySingleField("polarity", "markLow"); exitConfigMode(); break;
    case '4': applySingleField("baudRate", 45.45f); exitConfigMode(); break;
    case '5': applySingleField("baudRate", 50.0f); exitConfigMode(); break;
    case '7': applySingleField("baudRate", 75.0f); exitConfigMode(); break;
    case '?': exitConfigMode(); break; // just dump current config, no change
    case 'D': applySingleField("liveLcdText", true); exitConfigMode(); break;
    case 'd': applySingleField("liveLcdText", false); exitConfigMode(); break;

    case 'L': case 'T': case 'l': case 't':
      s_numericCommand = static_cast<char>(b);
      s_numericValue = 0;
      s_numericHasDigits = false;
      s_mode = Mode::NumericEntry;
      Serial.print(F("\nEnter new value in ms (0-9999) and press Enter,\n"
                      "or press Enter alone to view the current value: "));
      break;

    case 'C':
      s_callsignLen = 0;
      s_callsignHasChars = false;
      s_mode = Mode::CallsignEntry;
      Serial.print(F("\nEnter new callsign (up to 6 chars) and press Enter,\n"
                      "or press Enter alone to view the current value: "));
      break;

    default:
      Serial.print(F("\nNot a recognized command. Exiting configuration mode.\n"));
      exitConfigMode();
      break;
  }
}

void handleNumericEntry(uint8_t b) {
  if (b >= '0' && b <= '9') {
    if (s_numericValue <= 999) { // cap accumulation at 4 digits (0-9999)
      s_numericValue = s_numericValue * 10 + (b - '0');
      s_numericHasDigits = true;
      Serial.write(b);
    }
  } else if (b == '\r' || b == '\n') {
    if (s_numericHasDigits) {
      const char *key = nullptr;
      switch (s_numericCommand) {
        case 'L': key = "pttLeadMs"; break;
        case 'T': key = "pttTailMs"; break;
        case 'l': key = "paLeadMs"; break;
        case 't': key = "paTailMs"; break;
      }
      if (key) applySingleField(key, static_cast<int>(s_numericValue));
    }
    exitConfigMode();
  } else if (b == COMMAND_ESCAPE) {
    Serial.print(F("\nCancelled.\n"));
    s_mode = Mode::Normal;
  }
  // any other byte is ignored while entering a numeric value
}

void handleCallsignEntry(uint8_t b) {
  if (b == '\r' || b == '\n') {
    if (s_callsignHasChars) {
      s_callsignBuf[s_callsignLen] = '\0';
      JsonDocument doc;
      doc["callsign"] = s_callsignBuf;
      JsonDocument errDoc;
      JsonObject errs = errDoc.to<JsonObject>();
      if (!ConfigStore::applyAndSave(doc.as<JsonVariantConst>(), errs)) {
        Serial.print(F("\nNot applied: "));
        for (JsonPair p : errs) Serial.println(p.value().as<const char *>());
      }
    }
    exitConfigMode();
  } else if (b == COMMAND_ESCAPE) {
    Serial.print(F("\nCancelled.\n"));
    s_mode = Mode::Normal;
  } else if (b >= 0x20 && b <= 0x7E && s_callsignLen < CfgLimits::CALLSIGN_MAX_LEN) {
    s_callsignBuf[s_callsignLen++] = static_cast<char>(b);
    s_callsignHasChars = true;
    Serial.write(b);
  }
}

} // namespace

void begin() { s_mode = Mode::Normal; }

void poll() {
  if (Serial.available() <= 0) return;
  uint8_t b = Serial.read();

  switch (s_mode) {
    case Mode::NumericEntry:
      handleNumericEntry(b);
      return;
    case Mode::CallsignEntry:
      handleCallsignEntry(b);
      return;
    case Mode::Config:
      handleConfigCommand(b);
      return;
    case Mode::Normal:
      break;
  }

  if (b == TX_ABORT) {
    TxManager::enqueueAbort();
  } else if (b == TX_ON) {
    TxManager::enqueueKeyUp(TxManager::Source::SerialLink);
  } else if (b == TX_END) {
    TxManager::enqueueBufferedEnd();
  } else if (b == COMMAND_ESCAPE) {
    s_mode = Mode::Config;
    Serial.print(F("\n--- Config menu: 0/1 polarity, 4/5/7 baud, L/T/l/t PTT/PA "
                    "lead/tail, C callsign, D/d live LCD text, ? show config ---\n"));
  } else {
    TxManager::enqueueByte(b, TxManager::Source::SerialLink);
  }
}

} // namespace SerialControl
