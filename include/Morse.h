#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// International Morse code table + a CW send-buffer/element-generator,
// playing the same architectural role for Winkey/CW that Baudot.h/.cpp
// plays for RTTY: this class owns the pending buffer and turns queued
// characters (and Winkey "buffered commands") into a stream of timed
// key-down/key-up Runs for CwTimer to play back, one Run per call --
// mirrors Baudot::SendBuffer::nextSymbol()'s incremental-generator shape.
//
// Scope note (see design doc's CW/Winkey section, "maximum compatibility"
// decision, 2026-09-27): implements the timing-affecting Winkey commands
// fully (speed, weighting, Farnsworth, key compensation, first-element
// extension, PTT lead/tail via TxManager's existing sequencer) plus
// character buffering, backspace, clear-buffer, pause, prosign merging
// (0x1B) and buffered PTT-on/off/wait/nop. A few protocol corners with no
// local-hardware equivalent (paddle/pot-related commands, HSCW, dit/dah
// ratio, pin config, the buffer "pointer" command) are ACCEPTED by
// WinkeyEmulator's parser -- so a real host never desyncs talking to this
// board -- but are not yet acted on. See WinkeyEmulator.cpp for exactly
// which.
// ---------------------------------------------------------------------------

namespace Morse {

constexpr size_t BUFFER_SIZE = 128; // matches the Winkey protocol's own 128-byte host FIFO

// One timed segment of the CW waveform: either the key held down (a dit or
// dah, or part of one after weighting/key-comp adjustments) or held up (an
// intra-character, inter-character, or word gap). `asciiByte` is set (and
// non-zero) only on the Run that represents the START of a new character's
// keying -- mirrors Baudot::QueuedSymbol::asciiByte's live-text-timing role.
struct Run {
  bool keyDown = false;
  uint16_t durationMs = 0;
  uint8_t asciiByte = 0;
};

// A handful of buffered items are not CW waveform at all -- they're
// side-effect commands that must still take their turn in the SAME ordered
// buffer as the text around them (Winkey's "buffered commands", 0x18-0x1F
// range in the K1EL manual). CwBuffer surfaces these as a distinct "next
// item is a side effect, not a Run" signal (peekKind()) so the caller
// (TxManager) can let CwTimer fully drain before executing one, preserving
// order without needing the ISR/ring layer to know about them at all.
enum class SideEffect : uint8_t { PttOn, PttOff, Wait, Nop, BufferedSpeed, KeyBuffered };

enum class NextKind : uint8_t {
  None,       // buffer (and any character mid-expansion) is truly empty
  Element,    // call nextElementRun() to get the next Run
  SideEffect, // call takeSideEffect() to consume it
};

class CwBuffer {
public:
  void reset();

  // Winkey text byte (0x20-0x7E). False if the 128-byte FIFO is full,
  // mirroring the protocol's own buffer-full/XOFF behavior.
  bool addChar(uint8_t asciiByte);

  // Marks that the NEXT TWO characters added should be sent back-to-back
  // with no inter-character gap between them (Winkey buffered command
  // 0x1B, used for prosigns like <AR>/<SK>/<BT>). Applies once, to
  // whichever two Char items follow it in the buffer.
  bool addMergeMark();

  bool addSideEffect(SideEffect effect, uint8_t value = 0); // value: Wait's seconds count
  bool addCancelBufferedSpeed(); // Winkey 0x1E: relaxed to a no-op below -- see .cpp
  bool addBufferedSpeed(uint8_t wpm);

  size_t pending() const { return count_; }
  bool full() const { return count_ >= BUFFER_SIZE; }
  void backspace(); // Winkey 0x08: drop the most recently ADDED, not-yet-sent item

  // --- consumer side (TxManager's Sending-state pump, source == Winkey) ---
  NextKind peekKind();
  bool nextElementRun(Run &out);           // valid only when peekKind() == Element
  SideEffect takeSideEffect(uint8_t &value); // valid only when peekKind() == SideEffect

  // --- live timing parameters, applied to runs generated AFTER the call
  // (an in-flight character finishes at its old timing, matching the
  // Winkey spec's "changes apply to the next character" behavior) ---
  void setSpeedWpm(uint8_t wpm);          // 5-99
  void setWeightingPct(uint8_t pct);      // 10-90, 50 = normal
  void setFarnsworthWpm(uint8_t wpm);     // 0 = disabled (use setSpeedWpm timing throughout)
  void setKeyCompMs(uint8_t ms);          // 0-250
  void setFirstExtensionMs(uint8_t ms);   // 0-250

private:
  enum class ItemType : uint8_t { Char, SideEffectItem };
  struct Item {
    ItemType type;
    uint8_t value; // ascii byte, or SideEffect value (cast), or Wait seconds
    SideEffect effect;
    bool mergeWithPrev; // true if this Char should not get a leading inter-char gap
  };

  Item buf_[BUFFER_SIZE];
  size_t head_ = 0, count_ = 0;
  // 2 right after addMergeMark(), 1 after the first of the pair is added,
  // 0 otherwise -- see addMergeMark()/addChar() in the .cpp.
  uint8_t mergeCountdown_ = 0;

  // Timing parameters (ms), recomputed by recomputeTiming() whenever any
  // setter above is called.
  uint8_t speedWpm_ = 20;
  uint8_t weightingPct_ = 50;
  uint8_t farnsworthWpm_ = 0;
  uint8_t keyCompMs_ = 0;
  uint8_t firstExtMs_ = 0;
  float ditMs_ = 60.0f;
  float markScale_ = 1.0f;    // weighting's effect on key-down segments
  float gapScale_ = 1.0f;     // weighting's effect on intra-character gaps
  float interCharGapMs_ = 180.0f;
  float wordGapMs_ = 420.0f;
  void recomputeTiming();

  // --- character-expansion state (mid-character generator position) ---
  enum class Phase : uint8_t { LeadGap, Elements, IntraGap, Done };
  bool expanding_ = false;
  Phase expandingPhase_ = Phase::Done;
  const char *pattern_ = nullptr; // ".-" style, from the Morse table; nullptr for a space item
  uint8_t patternPos_ = 0;
  uint8_t expandingAscii_ = 0;
  bool expandingSuppressGap_ = false; // from Item::mergeWithPrev
  bool firstRunEver_ = true; // true until the first Run after reset(): suppresses a leading gap before the very first character of a fresh transmission
  bool suppressNextLeadGap_ = false; // a space already emitted the complete word gap

  // Snapshot taken when a character starts expanding. Immediate Winkey
  // timing changes must affect the next character, not split an in-flight
  // character between old and new timings.
  float charDitMs_ = 60.0f;
  float charMarkScale_ = 1.0f;
  float charGapScale_ = 1.0f;
  uint8_t charKeyCompMs_ = 0;
  uint8_t charFirstExtMs_ = 0;

  bool popItem(Item &out); // internal: pop the front buffer item
};

// Looks up the dit/dah pattern for one ASCII character (case-insensitive
// letters, digits, common punctuation). Returns nullptr for anything with
// no Morse representation (unsupported bytes are silently dropped by
// WinkeyEmulator, same spirit as Baudot's requiresLetters/requiresFigures
// gating). ' ' (space) is handled specially by the caller as a word gap,
// not looked up here.
const char *lookupPattern(uint8_t asciiByte);

} // namespace Morse
