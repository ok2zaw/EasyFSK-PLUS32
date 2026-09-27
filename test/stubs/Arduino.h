#pragma once

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <vector>

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

// Host-test substitute for the subset of HardwareSerial used by
// WinkeyEmulator. Tests can queue receive bytes and inspect transmitted
// replies without exposing test-only hooks in production code.
class HardwareSerialMock {
public:
  void begin(unsigned long baudValue, uint32_t configValue, int8_t rxPinValue,
             int8_t txPinValue) {
    baud = baudValue;
    config = configValue;
    rxPin = rxPinValue;
    txPin = txPinValue;
    beginCount++;
  }

  void end() { endCount++; }
  int available() const { return static_cast<int>(rx.size()); }

  int read() {
    if (rx.empty()) return -1;
    uint8_t value = rx.front();
    rx.pop_front();
    return value;
  }

  size_t write(uint8_t value) {
    tx.push_back(value);
    return 1;
  }

  void pushRx(uint8_t value) { rx.push_back(value); }

  void reset() {
    rx.clear();
    tx.clear();
    baud = 0;
    config = 0;
    rxPin = -1;
    txPin = -1;
    beginCount = 0;
    endCount = 0;
  }

  std::deque<uint8_t> rx;
  std::vector<uint8_t> tx;
  unsigned long baud = 0;
  uint32_t config = 0;
  int8_t rxPin = -1;
  int8_t txPin = -1;
  size_t beginCount = 0;
  size_t endCount = 0;
};

constexpr uint32_t SERIAL_8N1 = 0x800001cu;
constexpr uint32_t SERIAL_8N2 = 0x800003cu;

extern HardwareSerialMock Serial2;
