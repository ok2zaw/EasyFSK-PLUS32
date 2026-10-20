#pragma once

// ---------------------------------------------------------------------------
// Hardware routing control line (`MODE_SEL_PIN` in the design doc): a
// static-level signal telling downstream interface circuitry which operating
// mode is active -- LOW for RTTY (FSK_PIN), HIGH for CW (CW_KEY_PIN). The
// keying signals now have separate GPIOs; this is a mode indicator/routing
// control, not a selector for a shared keying pin. It changes only when
// `uart2Mode` changes (a config-apply-time action, not a per-keystroke
// signal) -- see ConfigStore::applyAndSave().
//
// STUB, 2026-09-27: MODE_SEL_PIN lives on the MCP23017 I2C GPIO expander
// (design doc: "takes the last spare MCP23017 pin"), and no MCP23017 driver
// exists in this codebase yet (the LED bargraph / encoder-select LEDs /
// pushbutton that also live on it are all still design-only, per the
// README's "decided but not yet applied" list). This module exists so
// WinkeyEmulator/ConfigStore have a real call site to wire up now, ready to
// fill in once the MCP23017 driver lands -- today it only tracks the
// intended state, no physical pin is driven.
// ---------------------------------------------------------------------------

namespace ModeSelect {

void begin();

// true = CW/Winkey routing (HIGH), false = RTTY/FSK2 routing (LOW).
void setActive(bool cwActive);

} // namespace ModeSelect
