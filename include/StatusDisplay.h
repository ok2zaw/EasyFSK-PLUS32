#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Status LCD (16x2, I2C) wrapper. Loosely mirrors lcdShowStatus() /
// lcdRefreshLine1() / lcdResetTxLine() / lcdAppendTxChar() / lcdShowSplash()
// from the AVR original, simplified: exact column-by-column layout parity
// with the AVR firmware was not preserved (this is a fresh, simpler
// formatting), but the *behavior* that matters for timing -- one bounded
// I2C write per call, called only from TxManager's task context, never
// from FskTimer's ISR -- is preserved exactly, per the design doc's
// "live LCD text without stealing bit time" decision.
// ---------------------------------------------------------------------------

namespace StatusDisplay {

void begin();

// One-time boot splash (callsign, firmware version, PTT/PA timing).
void showSplash(const char *callsign, const char *fwVersion, uint16_t pttLeadMs,
                 uint16_t pttTailMs);

// Line 2: "RX" / "TX" / "INHIBIT".
void showStatus(const char *line2);

// Line 1: callsign + PTT source label ("FSK" for Baudot-sourced TX, "RTS"
// for hardware-RTS-sourced TX) + polarity indicator.
void refreshLine1(const char *callsign, const char *sourceLabel, bool markHigh);

// Clears line 2 and resets the live-text write column to the start --
// called right before a new TX's first character, and right after a TX
// ends (ready for the next one).
void resetTxLine();

// Appends one live-transmitted character to line 2, wrapping back to
// column 0 once the line is full. Called from TxManager's task context
// only, in response to FskTimer::takeCharStarted() -- never from the ISR.
void appendTxChar(uint8_t asciiByte);

} // namespace StatusDisplay
