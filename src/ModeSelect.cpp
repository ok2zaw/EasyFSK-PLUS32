#include "ModeSelect.h"
#include <Arduino.h>

namespace ModeSelect {

namespace {
bool s_cwActive = false;
} // namespace

void begin() {
  s_cwActive = false;
  // TODO(MCP23017 driver): pinMode/write MODE_SEL_PIN LOW here once the
  // I2C GPIO-expander driver exists. No-op until then.
}

void setActive(bool cwActive) {
  s_cwActive = cwActive;
  // TODO(MCP23017 driver): drive MODE_SEL_PIN HIGH/LOW via the MCP23017
  // here. Tracked in s_cwActive in the meantime so callers have a single,
  // stable call site to switch to once the driver exists.
  (void)s_cwActive;
}

} // namespace ModeSelect
