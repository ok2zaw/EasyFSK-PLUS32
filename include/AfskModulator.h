#pragma once

#include <cstddef>
#include <cstdint>

#include "Baudot.h"

namespace AfskModulator {

constexpr size_t SYMBOL_QUEUE_CAPACITY = 64;

void configure(uint32_t sampleRate, float baudRate, uint16_t markHz,
               uint16_t spaceHz);
void reset();
bool pushSymbol(const Baudot::QueuedSymbol &symbol);
void setActive(bool active);
int16_t nextSample();
uint32_t underrunCount();

} // namespace AfskModulator
