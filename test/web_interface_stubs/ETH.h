#pragma once

#include <Arduino.h>

class IPAddressMock {
public:
  String toString() const { return String(value); }
  std::string value = "0.0.0.0";
};

class ETHClass {
public:
  String macAddress() const { return String(mac); }
  IPAddressMock localIP() const { return ip; }
  bool linkUp() const { return linked; }

  std::string mac = "00:00:00:00:00:00";
  IPAddressMock ip;
  bool linked = false;
};

extern ETHClass ETH;
