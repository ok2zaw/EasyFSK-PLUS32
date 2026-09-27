# EasyFSK-PLUS32

An ESP32 port of [EasyFSK-PLUS](https://github.com/ok2zaw/EasyFSK-PLUS) — an
RTTY/FSK keyer with PTT/PA relay sequencing, a status LCD, and USB-serial
control from N1MM+ and similar logging/contest software.

This is a **separate, standalone firmware**, not a fork or branch of the
original AVR/Nano project, which remains unchanged. It targets the
**Impero32** board (ESP32-WROOM-32U with an onboard LAN8720A Ethernet PHY)
and keeps the original's careful FSK/PTT bit-timing discipline while adding
wired LAN connectivity: a web UI for configuration, live monitoring, and
manual transmit.

## Status

Early first pass at the full architecture — build-verified, but not yet
tested on real hardware. Every module is a direct, deliberate port of the
corresponding AVR logic (see comments throughout the source referencing
`TinyFSK_ZAW_01.cpp`), restructured only where the ESP32 architecture
required it (see "What's different from the AVR original" below).

See [`docs/design-decisions.md`](docs/design-decisions.md) for the full,
running design log — including several **planned features not yet
implemented in this code** (RTTY tuning indicator + spectrum waterfall,
signal-quality meter, RX/TX audio level digipot trim via the rotary
encoder, a 9-LED tuning bargraph, an RTTY receive decoder, and CW keying
via Winkey protocol emulation). Some of these imply GPIO reassignments
(e.g. `ON_PIN` removed, `LED_RX_PIN` moving to an MCP23017 I2C expander,
GPIO2/GPIO14 repurposed for the encoder) that are **decided in the doc
but not yet applied to `Pins.h`/this codebase** — the GPIO table below
reflects the code as it stands today, not the doc's latest plan.

## Hardware

- Board: Impero32 (ESP32-WROOM-32U, no PSRAM, onboard LAN8720A RMII PHY).
  4MB flash is sufficient (see `partitions.csv`); 16MB-revision boards work
  unmodified.
- An external, opto-isolated USB-serial adapter is recommended for the
  N1MM/logger link, wired to the same UART0 pins the board's J1 header
  exposes (not the board's own onboard USB port).

### GPIO mapping

| Signal | GPIO | Notes |
|---|---|---|
| `FSK_PIN` | 5 | FSK keying output (mark/space) |
| `PTT_PIN` | 16 | Main relay PTT, safety-critical, defaults LOW |
| `PTT_PA_PIN` | 13 | PA/amp-stage PTT, safety-critical, defaults LOW |
| `CPU_INH_PIN` | 15 | Hardware inhibit input (active LOW, internal pull-up) |
| `LED_RX_PIN` | 2 | RX indicator LED |
| `ON_PIN` | 14 | Reserved, unused |
| `PTT_USB_RTS_PIN` | 39 | External hardware PTT-request input (active LOW) |
| I2C SDA / SCL (status LCD) | 32 / 33 | Moved off the ESP32 defaults — those pins are used by Ethernet |
| Ethernet | 19/21/22/25/26/27 (RMII data), 23/18 (MDC/MDIO), 17 (oscillator enable), 0 (REFCLK in) | Fixed by the ESP32 EMAC; PHY address is **1**, not the arduino-esp32 default of 0 |

See `include/Pins.h` for the full rationale (several of these were
deliberately chosen to avoid ESP32 boot-strapping pins; GPIO12 is left
completely unused on purpose).

## Building

Requires [PlatformIO](https://platformio.org/). Easiest path: open this
folder in **Visual Studio Code** with the **PlatformIO IDE** extension
installed (`.vscode/extensions.json` prompts VS Code to recommend it on
first open) — it will offer to install the extension, then download the
`espressif32` platform and all `lib_deps` automatically on first build.

From the PlatformIO sidebar (or the CLI, from the project root):

```sh
pio run                       # build firmware
pio run --target upload       # flash firmware
pio run --target uploadfs     # build + flash the web UI (data/) to LittleFS
```

Flash both the firmware and the filesystem image — the web UI lives in
`data/index.html` and is served from LittleFS, not compiled into the
firmware image.

The build environment is pinned in `platformio.ini`: PlatformIO Espressif32
7.1.3 with Arduino-ESP32 2.0.17 and exact library versions. The first verified
release build uses 45,856 bytes of RAM (14.0%) and 967,337 bytes of its
1,966,080-byte application slot (49.2%). Hardware testing is still required.

## Configuration

All settings (callsign, baud rate, FSK polarity, PTT/PA lead-tail timing,
network) are stored as JSON at `/config.json` on LittleFS, editable via:

- The web UI's **Configuration** tab (`http://<device-ip>/`), or
- The serial `~` configuration menu, same command letters as the AVR
  original (`0`/`1` polarity, `4`/`5`/`7` baud, `L`/`T`/`l`/`t` PTT/PA
  lead/tail, `C` callsign, `D`/`d` live LCD text, `?` show current config).

The web UI also has a **Backup / Restore** tab to download/upload
`config.json` directly.

## Web UI

Served over the wired LAN, no authentication (trusted-network assumption —
see the design doc if this needs revisiting for your setup):

- **Status** — live state (RX/TX/INHIBIT), PTT source, live transmitted
  text, and basic system info, pushed over a WebSocket.
- **Configuration** — all settings above, with inline hint text.
- **Send** — type text and Send / End / Abort, same semantics as the
  serial `[`/`]`/`\` commands. If idle, Send behaves like a self-contained
  `[text]`; if already transmitting, the text is appended to the buffer.
- **Backup / Restore** — `config.json` download/upload.

## What's different from the AVR original

The FSK/PTT timing *behavior* is preserved (same Baudot engine, same
PTT/PA lead-tail sequencing, same shift-state/USOS handling), but the
**architecture** changed where the ESP32 platform required or allowed an
improvement:

- **FSK_PIN is toggled directly from a hardware timer ISR**, not from
  `loop()`/task context — removes any dependency on scheduling latency,
  which matters once LAN/web traffic is competing for CPU. The ISR plays
  back Baudot symbols that a separate task (`TxManager`) computes and
  queues about one character-period ahead of time — at RTTY speeds
  (93–154ms/char) that's enormous slack for a ~2-4ms LCD write, so the
  live status LCD text is genuinely live with zero timing cost.
- **PTT/PA lead-tail sequencing is a non-blocking state machine**
  (checked every ~2ms via `millis()`), not blocking `delay()`. This is a
  deliberate improvement: the AVR original ignores abort/end commands
  while a lead delay is in progress; this version keeps servicing them
  throughout, so an abort takes effect immediately even mid-lead-delay.
- **Config storage is LittleFS + JSON** (`/config.json`), not raw EEPROM
  byte addressing — this is also what the web UI's config and
  backup/restore endpoints read and write directly.
- **A `TxManager` task arbitrates three TX sources** (serial, the
  hardware RTS-style input, and the web Send button) through one command
  queue — the AVR original only ever had one (serial), so this
  concurrency didn't exist there.
- **Flash writes are refused while a transmission is active** (`ConfigStore::applyAndSave`),
  extending the AVR's existing "no I2C on the bit-timing critical path"
  discipline to flash, since an ESP32 flash write briefly disables
  interrupts on both cores.

## Relationship to EasyFSK-PLUS

The original AVR/Nano firmware at
[ok2zaw/EasyFSK-PLUS](https://github.com/ok2zaw/EasyFSK-PLUS) remains the
reference implementation and is unaffected by this project. Anything here
that isn't explicitly called out above is intended to behave identically.
