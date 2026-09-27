#pragma once

// ---------------------------------------------------------------------------
// Hardware routing control line (`MODE_SEL_PIN` in the design doc): a
// static-level signal telling the board's downstream analog/interface
// circuitry which conditioning path FSK_PIN's signal should be routed
// through -- LOW while FSK_PIN carries RTTY mark/space shift keying (UART1
// always, or UART2 in "fsk2" mode), HIGH while it carries a plain CW
// key-down/key-up line (UART2 in "cw"/Winkey mode). It changes only when
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
