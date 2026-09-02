#pragma once

// ---------------------------------------------------------------------------
// Serial-receive handling: thin translation layer between the N1MM/logger
// serial link and TxManager's command queue, plus the `~` configuration
// submenu -- ported from loop()'s serial-byte handling and
// handleConfigurationCommand()/handleNumericEntry()/handleCallsignEntry()
// in the AVR original.
//
// Reads at most one byte per poll() call (matches the AVR original's
// "don't bog down the processor" comment), and is not itself timing-
// sensitive -- all it does is translate bytes into TxManager::enqueueXxx()
// calls or ConfigStore::applyAndSave() calls.
// ---------------------------------------------------------------------------

namespace SerialControl {

void begin();

// Call once per Arduino loop() iteration.
void poll();

} // namespace SerialControl
