#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// Baudot (ITA2/US 5-bit) encoding engine, ported from getNextSendChar() and
// its supporting tables/state in the AVR original (TinyFSK_ZAW_01.cpp).
//
// Behavior is preserved as closely as possible:
//   - Same ASCII->Baudot table.
//   - Same shift-state tracking (LTRS/FIGS) and same USOS handling --
//     fixed at the AVR's USOS_MMTTY_HACK mode (the only mode the AVR
//     firmware actually shipped with; USOS_OFF/USOS_ON were compile-time
//     alternatives never exposed to the user there either, so they're not
//     carried over as a runtime option here -- see design doc).
//   - Same 500-byte send buffer size and same shift-injection logic.
//
// What's DIFFERENT from the AVR original, deliberately, per the design
// doc's ESP32 architecture: this class only decides *which Baudot symbol
// comes next* (nextSymbol()). It does NOT bit-bang FSK_PIN itself -- that
// happens in the timer ISR (see FskTimer.h), which plays back symbols
// this class produces one character-period ahead of time. Because of that
// lookahead, Serial echo() of a typed character now happens up to ~1
// character-period *before* that character is actually on the air
// (harmless -- echo is a local terminal-feedback convenience, not part of
// the RF signal). Live LCD/WebSocket text, which the design doc requires
// to stay genuinely synced to on-air timing, is NOT done here -- see
// QueuedSymbol::asciiByte and FskTimer's "character started" signal.
// ---------------------------------------------------------------------------

namespace Baudot {

constexpr uint8_t LTRS_SHIFT = 0x1F;
constexpr uint8_t FIGS_SHIFT = 0x1B;
constexpr uint8_t SHIFT_UNKNOWN = 0;
constexpr uint8_t TX_END_FLAG = 0xFF; // sentinel: "nothing left to send, unkey"

// Stop-bit half-period count. Carried over verbatim from the AVR
// original's `stopBits = STOP_BITS_1R5` (numeric value 2) -- see the
// source's own comment block above processHalfBit() for how this counts
// half-bit periods. Preserved as-is for exact on-air timing parity rather
// than re-derived from the "1.5 stop bits" label.
constexpr uint8_t STOP_BIT_HALF_PERIODS = 2;

constexpr size_t SEND_BUFFER_SIZE = 500; // matches AVR SEND_BUFFER_SIZE

bool requiresLetters(uint8_t asciiByte);
bool requiresFigures(uint8_t asciiByte);

// One Baudot symbol (one full start+data+stop frame) ready for the timer
// ISR to play back.
struct QueuedSymbol {
  uint8_t baudotCode; // 5-bit Baudot value, or LTRS_SHIFT/FIGS_SHIFT/TX_END_FLAG
  uint8_t asciiByte;  // source ASCII byte this frame represents for live-text
                       // purposes, or 0 if this frame carries no live-text
                       // character (a pure shift symbol or an idle diddle)
};

// Owns the pending-text buffer and shift-state, mirrors sendBufferArray /
// sendBufferBytes / currentShiftState / endWhenBufferEmpty / lastAsciiByteSent
// from the AVR original. Not thread-safe by itself -- the design doc makes
// the TX Manager task the sole owner/caller of this class; producers
// (serial, RTS, web) go through the TX Manager's queue instead of touching
// this directly.
class SendBuffer {
public:
  void reset();                 // mirrors resetSendBuffer() + shift-state reset
  bool addByte(uint8_t b);      // mirrors addToSendBuffer(); false if buffer is full
  size_t pending() const { return count_; }

  bool endWhenBufferEmpty = true;

  // Computes the next symbol to send. Mirrors getNextSendChar() exactly,
  // including the shift-injection rules and the USOS_MMTTY_HACK special
  // case, minus the LCD/echo side effects (Serial echo is done here since
  // it's not on-air-timing-sensitive; see QueuedSymbol::asciiByte for how
  // the on-air-synced LCD/WebSocket update is deferred to FskTimer).
  Baudot::QueuedSymbol nextSymbol();

private:
  uint8_t buffer_[SEND_BUFFER_SIZE];
  size_t count_ = 0;
  uint8_t currentShiftState_ = SHIFT_UNKNOWN;
  uint8_t lastSentBaudot_ = LTRS_SHIFT; // mirrors `sendingChar` at call time
};

} // namespace Baudot
