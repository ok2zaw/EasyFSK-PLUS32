#pragma once

#include <Arduino.h>
#include "Morse.h"

// ---------------------------------------------------------------------------
// CW element-timer ISR + lookahead ring buffer -- the Winkey/CW counterpart
// to FskTimer.h, same architectural role: the ONLY code that toggles
// FSK_PIN while a Winkey-sourced transmission is active (FSK_PIN is REUSED
// as the plain CW key line per the design doc -- idle during CW since the
// Baudot tone generator isn't running then), driven from a real hardware
// timer ISR so keying timing doesn't depend on task scheduling latency.
//
// Unlike FskTimer (which reprograms its timer alarm period per RTTY baud
// rate), CwTimer runs its timer at a FIXED 1ms tick and counts down a
// per-Run millisecond counter -- simpler, and plenty of resolution for CW
// (5-99 WPM gives dit lengths from 240ms down to ~12ms; RTTY's half-bit
// timing needs much tighter tolerances than human/decoder-read Morse does).
// ---------------------------------------------------------------------------

namespace CwTimer {

// Sets up the fixed 1ms timer and attaches the ISR. Call once from setup(),
// after pinMode() on FSK_PIN (shared with FskTimer -- whichever of the two
// engines is actually active is the only one touching the pin's level at
// any given moment, enforced by TxManager's single-state-machine design).
void begin();

// TxManager (sole writer) tells the ISR whether it should be running at
// all. Mirrors FskTimer::setActive(), minus the RTS-session concept (CW has
// no equivalent -- Winkey is the only source that ever uses this engine).
void setActive(bool active);

// Resets the ISR-side state to a clean idle start. Called by TxManager
// exactly once, right before it sets active(true) for a new Winkey-sourced
// transmission.
void resetForNewTx();

// Direct FSK_PIN level control for TxManager's lead/tail state machine,
// mirroring FskTimer::holdMark()/holdSpace(). ONLY safe to call while the
// ISR is inactive (active == false). CW's "key up" idle level is simply
// LOW (no mark/space polarity concept -- this is a plain on/off key line).
void holdKeyUp();

// Producer-side (TxManager) ring buffer access, same shape as FskTimer's.
bool ringHasFreeSlot();
bool pushRun(const Morse::Run &run);

// True once the ring is empty AND the ISR has finished playing whatever
// Run it was last given -- i.e. the key line is fully idle and it's safe
// to execute a buffered side-effect (PTT on/off, wait) without reordering
// it ahead of CW that hasn't gone out yet. TxManager polls this before
// consuming a Morse::SideEffect item from CwBuffer.
bool isIdle();

// One-shot flag, same contract as FskTimer::takeCharStarted(): true (and
// cleared) at most once per event.
bool takeCharStarted(uint8_t &asciiByte);

} // namespace CwTimer
