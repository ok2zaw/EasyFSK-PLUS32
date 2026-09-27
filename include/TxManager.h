#pragma once

#include <Arduino.h>
#include "Config.h"
#include "Morse.h"

// ---------------------------------------------------------------------------
// TX Manager: the single owner of send-buffer state, PTT/PA sequencing, and
// shift state, per the design doc's "Firmware task architecture" section.
// Runs as its own FreeRTOS task (pinned to the same core as the timer ISR)
// and is the only code that ever calls into Baudot::SendBuffer/FskTimer's
// or Morse::CwBuffer/CwTimer's producer-side APIs.
//
// Every other producer (UART1 serial-receive, UART2 in "fsk2" mode, the
// RTS-style hardware input [handled internally], the web Send handler, and
// now WinkeyEmulator) submits short messages through the enqueueXxx()/
// cwXxx() functions below instead of touching TX state directly.
//
// 2026-09-27: extended for the "UART1/UART2 split" + Winkey/CW keying
// architecture (see the design doc). Two independent playback engines now
// share this one state machine and its PTT/PA lead-tail sequencer:
//   - Baudot/FskTimer, for RTTY (Source::SerialLink and Source::Uart2Fsk).
//   - Morse/CwTimer, for CW (Source::Winkey only).
// Exactly one of the two is ever active at a time -- the state machine's
// existing single-`s_state` design already enforces "only one TX session at
// a time regardless of source", which is what keeps the two engines from
// ever fighting over the shared FSK_PIN.
//
// PTT/PA lead-tail sequencing is a NON-BLOCKING state machine (checked once
// per task iteration via millis()), not blocking delay()/waitDrainingSerial()
// like the AVR original. This is a deliberate, user-approved improvement:
// the AVR ignores TX_ABORT/TX_END/TX_ON while a lead delay is in progress;
// this version keeps servicing the queue (including abort) throughout every
// state, so an abort takes effect immediately even mid-lead-delay.
// ---------------------------------------------------------------------------

namespace TxManager {

// Drives every safety-relevant output to its inactive level and configures
// the hardware interlock inputs. Call as the very first operation in setup(),
// before Serial, LittleFS, LCD, Ethernet, timers, or any boot splash. begin()
// calls it again defensively, so an omitted early call still fails safe once
// TxManager itself is initialized.
void prepareSafePins();

// FSK1 = UART1 (always-on RTTY control), Uart2Fsk = UART2 in "fsk2" mode
// (a second, independent RTTY control input, same framing as UART1),
// Winkey = UART2 in "cw" mode (Winkey protocol emulation, CW keying).
enum class Source : uint8_t { SerialLink, Uart2Fsk, Rts, Web, Winkey };

// Creates the command queue and the TX Manager FreeRTOS task. Call once
// from setup(), after Config/FskTimer/CwTimer/Pins are initialized. `cfg`
// is copied in; call applyConfig() again later if the user saves new
// PTT/PA timing, polarity/baud, or CW timing values.
void begin(const Config &cfg);

// Re-reads timing/polarity/baud values from `cfg` for use on the *next*
// transmission. Safe to call at any time -- like the AVR original, a
// config change never affects a TX already in progress, only future ones.
void applyConfig(const Config &cfg);

// --- RTTY/Baudot producer-side API (Source::SerialLink, Uart2Fsk, Rts, Web) ---
bool enqueueKeyUp(Source src);      // mirrors TX_ON ('[')
bool enqueueBufferedEnd();          // mirrors TX_END (']')
bool enqueueAbort();                // mirrors TX_ABORT ('\')
bool enqueueByte(uint8_t b, Source src); // mirrors addToSendBuffer()

// Number of currently unused command-queue slots. Intended for producers
// that enqueue a burst synchronously (the web Send endpoint) so they can
// reject an oversized request before queuing a partial transmission.
size_t commandQueueFreeSlots();

// --- CW/Winkey producer-side API (Source::Winkey only; called by
// WinkeyEmulator). Each Add-style call implicitly enqueues a KeyUp(Winkey)
// first when idle (harmless/ignored if a CW session is already active,
// same "redundant TX_ON" guard as the RTTY path) -- callers don't need to
// track session lifecycle themselves, just push content as it arrives from
// the host. All timing setters take effect on characters generated AFTER
// the call (an in-flight character finishes at its old timing). ---
bool cwEnqueueChar(uint8_t asciiByte);
bool cwEnqueueMergeMark();               // Winkey 0x1B: no gap before the NEXT char queued
bool cwEnqueueSideEffect(Morse::SideEffect effect, uint8_t value = 0);
bool cwEnqueueBufferedSpeed(uint8_t wpm); // Winkey 0x1C (buffered, takes its turn in order)
bool cwEnqueueCancelBufferedSpeed();      // Winkey 0x1E: restore the speed in force before 0x1C
void cwCancelBufferedSpeedOverride();     // immediate mode/ratio changes also restore the base speed
void cwBackspace();                       // Winkey 0x08
void cwClearPendingBuffer();              // Winkey 0x0A: drop the buffer AND stop the in-flight character
uint16_t cwBufferPending();                // for the Winkey status byte's XOFF bit

void cwSetSpeedWpm(uint8_t wpm);          // Winkey 0x02 (immediate)
void cwSetWeightingPct(uint8_t pct);      // Winkey 0x03
void cwSetFarnsworthWpm(uint8_t wpm);     // Winkey 0x0D (0 = disabled)
void cwSetKeyCompMs(uint8_t ms);          // Winkey 0x11
void cwSetFirstExtensionMs(uint8_t ms);   // Winkey 0x10
void cwSetTuneKeyDown(bool down);         // Winkey 0x0B "Key Immediate" (antenna-tune carrier)
void cwSetPttLeadTail(uint16_t leadMs, uint16_t tailMs); // Winkey 0x04, values already ×10ms-decoded

// Status snapshot for the web/LCD status feed. `revision` increments on
// every state change -- callers (the web layer's status-push loop) can
// cheaply detect "did anything change since I last looked" without
// comparing full structs.
struct Status {
  bool txActive = false;
  bool ending = false;  // in the PTT/PA tail: the session is over, only the relays are still held
  bool pttActive = false;
  bool paActive = false;
  bool inhibited = false;
  Source pttSource = Source::SerialLink;
  uint16_t bufferPending = 0;     // pending text of the ACTIVE session's engine
  uint16_t cwBufferPending = 0;   // CW buffer, always -- for the Winkey XOFF/BUSY bits
  char lastChar = 0;   // most recent character that started transmitting
  uint32_t charSeq = 0; // increments each time lastChar changes; independent
                        // readers (LCD, WebSocket push loop) each track their
                        // own last-seen charSeq to detect a new character
                        // without stepping on each other
  uint32_t revision = 0; // increments on any status field change at all
};

// Thread-safe-ish snapshot read (short critical section).
Status getStatus();

// LCD/status source label -- FSK1/FSK2/RTS/WEB/CWK, see the design doc's
// "UART1/UART2 split" LCD-display decision (2026-09-27).
const char *sourceLabel(Source src);

} // namespace TxManager
