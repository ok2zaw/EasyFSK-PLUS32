#include "Baudot.h"

namespace Baudot {

// ASCII -> Baudot (ITA2/US 5-bit) translation table, copied verbatim from
// the AVR original's `asciiToBaudot[127]`. Any ASCII control character not
// otherwise handled maps to Baudot NULL (0); punctuation with no Baudot
// equivalent maps to '?' (25).
static const uint8_t asciiToBaudot[127] = {
    0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 2, 0, 0, 8, 0, 0,      // 0-15
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      // 16-31
    4, 13, 17, 20, 9, 25, 26, 11, 15, 18, 25, 17, 12, 3, 28, 29, // 32-47 (space..'/')
    22, 23, 19, 1, 10, 16, 21, 7, 6, 24, 14, 30, 25, 30, 25, 25, // 48-63 ('0'..'?')
    25, 3, 25, 14, 9, 1, 13, 26, 20, 6, 11, 15, 18, 28, 12, 24,  // 64-79 ('@'..'O')
    22, 23, 10, 5, 16, 7, 30, 19, 29, 21, 17, 15, 20, 18, 25, 4, // 80-95 ('P'..'_')
    25, 3, 25, 14, 9, 1, 13, 26, 20, 6, 11, 15, 18, 28, 12, 24,  // 96-111 ('`'..'o')
    22, 23, 10, 5, 16, 7, 30, 19, 29, 21, 17, 15, 20, 18, 25,    // 112-126 ('p'..'~')
};

bool requiresLetters(uint8_t asciiByte) {
  return (asciiByte >= 'A' && asciiByte <= 'Z') || (asciiByte >= 'a' && asciiByte <= 'z');
}

bool requiresFigures(uint8_t asciiByte) {
  return !requiresLetters(asciiByte) && asciiByte != 0x00 && asciiByte != 0x0A &&
         asciiByte != 0x0D && asciiByte != ' ';
}

void SendBuffer::reset() {
  count_ = 0;
  currentShiftState_ = SHIFT_UNKNOWN;
  lastSentBaudot_ = LTRS_SHIFT;
  endWhenBufferEmpty = true;
}

void SendBuffer::beginSession() {
  currentShiftState_ = SHIFT_UNKNOWN;
  lastSentBaudot_ = LTRS_SHIFT;
}

bool SendBuffer::addByte(uint8_t b) {
  if (count_ >= SEND_BUFFER_SIZE) {
    return false;
  }
  buffer_[count_++] = b;
  return true;
}

QueuedSymbol SendBuffer::nextSymbol() {
  QueuedSymbol result{LTRS_SHIFT, 0};

  if (count_ > 0) {
    uint8_t asciiByte = buffer_[0];

    if (currentShiftState_ != LTRS_SHIFT && requiresLetters(asciiByte)) {
      result.baudotCode = LTRS_SHIFT;
    } else if (currentShiftState_ != FIGS_SHIFT && requiresFigures(asciiByte)) {
      result.baudotCode = FIGS_SHIFT;
    } else if (currentShiftState_ != LTRS_SHIFT && requiresFigures(asciiByte) &&
               lastSentBaudot_ == 0x04 /* Baudot space, MMTTY-hack robustness case */) {
      result.baudotCode = FIGS_SHIFT;
    } else {
      uint8_t code = (asciiByte < 127) ? asciiToBaudot[asciiByte] : 0;
      result.baudotCode = code;
      result.asciiByte = asciiByte;

      // consume the byte, shift the rest of the buffer down (matches the
      // AVR original's simple array-shift approach -- buffer is small and
      // this isn't on the ISR's critical path)
      count_--;
      for (size_t i = 0; i < count_; i++) {
        buffer_[i] = buffer_[i + 1];
      }

      Serial.write(asciiByte); // local terminal echo, see header comment
    }
  } else {
    if (endWhenBufferEmpty) {
      result.baudotCode = TX_END_FLAG;
    } else {
      result.baudotCode = (currentShiftState_ == SHIFT_UNKNOWN) ? LTRS_SHIFT : currentShiftState_;
    }
  }

  // Shift-state update happens once per completed frame in the AVR
  // original (at stop-bit completion, functionally equivalent to "right
  // before the next symbol is decided", which is where we are now).
  if (result.baudotCode == LTRS_SHIFT) {
    currentShiftState_ = LTRS_SHIFT;
  } else if (result.baudotCode == FIGS_SHIFT) {
    currentShiftState_ = FIGS_SHIFT;
  }

  lastSentBaudot_ = result.baudotCode;
  return result;
}

} // namespace Baudot
