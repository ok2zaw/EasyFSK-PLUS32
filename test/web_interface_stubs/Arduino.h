#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

class String {
public:
  String() = default;
  String(const char *value) : value_(value == nullptr ? "" : value) {}
  String(const std::string &value) : value_(value) {}

  String &operator=(const char *value) {
    value_ = value == nullptr ? "" : value;
    return *this;
  }

  const char *c_str() const { return value_.c_str(); }
  size_t length() const { return value_.length(); }
  size_t size() const { return value_.size(); }
  const char *data() const { return value_.data(); }

  bool concat(const char *value) {
    if (value != nullptr) value_ += value;
    return true;
  }

  size_t write(uint8_t value) {
    value_.push_back(static_cast<char>(value));
    return 1;
  }

  size_t write(const uint8_t *data, size_t size) {
    value_.append(reinterpret_cast<const char *>(data), size);
    return size;
  }

private:
  std::string value_;
};

class EspMock {
public:
  uint32_t getFlashChipSize() const { return flashChipSize; }
  uint32_t getFreeHeap() const { return freeHeap; }

  uint32_t flashChipSize = 0;
  uint32_t freeHeap = 0;
};

extern EspMock ESP;

uint32_t millis();
