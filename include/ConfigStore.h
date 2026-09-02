#pragma once

#include "Config.h"

// ---------------------------------------------------------------------------
// Small thread-safe holder for the single live Config instance, shared
// between SerialControl (the `~` config submenu) and WebInterface (the
// POST /api/config and /api/config/restore handlers), which run in
// different task contexts. Centralizing this avoids either module needing
// to know about the other, and is where the design doc's "flash writes
// gated off during active TX" rule is actually enforced -- applyAndSave()
// itself checks TxManager's status and refuses (rather than silently
// deferring) while a transmission is in progress.
// ---------------------------------------------------------------------------

namespace ConfigStore {

// Loads from LittleFS (or defaults, on first boot) into the internal copy.
// Call once from setup(), before TxManager::begin()/FskTimer::begin() (both
// need initial values from the loaded config).
void begin();

// Thread-safe copy of the current live config.
Config get();

// Validates `in` (see Config.h configValidate()), and if valid AND no
// transmission is currently active, applies it live (TxManager::applyConfig,
// FskTimer::reconfigure) and persists it to LittleFS. `errors` and `out`
// behave as in configValidate(); additionally, if validation succeeds but a
// TX is in progress, returns false and errors["_"] explains that nothing
// was saved because a transmission is active.
bool applyAndSave(JsonVariantConst in, JsonObject errors);

} // namespace ConfigStore
