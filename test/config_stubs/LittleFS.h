#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

class File {
public:
  File() = default;
  File(std::string *storage, bool readable, bool writable)
      : storage_(storage), readable_(readable), writable_(writable) {
    if (writable_ && storage_) storage_->clear();
  }

  explicit operator bool() const { return storage_ != nullptr; }

  int read() {
    if (!readable_ || !storage_ || position_ >= storage_->size()) return -1;
    return static_cast<uint8_t>((*storage_)[position_++]);
  }

  size_t readBytes(char *buffer, size_t length) {
    if (!readable_ || !storage_) return 0;
    size_t available = storage_->size() - position_;
    size_t count = std::min(length, available);
    memcpy(buffer, storage_->data() + position_, count);
    position_ += count;
    return count;
  }

  size_t write(uint8_t value) {
    if (!writable_ || !storage_) return 0;
    storage_->push_back(static_cast<char>(value));
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t length) {
    if (!writable_ || !storage_) return 0;
    storage_->append(reinterpret_cast<const char *>(buffer), length);
    return length;
  }

  void close() {}

private:
  std::string *storage_ = nullptr;
  size_t position_ = 0;
  bool readable_ = false;
  bool writable_ = false;
};

class LittleFSMock {
public:
  bool exists(const char *) const { return fileExists; }

  File open(const char *, const char *mode) {
    if (mode && mode[0] == 'r') {
      return fileExists ? File(&contents, true, false) : File();
    }
    if (mode && mode[0] == 'w') {
      if (!allowWrite) return File();
      fileExists = true;
      return File(&contents, false, true);
    }
    return File();
  }

  void reset() {
    fileExists = false;
    allowWrite = true;
    contents.clear();
  }

  bool fileExists = false;
  bool allowWrite = true;
  std::string contents;
};

extern LittleFSMock LittleFS;
