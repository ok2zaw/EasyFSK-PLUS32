#include "ConfigStore.h"
#include "TxManager.h"
#include "FskTimer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace ConfigStore {

namespace {
Config s_cfg;
SemaphoreHandle_t s_mutex = nullptr;
} // namespace

void begin() {
  s_mutex = xSemaphoreCreateMutex();
  configLoad(s_cfg);
}

Config get() {
  Config copy;
  xSemaphoreTake(s_mutex, portMAX_DELAY);
  copy = s_cfg;
  xSemaphoreGive(s_mutex);
  return copy;
}

bool applyAndSave(JsonVariantConst in, JsonObject errors) {
  xSemaphoreTake(s_mutex, portMAX_DELAY);

  Config candidate = s_cfg;
  bool valid = configValidate(in, candidate, errors);
  if (!valid) {
    xSemaphoreGive(s_mutex);
    return false;
  }

  if (TxManager::getStatus().txActive) {
    // Same discipline as the AVR original's I2C-off-critical-path rule,
    // extended to flash: a LittleFS write briefly disables interrupts on
    // both ESP32 cores, long enough to matter at 75 baud. Refuse rather
    // than risk glitching an in-progress transmission.
    errors["_"] = "a transmission is currently active; try again once it ends";
    xSemaphoreGive(s_mutex);
    return false;
  }

  s_cfg = candidate;
  TxManager::applyConfig(s_cfg);
  FskTimer::reconfigure(s_cfg.baudRate, s_cfg.markHigh);
  bool saved = configSave(s_cfg);

  xSemaphoreGive(s_mutex);
  return saved;
}

} // namespace ConfigStore
