#pragma once

#include "Baudot.h"
#include "Config.h"

namespace TciAfsk {

void begin(const Config &cfg);
void applyConfig(const Config &cfg);
bool isEnabled();
bool setTxActive(bool active);
void startModulation();
void flushModulation();
bool pushSymbol(const Baudot::QueuedSymbol &symbol);

} // namespace TciAfsk
