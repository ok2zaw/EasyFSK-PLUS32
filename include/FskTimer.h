#pragma once

#include <Arduino.h>
#include "Baudot.h"

// ---------------------------------------------------------------------------
// Half-bit hardware timer ISR + lookahead ring buffer.
//
// Per the design doc's architecture, this is the ONLY code that ever
// toggles FSK_PIN, and it does so from inside the real ESP32 hardware
// timer ISR (ARDUINO_ISR_ATTR) -- not from loop()/task context like the
// AVR original -- so on-air bit timing no longer depends on task
// scheduling latency at all, even with LAN/web traffic competing for CPU.
//
// The ISR's state machine (bitPos_/stopBitCounter_/midBit_) is a direct
// port of processHalfBit() from TinyFSK_ZAW_01.cpp. The only structural
// change: instead of calling getNextSendChar() synchronously, it pops a
// pre-computed Baudot::QueuedSymbol from a small lock-free single-
// producer/single-consumer ring buffer that TxManager keeps filled ~1
// character-period ahead of time (huge slack at RTTY speeds: 93-154ms per
// character vs microseconds of ISR work). The ISR never touches
// PTT_PIN/PTT_PA_PIN, I2C, flash, or the send-text queue directly.
// ---------------------------------------------------------------------------

namespace FskTimer {

// Sets up the hardware timer at 2x the given baud rate (one interrupt per
// half-bit) and attaches the ISR. Call once from setup(), after pinMode()
// on FSK_PIN.
void begin(float baudRate, bool markHigh);

// Changes baud rate / polarity. Only safe to call while idle (no active
// TX) -- caller (TxManager, servicing a config save) is responsible for
// that gating, same as the flash-write-during-TX rule.
void reconfigure(float baudRate, bool markHigh);

// Resets the ISR-side bit-frame state machine to a clean start (mirrors
// resetChar() in the AVR original) and clears the ring buffer. Called by
// TxManager exactly once, right before it sets active(true) for a new
// Baudot-sourced transmission.
void resetForNewTx();

// TxManager (sole writer) tells the ISR whether it should be running at
// all, and whether this is an RTS-sourced session (in which case the ISR
// leaves FSK_PIN alone entirely -- mirrors the AVR original's `!ptt ||
// rtsKeyed` early-return in processHalfBit()).
void setActive(bool active, bool rtsKeyed);

// Direct FSK_PIN level control for TxManager's lead/tail state machine.
// ONLY safe to call while the ISR is inactive (active == false) or during
// an RTS-sourced session (where the ISR never touches FSK_PIN itself) --
// mirrors the handful of direct digitalWrite(FSK_PIN, ...) calls the AVR
// original makes from inside setPTT(), outside of processHalfBit().
void holdMark();  // FSK_PIN = mark level (start of a Baudot-sourced key-up)
void holdSpace(); // FSK_PIN = space level (end of a Baudot-sourced session)
void holdLow();   // FSK_PIN = raw LOW (RTS-sourced sessions, both ends)

// Producer-side (TxManager) ring buffer access. pushSymbol() returns
// false if the ring is momentarily full (TxManager should just try again
// on its next loop iteration -- at RTTY speeds this should not happen in
// practice given the depth chosen below).
bool ringHasFreeSlot();
bool pushSymbol(const Baudot::QueuedSymbol &sym);

// One-shot flags the ISR sets and TxManager polls/clears. Each returns
// true (and clears the flag) at most once per event -- TxManager is
// expected to poll frequently enough not to miss one (it does, given the
// large timing margins at RTTY speeds; a missed charStarted is cosmetic
// only -- one live-text update skipped, never a safety issue).
bool takeCharStarted(uint8_t &asciiByte);
bool takeEndOfData();

// Diagnostic only (exposed via GET /api/system): counts ring-buffer
// underruns (ISR needed a symbol, ring was empty, fell back to an idle
// LTRS diddle). Should stay at 0 in normal operation.
uint32_t underrunCount();

} // namespace FskTimer
