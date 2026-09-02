#pragma once

#include <Arduino.h>
#include "Config.h"

// ---------------------------------------------------------------------------
// TX Manager: the single owner of send-buffer state, PTT/PA sequencing, and
// shift state, per the design doc's "Firmware task architecture" section.
// Runs as its own FreeRTOS task (pinned to the same core as the timer ISR)
// and is the only code that ever calls into Baudot::SendBuffer or FskTimer's
// producer-side API.
//
// Every other producer (serial-receive, RTS-pin poll [handled internally],
// the web Send/End/Abort handlers) submits short messages through the
// enqueueXxx() functions below instead of touching TX state directly --
// this is what replaces the AVR original's implicit single-threadedness now
// that there are three possible sources of TX commands.
//
// PTT/PA lead-tail sequencing is a NON-BLOCKING state machine (checked once
// per task iteration via millis()), not blocking delay()/waitDrainingSerial()
// like the AVR original. This is a deliberate, user-approved improvement:
// the AVR ignores TX_ABORT/TX_END/TX_ON while a lead delay is in progress;
// this version keeps servicing the queue (including abort) throughout every
// state, so an abort takes effect immediately even mid-lead-delay.
// ---------------------------------------------------------------------------

namespace TxManager {

enum class Source : uint8_t { SerialLink, Rts, Web };

// Creates the command queue and the TX Manager FreeRTOS task. Call once
// from setup(), after Config/FskTimer/Pins are initialized. `cfg` is
// copied in; call applyConfig() again later if the user saves new PTT/PA
// timing or polarity/baud values.
void begin(const Config &cfg);

// Re-reads timing/polarity/baud values from `cfg` for use on the *next*
// transmission. Safe to call at any time -- like the AVR original, a
// config change never affects a TX already in progress, only future ones.
void applyConfig(const Config &cfg);

// Producer-side API. All of these are safe to call from any task (serial
// handling, the web server's task, etc.) -- they just post to an internal
// FreeRTOS queue and return quickly. Returns false only if the queue is
// momentarily full (shouldn't happen in practice at RTTY speeds/buffer
// sizes; callers may treat it as backpressure).
bool enqueueKeyUp(Source src);      // mirrors TX_ON ('[')
bool enqueueBufferedEnd();          // mirrors TX_END (']')
bool enqueueAbort();                // mirrors TX_ABORT ('\')
bool enqueueByte(uint8_t b, Source src); // mirrors addToSendBuffer()

// Status snapshot for the web/LCD status feed. `revision` increments on
// every state change -- callers (the web layer's status-push loop) can
// cheaply detect "did anything change since I last looked" without
// comparing full structs.
struct Status {
  bool txActive = false;
  bool paActive = false;
  bool inhibited = false;
  Source pttSource = Source::SerialLink;
  uint16_t bufferPending = 0;
  char lastChar = 0;   // most recent character that started transmitting
  uint32_t charSeq = 0; // increments each time lastChar changes; independent
                        // readers (LCD, WebSocket push loop) each track their
                        // own last-seen charSeq to detect a new character
                        // without stepping on each other
  uint32_t revision = 0; // increments on any status field change at all
};

// Thread-safe-ish snapshot read (short critical section).
Status getStatus();

} // namespace TxManager
