#pragma once

#include "Config.h"

// ---------------------------------------------------------------------------
// Owns UART2 (GPIO15 TX2/GPIO36 RX2, see Pins.h) end-to-end: both of its
// mode-switched roles (design doc's "UART1/UART2 split", 2026-09-27) live
// here --
//   - `Uart2Mode::Fsk2`: a second, independent FSK/RTTY control input, same
//     9600/8-N-1 framing and TX_ON/TX_END/TX_ABORT convention as UART1 --
//     forwarded into the SAME TxManager queue UART1 uses, tagged
//     Source::Uart2Fsk so the LCD/status can tell them apart (FSK1 vs FSK2).
//   - `Uart2Mode::Cw`: full Winkey host-protocol emulation at Winkey's
//     fixed 1200/8-N-2 framing, feeding TxManager's CW/Morse engine
//     (Source::Winkey). See the design doc's CW/Winkey section for the
//     research this is built from, and Morse.h/CwTimer.h for the actual
//     keying engine this protocol layer drives.
//
// Byte-at-a-time, called from loop() exactly like SerialControl::poll() --
// not its own FreeRTOS task. Winkey commands are multi-byte binary, but
// nothing here is timing-critical (the actual CW keying timing lives in
// CwTimer's hardware-timer ISR, not in this parser), so the same
// "don't bog down the processor" one-byte-per-pass style already used for
// UART1 fits fine.
// ---------------------------------------------------------------------------

namespace WinkeyEmulator {

// Call once from setup(), after TxManager::begin(). Configures UART2 for
// whichever mode `cfg.uart2Mode` currently is.
void begin(const Config &cfg);

// Call whenever the live config changes (from ConfigStore::applyAndSave(),
// alongside its other apply-time actions). Safe from any task: it only
// records the requested mode. The next poll() -- in loop(), the task that
// owns UART2 -- then reconfigures UART2's baud/framing and resets this
// module's parser state if uart2Mode actually changed, and does nothing if
// it didn't (baud doesn't need to be reset every unrelated config save).
void applyConfig(const Config &cfg);

// Call once per Arduino loop() iteration, same contract as SerialControl::poll().
void poll();

} // namespace WinkeyEmulator
