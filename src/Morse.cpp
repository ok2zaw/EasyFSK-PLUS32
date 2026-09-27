#include "Morse.h"

namespace Morse {

namespace {

struct TableEntry {
  char ascii;
  const char *pattern;
};

// International Morse code (ITU-R M.1677-1), uppercase letters, digits, and
// the common punctuation Winkey hosts actually send. Prosigns (AR/SK/BT/KN/
// etc.) are NOT separate entries -- they're just two of these letters sent
// via the 0x1B merge-mark buffered command, per the real protocol.
constexpr TableEntry kTable[] = {
    {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},
    {'E', "."},     {'F', "..-."},  {'G', "--."},   {'H', "...."},
    {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
    {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},
    {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
    {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
    {'Y', "-.--"},  {'Z', "--.."},
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
    {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
    {'8', "---.."}, {'9', "----."},
    {'.', ".-.-.-"}, {',', "--..--"}, {'?', "..--.."}, {'\'', ".----."},
    {'!', "-.-.--"}, {'/', "-..-."},  {'(', "-.--."},  {')', "-.--.-"},
    {'&', ".-..."},  {':', "---..."}, {';', "-.-.-."}, {'=', "-...-"},
    {'+', ".-.-."},  {'-', "-....-"}, {'_', "..--.-"}, {'"', ".-..-."},
    {'$', "...-..-"}, {'@', ".--.-."},
};

} // namespace

const char *lookupPattern(uint8_t asciiByte) {
  char c = static_cast<char>(asciiByte);
  if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A'); // case-fold
  for (const auto &e : kTable) {
    if (e.ascii == c) return e.pattern;
  }
  return nullptr; // unsupported byte -- caller skips it
}

// --- CwBuffer ----------------------------------------------------------------

void CwBuffer::reset() {
  cancelBufferedSpeed(); // a buffered speed belongs to the content being dropped
  head_ = 0;
  count_ = 0;
  mergeCountdown_ = 0;
  expanding_ = false;
  expandingPhase_ = Phase::Done;
  pattern_ = nullptr;
  patternPos_ = 0;
  firstRunEver_ = true;
  suppressNextLeadGap_ = false;
}

bool CwBuffer::popItem(Item &out) {
  if (count_ == 0) return false;
  out = buf_[head_];
  head_ = (head_ + 1) % BUFFER_SIZE;
  count_--;
  return true;
}

bool CwBuffer::addChar(uint8_t asciiByte) {
  if (full()) return false;
  Item it;
  it.type = ItemType::Char;
  it.value = asciiByte;
  it.effect = SideEffect::Nop; // unused for Char items
  it.mergeWithPrev = false;
  if (mergeCountdown_ == 2) {
    mergeCountdown_ = 1; // first of the merge pair: normal leading gap before it
  } else if (mergeCountdown_ == 1) {
    it.mergeWithPrev = true; // second of the pair: no gap between the two
    mergeCountdown_ = 0;
  }
  buf_[(head_ + count_) % BUFFER_SIZE] = it;
  count_++;
  return true;
}

bool CwBuffer::addMergeMark() {
  // Doesn't occupy a buffer slot itself -- just arms the next two addChar()
  // calls, which happen synchronously from the same parser, so there's no
  // ordering hazard versus items already queued.
  mergeCountdown_ = 2;
  return true;
}

bool CwBuffer::addSideEffect(SideEffect effect, uint8_t value) {
  if (full()) return false;
  Item it;
  it.type = ItemType::SideEffectItem;
  it.effect = effect;
  it.value = value;
  it.mergeWithPrev = false;
  buf_[(head_ + count_) % BUFFER_SIZE] = it;
  count_++;
  return true;
}

bool CwBuffer::addBufferedSpeed(uint8_t wpm) {
  // A genuinely BUFFERED speed change (0x1C) takes effect only once the CW
  // engine reaches this point in playback, unlike the immediate 0x02
  // command. Modeled as a side effect so it takes its turn in order;
  // TxManager's consumer calls applyBufferedSpeed(value) when it pops it.
  return addSideEffect(SideEffect::BufferedSpeed, wpm);
}

bool CwBuffer::addCancelBufferedSpeed() {
  // Winkey 0x1E ("cancel buffered speed change, restore previous") -- also
  // buffered, so text queued ahead of it still goes out at the override.
  return addSideEffect(SideEffect::CancelBufferedSpeed);
}

void CwBuffer::applyBufferedSpeed(uint8_t wpm) {
  if (baseSpeedWpm_ == 0) baseSpeedWpm_ = speedWpm_; // nested 0x1C keeps the original base
  applySpeed(wpm);
}

void CwBuffer::cancelBufferedSpeed() {
  if (baseSpeedWpm_ == 0) return;
  uint8_t base = baseSpeedWpm_;
  baseSpeedWpm_ = 0;
  applySpeed(base);
}

void CwBuffer::backspace() {
  // Winkey 0x08: drop the most recently ADDED item that hasn't started
  // playing yet. Safe no-op if the buffer is empty or already mid-playback
  // of everything queued (matches "backspace only affects buffered, not
  // in-flight, content").
  if (count_ == 0) return;
  count_--;
}

void CwBuffer::setSpeedWpm(uint8_t wpm) {
  // An explicit immediate speed wins over a buffered override -- there is
  // nothing left to restore once the host has set a new base speed.
  baseSpeedWpm_ = 0;
  applySpeed(wpm);
}

void CwBuffer::applySpeed(uint8_t wpm) {
  if (wpm < 5) wpm = 5;
  if (wpm > 99) wpm = 99;
  speedWpm_ = wpm;
  recomputeTiming();
}

void CwBuffer::setWeightingPct(uint8_t pct) {
  cancelBufferedSpeed();
  if (pct < 10) pct = 10;
  if (pct > 90) pct = 90;
  weightingPct_ = pct;
  recomputeTiming();
}

void CwBuffer::setFarnsworthWpm(uint8_t wpm) {
  cancelBufferedSpeed();
  farnsworthWpm_ = wpm;
  recomputeTiming();
}

void CwBuffer::setKeyCompMs(uint8_t ms) {
  cancelBufferedSpeed();
  keyCompMs_ = ms;
  recomputeTiming();
}

void CwBuffer::setFirstExtensionMs(uint8_t ms) {
  firstExtMs_ = ms;
  recomputeTiming();
}

void CwBuffer::recomputeTiming() {
  // PARIS standard: one dit-unit = 1200/wpm ms at the "character" (element)
  // speed set by setSpeedWpm().
  ditMs_ = 1200.0f / static_cast<float>(speedWpm_);

  // Weighting (K1EL 10-90%, 50 = normal 1:3 dit:dah with equal-length
  // intra-character gaps): scales key-down segments up and intra-character
  // gaps down by the complementary amount, so total per-element-period time
  // (and therefore overall WPM) is unchanged -- APPROXIMATE, not verified
  // byte-for-byte against a real K1EL Winkey; flagged for bench comparison
  // in the design doc.
  markScale_ = static_cast<float>(weightingPct_) / 50.0f;
  gapScale_ = 2.0f - markScale_;
  if (gapScale_ < 0.2f) gapScale_ = 0.2f; // guard against pathological 90% weighting

  // Inter-character/word gaps: plain (non-Farnsworth) timing is the
  // standard 3/7 dit-unit multiples of the character-speed dit.
  float plainInterChar = ditMs_ * 3.0f;
  float plainWord = ditMs_ * 7.0f;

  if (farnsworthWpm_ > 0 && farnsworthWpm_ < speedWpm_) {
    // Standard "PARIS word" Farnsworth approximation: a PARIS word is 50
    // dit-units total, of which 31 units are within-character content (sent
    // at the full character speed) and 19 units are inter-character/word
    // gaps (stretched to hit the slower effective WPM). This is the widely
    // published approximation, not independently re-derived here.
    float totalMsAtFarnsworth = 60000.0f / static_cast<float>(farnsworthWpm_);
    float inCharMs = 31.0f * ditMs_;
    float gapBudgetMs = totalMsAtFarnsworth - inCharMs;
    if (gapBudgetMs < 19.0f * ditMs_) {
      // Farnsworth WPM not actually slower once rounding/edge values are
      // considered -- fall back to plain timing rather than go negative.
      interCharGapMs_ = plainInterChar;
      wordGapMs_ = plainWord;
    } else {
      float gapUnitMs = gapBudgetMs / 19.0f;
      interCharGapMs_ = gapUnitMs * 3.0f;
      wordGapMs_ = gapUnitMs * 7.0f;
    }
  } else {
    interCharGapMs_ = plainInterChar;
    wordGapMs_ = plainWord;
  }
}

NextKind CwBuffer::peekKind() {
  // A character remains marked as expanding until its final Run has been
  // returned. Normalize that terminal state here so peekKind() never claims
  // an Element is available when nextElementRun() would only clean up and
  // return false (and so a following buffered side effect is visible
  // immediately after the character's last Run).
  if (expanding_ && expandingPhase_ == Phase::Done) {
    expanding_ = false;
    pattern_ = nullptr;
  }
  if (expanding_) return NextKind::Element; // mid-character; more Runs pending regardless of buffer content
  if (count_ == 0) return NextKind::None;
  const Item &front = buf_[head_];
  return (front.type == ItemType::SideEffectItem) ? NextKind::SideEffect : NextKind::Element;
}

Morse::SideEffect CwBuffer::takeSideEffect(uint8_t &value) {
  Item it;
  popItem(it); // caller guarantees peekKind() == SideEffect just returned true
  value = it.value;
  return it.effect;
}

bool CwBuffer::nextElementRun(Run &out) {
  for (;;) {
    if (!expanding_) {
      if (count_ == 0) return false;
      // PEEK, don't pop yet: if the front item is a SideEffect, it must be
      // left in place for takeSideEffect() to consume via the dedicated
      // path (ordering/PTT/wait handling lives there) -- popping and
      // discarding it here, as an earlier version of this function did,
      // would silently drop buffered PTT-on/off/wait/speed-change commands
      // whenever this function's own internal Phase::Done continuation (as
      // opposed to a fresh top-level peekKind() call) was the one to reach
      // them first.
      if (buf_[head_].type == ItemType::SideEffectItem) {
        return false;
      }
      Item it;
      popItem(it); // known to be a Char item, per the check above
      expandingAscii_ = it.value;
      expandingSuppressGap_ = it.mergeWithPrev;
      pattern_ = (expandingAscii_ == ' ') ? nullptr : lookupPattern(expandingAscii_);
      if (expandingAscii_ != ' ' && pattern_ == nullptr) {
        continue; // unsupported character -- silently skip, try the next item
      }
      charDitMs_ = ditMs_;
      charMarkScale_ = markScale_;
      charGapScale_ = gapScale_;
      charKeyCompMs_ = keyCompMs_;
      charFirstExtMs_ = firstExtMs_;
      patternPos_ = 0;
      expanding_ = true;
      expandingPhase_ = Phase::LeadGap;
    }

    switch (expandingPhase_) {
      case Phase::LeadGap: {
        bool isSpaceItem = (pattern_ == nullptr);
        expandingPhase_ = isSpaceItem ? Phase::Done : Phase::Elements;

        if (isSpaceItem) {
          if (expandingSuppressGap_ || firstRunEver_) {
            // Leading spaces (and a space used as the second half of a
            // merged prosign) do not create a delay or consume the
            // first-character state.
            continue;
          }
          out.keyDown = false;
          out.durationMs = static_cast<uint16_t>(wordGapMs_);
          out.asciiByte = 0;
          suppressNextLeadGap_ = true;
          return true;
        }

        if (expandingSuppressGap_ || firstRunEver_ || suppressNextLeadGap_) {
          firstRunEver_ = false;
          suppressNextLeadGap_ = false;
          continue; // no gap Run this time -- fall straight into the character
        }

        out.keyDown = false;
        out.durationMs = static_cast<uint16_t>(interCharGapMs_);
        out.asciiByte = 0;
        return true;
      }

      case Phase::Elements: {
        bool isFirstElement = (patternPos_ == 0);
        char sym = pattern_[patternPos_];
        patternPos_++;
        bool moreElements = (pattern_[patternPos_] != '\0');

        float dur = (sym == '-') ? (charDitMs_ * 3.0f) : charDitMs_;
        dur *= charMarkScale_;
        dur += charKeyCompMs_;
        if (isFirstElement) dur += charFirstExtMs_;
        if (dur < 1.0f) dur = 1.0f;

        out.keyDown = true;
        out.durationMs = static_cast<uint16_t>(dur + 0.5f);
        out.asciiByte = isFirstElement ? expandingAscii_ : 0;
        expandingPhase_ = moreElements ? Phase::IntraGap : Phase::Done;
        return true;
      }

      case Phase::IntraGap: {
        float dur = charDitMs_ * charGapScale_ - static_cast<float>(charKeyCompMs_);
        if (dur < 1.0f) dur = 1.0f;
        out.keyDown = false;
        out.durationMs = static_cast<uint16_t>(dur + 0.5f);
        out.asciiByte = 0;
        expandingPhase_ = Phase::Elements;
        return true;
      }

      case Phase::Done:
        expanding_ = false;
        pattern_ = nullptr;
        continue; // pop the next buffered item on the next loop iteration
    }
  }
}

} // namespace Morse
