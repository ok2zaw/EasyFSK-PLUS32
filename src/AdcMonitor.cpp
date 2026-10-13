#include "AdcMonitor.h"

#include "Pins.h"

#include <Arduino.h>
#include <driver/adc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

namespace AdcMonitor {

namespace {

constexpr uint32_t kSampleRateHz = 20000;
constexpr uint32_t kReadBufferBytes = 512;
constexpr uint32_t kAdc1ChannelMask = 1U << ADC1_CHANNEL_6;
constexpr uint8_t kAudioChannel = ADC1_CHANNEL_6; // GPIO34

portMUX_TYPE s_snapshotMux = portMUX_INITIALIZER_UNLOCKED;
Snapshot s_snapshot;

void publish(const AdcLevel::Reading &reading) {
  portENTER_CRITICAL(&s_snapshotMux);
  if (s_snapshot.valid) {
    s_snapshot.levelPct = static_cast<uint8_t>(
        (3U * s_snapshot.levelPct + reading.levelPct + 2U) / 4U);
  } else {
    s_snapshot.levelPct = reading.levelPct;
  }
  s_snapshot.clipped = s_snapshot.clipped || reading.clipped;
  s_snapshot.valid = reading.valid;
  s_snapshot.updatedAtMs = millis();
  portEXIT_CRITICAL(&s_snapshotMux);
}

void samplingTask(void *) {
  uint8_t buffer[kReadBufferBytes];
  AdcLevel::WindowAnalyzer analyzer;

  for (;;) {
    uint32_t bytesRead = 0;
    esp_err_t result = adc_digi_read_bytes(buffer, sizeof(buffer), &bytesRead, 100);
    if (result == ESP_ERR_TIMEOUT) continue;
    if (result != ESP_OK) {
      Serial.printf("ADC monitor: DMA read failed: %s\n", esp_err_to_name(result));
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    for (uint32_t offset = 0; offset + sizeof(adc_digi_output_data_t) <= bytesRead;
         offset += sizeof(adc_digi_output_data_t)) {
      adc_digi_output_data_t sample;
      memcpy(&sample, buffer + offset, sizeof(sample));
      if (sample.type1.channel != kAudioChannel) continue;

      AdcLevel::Reading reading;
      if (analyzer.addSample(sample.type1.data, reading)) publish(reading);
    }
  }
}

bool configureDma() {
  adc_digi_init_config_t initConfig = {};
  initConfig.max_store_buf_size = 2048;
  initConfig.conv_num_each_intr = 256;
  initConfig.adc1_chan_mask = kAdc1ChannelMask;
  initConfig.adc2_chan_mask = 0;
  esp_err_t result = adc_digi_initialize(&initConfig);
  if (result != ESP_OK) {
    Serial.printf("ADC monitor: DMA initialization failed: %s\n", esp_err_to_name(result));
    return false;
  }

  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = kAudioChannel;
  pattern.unit = ADC_UNIT_1;
  pattern.bit_width = ADC_WIDTH_BIT_12;

  adc_digi_configuration_t config = {};
  config.conv_limit_en = false;
  config.conv_limit_num = 0;
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  config.sample_freq_hz = kSampleRateHz;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
  result = adc_digi_controller_config(&config);
  if (result != ESP_OK) {
    Serial.printf("ADC monitor: DMA configuration failed: %s\n", esp_err_to_name(result));
    return false;
  }

  result = adc_digi_start();
  if (result != ESP_OK) {
    Serial.printf("ADC monitor: DMA start failed: %s\n", esp_err_to_name(result));
    return false;
  }
  return true;
}

} // namespace

bool begin() {
  if (!configureDma()) return false;
  if (xTaskCreatePinnedToCore(samplingTask, "AdcMonitor", 3072, nullptr, 2, nullptr, 0) != pdPASS) {
    adc_digi_stop();
    Serial.println(F("ADC monitor: could not create sampling task"));
    return false;
  }
  Serial.println(F("ADC monitor: sampling GPIO34 at 20 kHz"));
  return true;
}

Snapshot getSnapshot() {
  Snapshot snapshot;
  portENTER_CRITICAL(&s_snapshotMux);
  snapshot = s_snapshot;
  s_snapshot.clipped = false;
  portEXIT_CRITICAL(&s_snapshotMux);
  return snapshot;
}

} // namespace AdcMonitor
