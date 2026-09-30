#pragma once

#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <sstream>
#include <string>

#define F(value) value

// Host-test substitute for the USB serial API used by SerialControl.
class SerialMock {
public:
  int available() const { return static_cast<int>(rx.size()); }

  int read() {
    if (rx.empty()) return -1;
    const uint8_t value = rx.front();
    rx.pop_front();
    return value;
  }

  size_t write(uint8_t value) {
    tx.push_back(static_cast<char>(value));
    return 1;
  }

  size_t print(const char *value) {
    if (value != nullptr) tx += value;
    return value == nullptr ? 0 : std::char_traits<char>::length(value);
  }

  template <typename T>
  size_t print(const T &value) {
    std::ostringstream stream;
    stream << value;
    const std::string rendered = stream.str();
    tx += rendered;
    return rendered.size();
  }

  template <typename T>
  size_t println(const T &value) {
    const size_t written = print(value);
    tx.push_back('\n');
    return written + 1;
  }

  void pushRx(const std::string &value) {
    for (const unsigned char byte : value) rx.push_back(byte);
  }

  void pushRx(uint8_t value) { rx.push_back(value); }

  void reset() {
    rx.clear();
    tx.clear();
  }

  std::deque<uint8_t> rx;
  std::string tx;
};

extern SerialMock Serial;
