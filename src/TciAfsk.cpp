#include "TciAfsk.h"

#include "AfskModulator.h"

#include <Arduino.h>
#include <IPAddress.h>
#include <WebSocketsClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cmath>
#include <cstring>

namespace TciAfsk {
namespace {

constexpr size_t TCI_HEADER_SIZE = 64;
constexpr uint32_t TCI_TX_AUDIO_STREAM = 2;
constexpr uint32_t TCI_TX_CHRONO = 3;
constexpr uint32_t MAX_TCI_SAMPLES = 2048;
constexpr size_t MAX_AUDIO_FRAME_SIZE =
    TCI_HEADER_SIZE + MAX_TCI_SAMPLES * 2 * 4;

enum class ControlEvent : uint8_t { TxOn, StartAudio, FlushAudio };

Config s_cfg;
portMUX_TYPE s_cfgMux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t s_controlQueue = nullptr;
WebSocketsClient s_webSocket;
uint8_t s_audioFrame[MAX_AUDIO_FRAME_SIZE];
volatile bool s_connected = false;
volatile bool s_txSession = false;
volatile bool s_forceTxOff = false;
volatile uint32_t s_configRevision = 0;
uint32_t s_appliedRevision = 0;

uint32_t readLe32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

void writeLe32(uint8_t *data, uint32_t value) {
  data[0] = static_cast<uint8_t>(value);
  data[1] = static_cast<uint8_t>(value >> 8);
  data[2] = static_cast<uint8_t>(value >> 16);
  data[3] = static_cast<uint8_t>(value >> 24);
}

Config getConfig() {
  Config cfg;
  portENTER_CRITICAL(&s_cfgMux);
  cfg = s_cfg;
  portEXIT_CRITICAL(&s_cfgMux);
  return cfg;
}

void sendCommand(const char *command) {
  if (!s_webSocket.sendTXT(command)) {
    Serial.println(F("TCI AFSK: failed to enqueue text command"));
  }
}

void sendAudioConfiguration(const Config &cfg) {
  static const char *const formats[] = {"int16", "int24", "int32", "float32"};
  char command[80];
  snprintf(command, sizeof(command), "audio_stream_sample_type:%s;",
           formats[cfg.tci.sampleFormat]);
  sendCommand(command);
  snprintf(command, sizeof(command), "audio_stream_channels:%u;",
           static_cast<unsigned>(cfg.tci.channels));
  sendCommand(command);
  snprintf(command, sizeof(command), "audio_stream_samples:%u;",
           static_cast<unsigned>(cfg.tci.samples));
  sendCommand(command);
  snprintf(command, sizeof(command), "audio_samplerate:%lu;",
           static_cast<unsigned long>(cfg.tci.sampleRate));
  sendCommand(command);
  snprintf(command, sizeof(command), "tx_stream_audio_buffering:%u;",
           static_cast<unsigned>(cfg.tci.bufferTimeoutMs));
  sendCommand(command);
  snprintf(command, sizeof(command), "audio_start:%u;",
           static_cast<unsigned>(cfg.tci.receiver));
  sendCommand(command);
}

void sendTxCommand(bool active) {
  Config cfg = getConfig();
  char command[48];
  if (active) {
    snprintf(command, sizeof(command), "TRX:%u,true,tci;",
             static_cast<unsigned>(cfg.tci.receiver));
  } else {
    snprintf(command, sizeof(command), "TRX:%u,false;",
             static_cast<unsigned>(cfg.tci.receiver));
  }
  sendCommand(command);
}

uint32_t bytesPerSample(uint8_t format) {
  switch (format) {
    case 0: return 2;
    case 1: return 3;
    case 2:
    case 3: return 4;
    default: return 0;
  }
}

void writeSample(uint8_t *out, int16_t sample, uint8_t format) {
  const int32_t scaled = sample;
  switch (format) {
    case 0: {
      uint16_t value = static_cast<uint16_t>(sample);
      out[0] = static_cast<uint8_t>(value);
      out[1] = static_cast<uint8_t>(value >> 8);
      break;
    }
    case 1: {
      uint32_t value = static_cast<uint32_t>(scaled * 256);
      out[0] = static_cast<uint8_t>(value);
      out[1] = static_cast<uint8_t>(value >> 8);
      out[2] = static_cast<uint8_t>(value >> 16);
      break;
    }
    case 2: {
      uint32_t value = static_cast<uint32_t>(scaled * 65536);
      out[0] = static_cast<uint8_t>(value);
      out[1] = static_cast<uint8_t>(value >> 8);
      out[2] = static_cast<uint8_t>(value >> 16);
      out[3] = static_cast<uint8_t>(value >> 24);
      break;
    }
    case 3: {
      float value = static_cast<float>(sample) / 32768.0f;
      memcpy(out, &value, sizeof(value));
      break;
    }
  }
}

void sendChronoResponse(const uint8_t *request, size_t requestLength) {
  if (requestLength < TCI_HEADER_SIZE || readLe32(request + 24) != TCI_TX_CHRONO) {
    return;
  }

  Config cfg = getConfig();
  if (readLe32(request) != cfg.tci.receiver) return;
  uint32_t count = readLe32(request + 20);
  uint32_t bytesPer = bytesPerSample(cfg.tci.sampleFormat);
  if (count == 0 || count > MAX_TCI_SAMPLES || bytesPer == 0) {
    Serial.println(F("TCI AFSK: invalid TX_CHRONO frame"));
    return;
  }

  memset(s_audioFrame, 0, TCI_HEADER_SIZE);
  writeLe32(s_audioFrame, cfg.tci.receiver);
  writeLe32(s_audioFrame + 4, cfg.tci.sampleRate);
  writeLe32(s_audioFrame + 8, cfg.tci.sampleFormat);
  writeLe32(s_audioFrame + 20, count);
  writeLe32(s_audioFrame + 24, TCI_TX_AUDIO_STREAM);
  writeLe32(s_audioFrame + 28, cfg.tci.channels);

  uint8_t *audio = s_audioFrame + TCI_HEADER_SIZE;
  uint32_t underrunsBefore = AfskModulator::underrunCount();
  for (uint32_t i = 0; i < count; ++i) {
    int16_t sample = AfskModulator::nextSample();
    for (uint8_t channel = 0; channel < cfg.tci.channels; ++channel) {
      writeSample(audio, sample, cfg.tci.sampleFormat);
      audio += bytesPer;
    }
  }

  size_t frameLength = TCI_HEADER_SIZE +
                       static_cast<size_t>(count) * cfg.tci.channels * bytesPer;
  if (!s_webSocket.sendBIN(s_audioFrame, frameLength)) {
    Serial.println(F("TCI AFSK: failed to enqueue TX_AUDIO_STREAM"));
  }
  if (AfskModulator::underrunCount() != underrunsBefore) {
    Serial.println(F("TCI AFSK: audio symbol queue underrun; sent idle LTRS"));
  }
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      s_connected = true;
      Config cfg = getConfig();
      sendAudioConfiguration(cfg);
      if (s_txSession) sendTxCommand(true);
      break;
    }
    case WStype_DISCONNECTED:
      if (s_txSession) {
        Serial.println(F("TCI AFSK: server disconnected during TX"));
      }
      s_connected = false;
      s_txSession = false;
      AfskModulator::setActive(false);
      break;
    case WStype_BIN:
      sendChronoResponse(payload, length);
      break;
    case WStype_ERROR:
      Serial.println(F("TCI AFSK: WebSocket error"));
      break;
    default:
      break;
  }
}

