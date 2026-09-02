#include <Arduino.h>

// --- Ethernet PHY configuration -- MUST come before <ETH.h> is first
// included in this translation unit (see the note in Pins.h). Values match
// Impero32's onboard LAN8720A wiring from the design doc. ---
#define ETH_PHY_TYPE ETH_PHY_LAN8720
#define ETH_PHY_ADDR 1
#define ETH_PHY_MDC 23
#define ETH_PHY_MDIO 18
#define ETH_PHY_POWER -1 // handled manually via ETH_OSC_EN_PIN below, not by ETH.begin() itself
#define ETH_CLK_MODE ETH_CLOCK_GPIO0_IN

#include <ETH.h>
#include <LittleFS.h>

#include "Pins.h"
#include "Version.h"
#include "Config.h"
#include "ConfigStore.h"
#include "FskTimer.h"
#include "TxManager.h"
#include "SerialControl.h"
#include "StatusDisplay.h"
#include "WebInterface.h"

namespace {

void onEthEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START: {
      Config cfg = ConfigStore::get();
      ETH.setHostname(cfg.network.hostname);
      break;
    }
    case ARDUINO_EVENT_ETH_CONNECTED:
    case ARDUINO_EVENT_ETH_GOT_IP:
    case ARDUINO_EVENT_ETH_DISCONNECTED:
    case ARDUINO_EVENT_ETH_STOP:
    default:
      break; // link-state details are read on demand via ETH.linkUp() (see WebInterface)
  }
}

} // namespace

void setup() {
  Serial.begin(9600); // matches the AVR original's serialSpeed, 8-N-1

  if (!LittleFS.begin(true)) { // true = format on first boot / mount failure
    Serial.println(F("LittleFS mount failed"));
  }

  ConfigStore::begin();
  Config cfg = ConfigStore::get();

  StatusDisplay::begin();
  StatusDisplay::showSplash(cfg.callsign, FW_VERSION, cfg.pttLeadMs, cfg.pttTailMs);

  // --- Ethernet bring-up, Impero32-specific sequence (see design doc) ---
  WiFi.onEvent(onEthEvent);
  pinMode(ETH_OSC_EN_PIN, OUTPUT);
  digitalWrite(ETH_OSC_EN_PIN, HIGH); // enable the external 50MHz oscillator BEFORE ETH.begin()
  ETH.begin();
  if (!cfg.network.dhcp) {
    IPAddress ip, gw, sn, dns;
    ip.fromString(cfg.network.staticIp);
    gw.fromString(cfg.network.gateway);
    sn.fromString(cfg.network.subnet);
    dns.fromString(cfg.network.dns);
    ETH.config(ip, gw, sn, dns);
  }

  FskTimer::begin(cfg.baudRate, cfg.markHigh);
  TxManager::begin(cfg);
  SerialControl::begin();
  WebInterface::begin();

  bool inhibited = (digitalRead(CPU_INH_PIN) == LOW);
  StatusDisplay::showStatus(inhibited ? "INHIBIT" : "RX");
  Serial.write("\ncmd:\n"); // tell N1MM we are in RX mode (also sent at end of each TX)
}

void loop() {
  // Bit-timing is entirely interrupt-driven (FskTimer) and PTT/PA/queue
  // servicing runs in TxManager's own task -- loop() only has to pump the
  // serial-control byte-at-a-time state machine, matching the AVR
  // original's "don't bog down the processor" one-byte-per-pass approach.
  SerialControl::poll();
}
