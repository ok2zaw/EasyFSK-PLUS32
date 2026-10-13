#include "AfskModulator.h"

#include <cmath>

namespace AfskModulator {
namespace {

constexpr uint8_t START_BIT_POS = 0;
constexpr uint8_t DATA_BIT_COUNT = 5;
constexpr uint8_t STOP_BIT_POS = DATA_BIT_COUNT + 1;
constexpr uint8_t HALF_STOP_BIT_POS = STOP_BIT_POS + 1;
constexpr int16_t AMPLITUDE = 24575;

Baudot::QueuedSymbol s_symbols[SYMBOL_QUEUE_CAPACITY];
volatile size_t s_head = 0;
volatile size_t s_tail = 0;
volatile bool s_active = false;

uint32_t s_sampleRate = 48000;
uint32_t s_baudMilli = 45450;
uint16_t s_markHz = 2125;
uint16_t s_spaceHz = 2295;
uint32_t s_markPhaseIncrement = 0;
uint32_t s_spacePhaseIncrement = 0;
uint64_t s_bitThreshold = 48000000;
uint64_t s_bitPhase = 0;
uint32_t s_audioPhase = 0;
uint8_t s_bitPosition = 0;
uint8_t s_currentCode = Baudot::LTRS_SHIFT;
bool s_frameActive = false;
bool s_finished = false;
uint32_t s_underruns = 0;

bool queueEmpty() { return s_head == s_tail; }
bool queueFull() { return (s_tail + 1) % SYMBOL_QUEUE_CAPACITY == s_head; }

bool popSymbol(Baudot::QueuedSymbol &symbol) {
  if (queueEmpty()) return false;
  symbol = s_symbols[s_head];
  s_head = (s_head + 1) % SYMBOL_QUEUE_CAPACITY;
  return true;
}

void startNextFrame() {
  Baudot::QueuedSymbol symbol;
  if (!popSymbol(symbol)) {
    symbol.baudotCode = Baudot::LTRS_SHIFT;
    symbol.asciiByte = 0;
    ++s_underruns;
  }
  if (symbol.baudotCode == Baudot::TX_END_FLAG) {
    s_finished = true;
    s_frameActive = false;
    return;
  }
  s_currentCode = symbol.baudotCode;
  s_bitPosition = START_BIT_POS;
  s_frameActive = true;
}

void advanceBit() {
  if (s_bitPosition < DATA_BIT_COUNT) {
    s_bitPosition++;
  } else if (s_bitPosition == DATA_BIT_COUNT) {
    s_bitPosition = STOP_BIT_POS;
  } else if (s_bitPosition == STOP_BIT_POS) {
    s_bitPosition = HALF_STOP_BIT_POS;
  } else {
    s_bitPosition = START_BIT_POS;
    s_frameActive = false;
  }
}

bool currentMark() {
  if (!s_frameActive) return true;
  if (s_bitPosition == START_BIT_POS) return false;
  if (s_bitPosition <= DATA_BIT_COUNT) {
    return (s_currentCode & (1u << (s_bitPosition - 1))) != 0;
  }
  return true;
}

} // namespace

void configure(uint32_t sampleRate, float baudRate, uint16_t markHz,
               uint16_t spaceHz) {
  s_sampleRate = sampleRate;
  s_baudMilli = static_cast<uint32_t>(baudRate * 1000.0f + 0.5f);
  s_markHz = markHz;
  s_spaceHz = spaceHz;
  s_markPhaseIncrement = sampleRate == 0
                             ? 0
                             : static_cast<uint32_t>(
                                   (static_cast<uint64_t>(markHz) << 32) /
                                   sampleRate);
  s_spacePhaseIncrement = sampleRate == 0
                              ? 0
                              : static_cast<uint32_t>(
                                    (static_cast<uint64_t>(spaceHz) << 32) /
                                    sampleRate);
  s_bitThreshold = static_cast<uint64_t>(sampleRate) * 1000u;
  reset();
}

void reset() {
  s_head = 0;
  s_tail = 0;
  s_active = false;
  s_bitPhase = 0;
  s_audioPhase = 0;
  s_bitPosition = START_BIT_POS;
  s_currentCode = Baudot::LTRS_SHIFT;
  s_frameActive = false;
  s_finished = false;
  s_underruns = 0;
}

bool pushSymbol(const Baudot::QueuedSymbol &symbol) {
  if (queueFull()) return false;
  s_symbols[s_tail] = symbol;
  s_tail = (s_tail + 1) % SYMBOL_QUEUE_CAPACITY;
  return true;
}

void setActive(bool active) {
  s_active = active;
  if (!active) {
    s_head = 0;
    s_tail = 0;
    s_frameActive = false;
    s_finished = false;
    s_bitPhase = 0;
  }
}

int16_t nextSample() {
  if (!s_active || s_sampleRate == 0 || s_baudMilli == 0) return 0;

  if (!s_frameActive && !s_finished) {
    startNextFrame();
  }

  bool mark = currentMark();
  float phase = static_cast<float>(s_audioPhase) *
                (6.2831853071795864769f / 4294967296.0f);
  int16_t sample = static_cast<int16_t>(sinf(phase) * AMPLITUDE);
  s_audioPhase += mark ? s_markPhaseIncrement : s_spacePhaseIncrement;

  if (s_frameActive) {
    uint64_t threshold = s_bitThreshold;
    uint64_t bitDuration = s_bitPosition == HALF_STOP_BIT_POS
                               ? threshold / 2
                               : threshold;
    s_bitPhase += s_baudMilli;
    while (s_bitPhase >= bitDuration) {
      s_bitPhase -= bitDuration;
      advanceBit();
      if (!s_frameActive) break;
      bitDuration = s_bitPosition == HALF_STOP_BIT_POS ? threshold / 2
                                                       : threshold;
    }
  }

  return sample;
}

uint32_t underrunCount() { return s_underruns; }

} // namespace AfskModulator
