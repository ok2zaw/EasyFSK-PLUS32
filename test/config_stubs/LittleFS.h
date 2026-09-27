#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

class File {
public:
  File() = default;
  File(std::string *storage, bool readable, bool writable, size_t *writeLimit)
      : storage_(storage), readable_(readable), writable_(writable),
        writeLimit_(writeLimit) {
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
    if (writeLimit_ && *writeLimit_ == 0) return 0;
    storage_->push_back(static_cast<char>(value));
    if (writeLimit_ && *writeLimit_ != std::numeric_limits<size_t>::max()) {
      (*writeLimit_)--;
    }
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t length) {
    if (!writable_ || !storage_) return 0;
    size_t count = length;
    if (writeLimit_ && *writeLimit_ != std::numeric_limits<size_t>::max()) {
      count = std::min(count, *writeLimit_);
      *writeLimit_ -= count;
    }
    storage_->append(reinterpret_cast<const char *>(buffer), count);
    return count;
  }

  void close() {}

private:
  std::string *storage_ = nullptr;
  size_t position_ = 0;
  bool readable_ = false;
  bool writable_ = false;
  size_t *writeLimit_ = nullptr;
};

class LittleFSMock {
public:
  bool exists(const char *path) const {
    if (strcmp(path, "/config.json") == 0) return fileExists;
    if (strcmp(path, "/config.tmp") == 0) return tempExists;
    return false;
  }

  File open(const char *path, const char *mode) {
    bool isPrimary = strcmp(path, "/config.json") == 0;
    bool isTemp = strcmp(path, "/config.tmp") == 0;
    if (!isPrimary && !isTemp) return File();
    bool &existsFlag = isPrimary ? fileExists : tempExists;
    std::string &storage = isPrimary ? contents : tempContents;
    if (mode && mode[0] == 'r') {
      return existsFlag ? File(&storage, true, false, nullptr) : File();
    }
    if (mode && mode[0] == 'w') {
      if (!allowWrite) return File();
      existsFlag = true;
      return File(&storage, false, true, &writeLimit);
    }
    return File();
  }

  bool remove(const char *path) {
    if (strcmp(path, "/config.json") == 0) {
      fileExists = false;
      contents.clear();
      return true;
    }
    if (strcmp(path, "/config.tmp") == 0) {
      tempExists = false;
      tempContents.clear();
      return true;
    }
    return false;
  }

  bool rename(const char *from, const char *to) {
    if (!allowRename || strcmp(from, "/config.tmp") != 0 ||
        strcmp(to, "/config.json") != 0 || !tempExists) {
      return false;
    }
    contents = tempContents;
    fileExists = true;
    tempContents.clear();
    tempExists = false;
    return true;
  }

  void reset() {
    fileExists = false;
    tempExists = false;
    allowWrite = true;
    allowRename = true;
    writeLimit = std::numeric_limits<size_t>::max();
    contents.clear();
    tempContents.clear();
  }

  bool fileExists = false;
  bool tempExists = false;
  bool allowWrite = true;
  bool allowRename = true;
  size_t writeLimit = std::numeric_limits<size_t>::max();
  std::string contents;
  std::string tempContents;
};

extern LittleFSMock LittleFS;
