#pragma once

#include <stddef.h>
#include <stdint.h>

// Minimal host-test substitute for the one Arduino API used by Baudot.cpp.
// Production builds continue to include the real framework header.
class SerialMock {
public:
  size_t write(uint8_t value) {
    lastByte = value;
    writeCount++;
    return 1;
  }

  void reset() {
    lastByte = 0;
    writeCount = 0;
  }

  uint8_t lastByte = 0;
  size_t writeCount = 0;
};

extern SerialMock Serial;
