#pragma once

// ---------------------------------------------------------------------------
// GPIO mapping for EasyFSK-PLUS32 on the Impero32 board (ESP32-WROOM-32U).
//
// This is the "near-final" mapping agreed in the design-decisions doc
// (claude/esp32-port-design-decisions.md in the EasyFSK_LCD_ESP project),
// carried over here verbatim. Rationale is repeated in comments below so
// this header is self-explanatory without the design doc in hand.
//
// Do NOT reassign these without re-checking the design doc's strapping-pin
// discussion -- several of these were chosen specifically to avoid ESP32
// boot-strapping pins (GPIO0, 2, 5, 12, 15), with GPIO12 (MTDI, flash
// voltage select) deliberately left completely unused.
// ---------------------------------------------------------------------------

// --- EasyFSK signal pins (J1 connector) -------------------------------------

// Main relay PTT output. Safety-critical: must default LOW at boot.
// J1 pin: OUT3. No strapping role at all.
#define PTT_PIN 16

// PA/amplifier-stage PTT output. Safety-critical: must default LOW at boot.
// J1 pin: MOSI. No strapping role.
#define PTT_PA_PIN 13

// FSK keying output (mark/space).
// J1 pin: OUT2. Technically a strapping pin (SDIO slave timing) but that
// role doesn't affect boot-mode selection, and its required strap state is
// satisfied by the pin's own internal pull-up -- low risk.
#define FSK_PIN 5

// Hardware inhibit input, active LOW (idle/not-inhibited = HIGH).
// Moved 2026-09-27 (see design doc's GPIO mapping section) from CS/GPIO15
// onto ADC2/GPIO35 to free GPIO15 for UART2 (below). GPIO35 is input-only
// with NO internal pull -- unlike the old GPIO15 home, this net now needs
// a small EXTERNAL pull-up resistor on the board (hardware/BOM change).
// Firmware must use plain INPUT here, not INPUT_PULLUP -- see TxManager.cpp.
#define CPU_INH_PIN 35

// RX indicator LED (cosmetic only -- opposite sense of PTT_PIN).
// J1 pin: OUT1. Board's own "avoid pulling low at reset" caution is
// harmless for an LED -- worst case is a boot-time flicker.
#define LED_RX_PIN 2

// Reserved/unused today, mirrors the AVR original's unused ON_PIN.
// J1 pin: CLK.
#define ON_PIN 14

// External hardware PTT-request input (RTS-style), active LOW, external
// pull-up expected (input-only pin, no internal pull available).
// J1 pin: ADC4_PTT -- net name literally says PTT.
#define PTT_USB_RTS_PIN 39

// Deliberately unused: J1 MISO / GPIO12 (MTDI, flash-voltage-select strap)
// -- the one genuinely risky pin on this board. Do not assign it.
// #define UNUSED_RESERVED_PIN 12

// --- UART2 (added 2026-09-27, freed by the CPU_INH_PIN move above) --------
//
// A second hardware UART (ESP32 has 3 real ones -- routed via the GPIO
// matrix, not a "virtual"/software UART). Mode-switched between a second,
// independent FSK/RTTY control input ("FSK2", same 9600/8-N-1 framing as
// UART1/Serial) and Winkey CW-keyer emulation (1200/8-N-2) -- see
// WinkeyEmulator.h and the design doc's "UART1/UART2 split" section.
// UART1 (the original UART0/J1 RXD-TXD link, GPIO1/GPIO3, plain `Serial`)
// is UNCHANGED and always speaks FSK/RTTY control regardless of this mode.
#define UART2_TX_PIN 15 // J1 CS -- freed by the CPU_INH_PIN move above
#define UART2_RX_PIN 36 // ADC1(SVN) -- the other previously-spare input-only pin

// --- Status LCD (I2C) -------------------------------------------------------

// Moved off the ESP32's usual default I2C pins because GPIO21/22 are used
// by the Ethernet RMII interface (TXEN/TXD1) -- see below.
#define LCD_SDA_PIN 32
#define LCD_SCL_PIN 33
#define LCD_I2C_ADDR 0x27 // matches the AVR original's LiquidCrystal_I2C address
#define LCD_COLS 16
#define LCD_ROWS 2

// --- Ethernet (onboard LAN8720A, RMII) --------------------------------------
//
// These are fixed by the ESP32's RMII MAC and are NOT reassignable -- listed
// here only for reference/completeness, ETH.begin() doesn't take them as
// arguments on this core.
//   TXD0=GPIO19  TXEN=GPIO21  TXD1=GPIO22
//   RXD0=GPIO25  RXD1=GPIO26  CRS_DV=GPIO27
//   MDC=GPIO23   MDIO=GPIO18

// GPIO17 gates the external 50MHz oscillator's output-enable pin on this
// board. Must be driven HIGH *before* calling ETH.begin() -- do NOT pass it
// as ETH.begin()'s own `power` pin argument, that causes reset conflicts on
// Impero32 specifically (confirmed by the board's own bring-up example).
#define ETH_OSC_EN_PIN 17

// NOTE: the rest of the Ethernet PHY configuration (PHY type/address, MDC/
// MDIO pins, clock mode) is NOT defined here as plain macros, even though
// conceptually it belongs in this file. arduino-esp32's <ETH.h> reads
// ETH_PHY_TYPE/ETH_PHY_ADDR/ETH_PHY_MDC/ETH_PHY_MDIO/ETH_PHY_POWER/
// ETH_CLK_MODE as preprocessor macros that must already be defined (with
// their *enum* values, e.g. ETH_PHY_LAN8720/ETH_CLOCK_GPIO0_IN, which only
// exist once <ETH.h>'s own headers are visible) at the point <ETH.h> is
// first included in a translation unit -- so they have to live directly in
// main.cpp, immediately before its `#include <ETH.h>`, not in a header
// that might be included in a different order elsewhere. See main.cpp for
// the actual values (PHY address 1, MDC=GPIO23, MDIO=GPIO18,
// ETH_CLOCK_GPIO0_IN) -- they match this board's wiring exactly, this
// comment just explains why they aren't here too.