void resetConnection(const Config &cfg) {
  if (s_connected && s_txSession) sendTxCommand(false);
  s_txSession = false;
  s_connected = false;
  AfskModulator::setActive(false);
  s_webSocket.disconnect();
  if (!cfg.tci.enabled) return;

  IPAddress address;
  if (!address.fromString(cfg.tci.serverIp)) {
    Serial.println(F("TCI AFSK: invalid server IPv4 address"));
    return;
  }
  s_webSocket.begin(address, cfg.tci.port, "/");
  s_webSocket.setReconnectInterval(5000);
}

void taskFn(void *) {
  s_webSocket.onEvent(webSocketEvent);
  s_webSocket.enableHeartbeat(15000, 3000, 2);

  Config cfg = getConfig();
  AfskModulator::configure(cfg.tci.sampleRate, cfg.baudRate, cfg.tci.markHz,
                           cfg.tci.spaceHz);
  resetConnection(cfg);
  s_appliedRevision = s_configRevision;

  for (;;) {
    uint32_t revision = s_configRevision;
    if (revision != s_appliedRevision) {
      cfg = getConfig();
      AfskModulator::configure(cfg.tci.sampleRate, cfg.baudRate, cfg.tci.markHz,
                               cfg.tci.spaceHz);
      resetConnection(cfg);
      s_appliedRevision = revision;
    }

    ControlEvent event;
    while (s_controlQueue &&
           xQueueReceive(s_controlQueue, &event, 0) == pdTRUE) {
      switch (event) {
        case ControlEvent::TxOn:
          if (s_connected && s_txSession) {
            AfskModulator::reset();
            sendTxCommand(true);
          }
          break;
        case ControlEvent::StartAudio:
          if (s_txSession) AfskModulator::setActive(true);
          break;
        case ControlEvent::FlushAudio:
          if (s_txSession) {
            AfskModulator::reset();
            AfskModulator::setActive(true);
          }
          break;
      }
    }
    if (s_forceTxOff) {
      s_forceTxOff = false;
      AfskModulator::setActive(false);
      if (s_connected) sendTxCommand(false);
    }

    if (cfg.tci.enabled) s_webSocket.loop();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

} // namespace

void begin(const Config &cfg) {
  portENTER_CRITICAL(&s_cfgMux);
  s_cfg = cfg;
  portEXIT_CRITICAL(&s_cfgMux);

  s_controlQueue = xQueueCreate(8, sizeof(ControlEvent));
  if (!s_controlQueue) {
    Serial.println(F("TCI AFSK: control queue allocation failed; TCI disabled"));
    return;
  }

  BaseType_t result = xTaskCreatePinnedToCore(
      taskFn, "TciAfsk", 8192, nullptr, 2, nullptr, 0);
  if (result != pdPASS) {
    Serial.println(F("TCI AFSK: task creation failed; TCI disabled"));
    vQueueDelete(s_controlQueue);
    s_controlQueue = nullptr;
  }
}

void applyConfig(const Config &cfg) {
  portENTER_CRITICAL(&s_cfgMux);
  s_cfg = cfg;
  ++s_configRevision;
  portEXIT_CRITICAL(&s_cfgMux);
}

bool isEnabled() {
  Config cfg = getConfig();
  return cfg.tci.enabled;
}

bool setTxActive(bool active) {
  if (!isEnabled()) return true;
  if (!s_controlQueue) return !active;
  if (active && !s_connected) return false;

  s_txSession = active;
  if (!active) {
    s_forceTxOff = true;
    return true;
  }
  s_forceTxOff = false;
  ControlEvent event = ControlEvent::TxOn;
  if (s_controlQueue && xQueueSend(s_controlQueue, &event, 0) == pdTRUE) {
    return true;
  }
  if (active) s_txSession = false;
  Serial.println(F("TCI AFSK: could not queue TX state change"));
  return false;
}

void startModulation() {
  if (!s_txSession) return;
  ControlEvent event = ControlEvent::StartAudio;
  if (!s_controlQueue || xQueueSend(s_controlQueue, &event, 0) != pdTRUE) {
    Serial.println(F("TCI AFSK: could not queue audio start"));
  }
}

void flushModulation() {
  if (!s_txSession) return;
  ControlEvent event = ControlEvent::FlushAudio;
  if (!s_controlQueue || xQueueSend(s_controlQueue, &event, 0) != pdTRUE) {
    Serial.println(F("TCI AFSK: could not queue audio flush"));
  }
}

bool pushSymbol(const Baudot::QueuedSymbol &symbol) {
  if (!s_txSession) return true;
  return AfskModulator::pushSymbol(symbol);
}

} // namespace TciAfsk
