# EasyFSK-PLUS → ESP32 (Impero32) port — running design decisions

Source firmware: https://github.com/ok2zaw/EasyFSK-PLUS (`src/TinyFSK_ZAW_01.cpp`,
Arduino Nano/ATmega328). Goal: port to ESP32 keeping the same (or better)
FSK/PTT bit-timing discipline, and add wired LAN.

## Repo / project setup

- **EasyFSK-PLUS (AVR/Nano) stays as-is, untouched** — not merged with the
  ESP32 work. **The separate ESP32/Impero32 repository is
  https://github.com/ok2zaw/EasyFSK-PLUS32.** It is now an initialized,
  actively developed project with a PlatformIO build, host-side test
  suites, and CI; it is not an empty repository awaiting its first commit.
  This design doc describes that repository's architecture, not a branch
  of the AVR project.

## Hardware: confirmed

- Board: **Impero32** (KiCad project `impero_ver.2.0`), MCU **ESP32-WROOM-32U**
  (dual-core Xtensa LX6 up to 240MHz, external U.FL antenna, **no PSRAM** —
  it's WROOM not WROVER). Flash: 4MB on earlier board revisions, 16MB on at
  least one confirmed revision — check `esptool.py flash_id` per unit, don't
  assume.
- LAN: onboard **LAN8720A** RMII PHY, wired directly (not via J1), so it
  never competes with J1 signals for pins.
  - RMII data (fixed by ESP32 EMAC, not reassignable): TXD0=GPIO19,
    TXEN=GPIO21, TXD1=GPIO22, RXD0=GPIO25, RXD1=GPIO26, CRS_DV=GPIO27.
  - MDC=GPIO23, MDIO=GPIO18.
  - Clock: GPIO17 gates the external 50MHz oscillator's output-enable pin
    (drive HIGH before `ETH.begin()`, do NOT pass as ETH.begin()'s own
    `power` arg — causes reset conflicts on this board). GPIO0 receives the
    LAN8720A's buffered REFCLK output (`ETH_CLOCK_GPIO0_IN` mode) — GPIO0 is
    *also* tied to this board's USB auto-reset transistor net (shared role,
    worth knowing when debugging boot/upload issues that correlate with
    Ethernet).
  - **PHY address = 1**, not the arduino-esp32 default of 0 — must pass
    explicitly to `ETH.begin()` or `ARDUINO_EVENT_ETH_CONNECTED` never fires
    despite a good physical link.
  - No extra lib_deps — `ETH.h`/`esp_eth` ships with arduino-esp32 core.
- J1 (20-pin header) exposes: 3V3, GND, RXD(GPIO1/UART0 TX), TXD(GPIO3/UART0
  RX), EN, OUT1(GPIO2, boot-strapping-sensitive — avoid pulling low at
  reset), OUT2(GPIO5), OUT3(GPIO16), SDA(GPIO32), SCL(GPIO33),
  MISO(GPIO12, strapping — MTDI/flash voltage select, the riskiest one),
  MOSI(GPIO13), CLK(GPIO14, strapping), CS(GPIO15, strapping),
  ADC1(GPIO34, input-only, no internal pull), ADC2(GPIO35, input-only, no
  internal pull), GPIO36(input-only, no internal pull), **ADC4_PTT(GPIO39,
  input-only, no internal pull — net name literally says PTT, clearly
  meant for an external PTT-request input)**, GND, +5V.
- **Onboard USB**: Impero32 has its own USB connector/auto-reset circuit
  (shares GPIO0 with the Ethernet clock-input net, see above). Confirmed
  plan: the actual N1MM/logger control link will instead go through an
  **external, opto-isolated USB-serial adapter**, but wired to the *same*
  UART0 pins (J1 RXD/TXD = GPIO1/GPIO3) rather than the onboard USB port.
  Exact isolated-adapter part not chosen yet.
  - **Open follow-up (not yet asked/answered)**: if the onboard USB-serial
    chip stays populated and its port gets plugged in at the same time as
    the external isolated adapter, both would drive the same UART0 RX/TX
    net — possible bus contention unless the onboard chip tri-states its
    TX when its own USB side is unplugged (true for many USB-UART bridge
    chips, but should be confirmed for whatever's on this board) or the
    onboard USB is simply never used once the external adapter is in
    place. Flag before final assembly, not a firmware concern.
- **Flash size target: 4MB confirmed sufficient (decided).** This
  firmware has no large-footprint drivers pulling in extra flash: it's
  wired-Ethernet-only (no WiFi stack needed), plus ESPAsyncWebServer/
  AsyncWebSocket, ArduinoJson, and LittleFS with a config file that's
  well under a few KB. Per Espressif's current arduino-esp32 partition-
  table docs (https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/partition_table.html),
  a 4MB board already supports a scheme with **two ~1.9MB OTA-capable
  app partitions** (plus a small NVS/storage partition) — comfortably
  more than this project's expected compiled image size, and enough
  headroom to add OTA updates later without ever needing the 16MB
  revision. Decision: build against 4MB as the baseline target, using
  an OTA-capable partition scheme (two ~1.9MB app slots + a small
  LittleFS partition for `/config.json`) from the start as cheap
  insurance — even before OTA itself is implemented, since the flash is
  there either way. The current verified release build uses 971,821 bytes of
  its 1,966,080-byte application slot (49.4%) and 45,904 bytes of RAM
  (14.0%). 16MB-revision boards stay fully compatible (just unused
  headroom), so one firmware build target covers both revisions.

## Decisions made

- **LAN role: configuration, status/monitoring, AND manual web-triggered
  TX** (superseded the earlier "config/status only" recommendation once
  the user asked for a web text-entry send feature). Automated
  logger-driven TX control (N1MM etc.) stays on the serial link, unchanged
  — that's the path with a real "must respond immediately" expectation.
  Manual, human-clicked web TX is a different risk class: a variable
  delay of tens-to-hundreds of ms between clicking Send and key-up is
  imperceptible to an operator and doesn't affect the FSK signal itself
  once transmission starts (bit timing is fully handled by the ISR
  regardless of which path fed the buffer). So this doesn't compromise the
  timing goal, but it does introduce **concurrency** that didn't exist
  before — see the Firmware task architecture section below.
- **Hardware inhibit (`CPU_INH_PIN`) gates every TX path uniformly** — `[`,
  `PTT_USB_RTS_PIN`, and now the web Send button all go through the same
  inhibit check, exactly like today's firmware already does for the first
  two ("no matter what asks it to").
- **PTT/PA tail order confirmed 2026-09-27:** after the final transmitted bit,
  wait `pttTailMs`, release the main `PTT_PIN` first, wait `paTailMs`, then
  release `PTT_PA_PIN`. The PA output therefore encloses the complete main-PTT
  interval and always releases last. Hardware inhibit remains the exception:
  as a safety cutoff it releases both outputs immediately without tail delays.
- **Bit-timing architecture for ESP32**: replace TimerOne with the native
  hw_timer API. The reproducible build is pinned to Arduino-ESP32 2.0.17 and
  therefore uses `timerBegin(timer, divider, countUp)`,
  `timerAttachInterrupt(..., edge)`, and `timerAlarmWrite`. Do the actual
  `digitalWrite(FSK_PIN, ...)` bit toggle **inside**
  the ISR itself (Espressif docs confirm this is safe/supported), not via
  the old AVR-style flag-then-loop() pattern — removes dependency on loop()
  scheduling latency, which matters once WiFi/LAN driver tasks are
  competing for CPU. ISR must carry `IRAM_ATTR`/`ARDUINO_ISR_ATTR`.
- **Live LCD text without stealing bit time**: decouple "compute next
  character's bit pattern" (task context, done one full character-period
  ahead — 93-154ms of slack vs ~2-4ms I2C write) from "toggle the pin"
  (ISR, reads a pre-filled ring buffer). ISR does no I2C ever. To keep the
  *display* update genuinely live (shown exactly when that character
  starts going out, not up to one character early), the ISR sets a
  lightweight flag/index when a new character's start bit begins; the TX
  Manager task picks that up and does the actual `lcd.print()` I2C write
  then — mirrors today's `isrFlag` pattern but fully decoupled from bit
  timing since the toggle itself no longer depends on task scheduling at
  all.
- **Flash writes (config saves, future OTA) must be gated off during an
  active TX** — ESP32 briefly disables interrupts on both cores during a
  flash erase/write (low single-digit ms), long enough to matter at 75
  baud (6.65ms half-bit). Same discipline as the existing I2C-off-
  critical-path rule, just extended to flash. Applies no matter which
  storage backend — LittleFS writes hit the same physical SPI flash with
  the same critical-section behavior NVS would have had.
- **Dual-core use**: not strictly required at RTTY speeds (93-154ms/char
  gives huge margin), but worth doing as insurance now that LAN carries
  real request traffic — pin the TX Manager task to the same core as the
  timer ISR (default: core 1, since Arduino's own task runs there) with
  elevated priority, keep heavier work (the async web server, serial
  config menu) on the other core. Does NOT protect against the
  flash-write interrupt-disable issue above (that hits both cores).
- **Config storage: LittleFS + JSON** (`/config.json`, via ArduinoJson —
  user confirmed JSON over a hand-rolled format). Pairs with the web UI:
  the file can be served for download / accepted for upload as a
  config backup/restore feature. `LittleFS.h` ships with arduino-esp32
  core; ArduinoJson is one additional well-known dependency. LittleFS
  recommended over SPIFFS (legacy/being phased out in the ESP32 Arduino
  ecosystem). Should carry forward the same defensive-fallback discipline
  the AVR `eeLoad()` already has (blank/corrupt/missing file → compiled-in
  defaults, range-validate loaded values).
  - PlatformIO detail: `board_build.filesystem = littlefs` so
    `pio run --target uploadfs` writes a LittleFS image; the reserved
    flash region (the partition CSV's spiffs-labeled entry) works for
    either filesystem format, so no special partition changes beyond
    size (trivial — config is well under a few KB).
- I2C for the status LCD moves off any default pins to **SDA=GPIO32,
  SCL=GPIO33** (J1 already dedicates these) — required anyway since
  GPIO21/22 are taken by RMII TXEN/TXD1.
- **GPIO assignment for outputs is fully ours to choose** — nothing is
  externally hard-wired downstream of OUT1/OUT2/OUT3 or the SPI-labeled
  J1 pins yet (confirmed by hardware owner), so the mapping below is
  chosen purely for firmware-side safety, not constrained by existing
  driver circuitry. Whatever relay/PTT driver stage gets designed later
  should follow this assignment, not the other way around.
- **AVR/Nano EasyFSK-PLUS stays as a separate, untouched project.** The
  ESP32 port is a fresh, standalone codebase in its own new GitHub repo,
  not a fork/branch — see "Repo / project setup" above.
- **`ON_PIN` removed; `LED_RX_PIN` moved off a direct GPIO onto the
  MCP23017 I2C expander (confirmed 2026-09-02).** `ON_PIN` was reserved/
  unused and is dropped from the design outright. `LED_RX_PIN` (the RX
  indicator LED) is now driven as one more MCP23017 output pin instead
  of OUT1/GPIO2 — see the GPIO mapping table and the LED bargraph/
  MCP23017 section below for the freed pins and the updated MCP23017 pin
  budget this causes.
- **The two pins freed by the above were reassigned the same day to the
  digipot-trim rotary encoder's quadrature A/B lines (confirmed
  2026-09-02)**: `ENC_A_PIN`=OUT1/GPIO2, `ENC_B_PIN`=CLK/GPIO14, decoded
  via the ESP32's hardware PCNT peripheral rather than routed through
  the MCP23017 — see the GPIO mapping table and the digipot section's
  "encoder GPIO problem" subsection below (now largely superseded by
  this direct-pin assignment; kept for the pushbutton discussion).
- **LED bargraph reduced to 9 LEDs; 3 new encoder mode-select LEDs
  added; encoder pushbutton decided (all confirmed 2026-09-02, later
  the same day).** The encoder now cycles between 3 targets — RX
  volume, TX volume, CW speed (the last one an unscoped future
  feature) — via a short press on its pushbutton, with one of 3 new
  MCP23017-driven LEDs (`ENC_SEL_RX_LED`/`ENC_SEL_TX_LED`/
  `ENC_SEL_CW_LED`) showing the active target. This **supersedes the
  manual DPDT RX/TX switch plan** in the digipot section — see that
  section's "encoder mode-select LEDs" and "Architectural implication"
  notes, and the updated MCP23017 pin budget (14 of 16 committed).

## GPIO mapping for the remaining EasyFSK signals

**Implementation update, 2026-09-27:** the original near-final table below was
superseded in part when the independent UART2/Winkey input was implemented.
The current firmware moves `CPU_INH_PIN` from GPIO15 to input-only GPIO35
(therefore requiring an external pull-up), assigns UART2 TX/RX to GPIO15/36,
uses GPIO34 for the RX audio ADC monitor (added 2026-10-13), and still drives
`LED_RX_PIN` on GPIO2. The encoder is now definitively assigned GPIO2/GPIO14;
its handling is not implemented, and the RX LED must move to the MCP23017
before GPIO2 is used by the encoder. The unused `ON_PIN` assignment was
removed to free GPIO14. `include/Pins.h` and the README's GPIO table describe
the confirmed pin plan and call out firmware not yet migrated.

| Signal | J1 pin / GPIO | Rationale |
|---|---|---|
| `PTT_USB_RTS_PIN` | ADC4_PTT / GPIO39 | Net name literally says PTT; input-only is fine, external pull-up as today |
| `PTT_PIN` (safety-critical, must default LOW at boot) | OUT3 / GPIO16 | No strapping role at all — cleanest choice for the most safety-critical output |
| `PTT_PA_PIN` (safety-critical, must default LOW at boot) | MOSI / GPIO13 | No strapping role — the only clean pin among the SPI-labeled four |
| `FSK_PIN` | OUT2 / GPIO5 | Technically a strapping pin (SDIO slave timing) but that role is irrelevant to boot-mode selection and its required state is satisfied by the internal pull-up by default — much lower risk category than GPIO0/2/12/15 |
| `CPU_INH_PIN` (current implementation; external pull-up mandatory) | ADC2 / GPIO35 | Moved from GPIO15 to free UART2 TX; input-only and has no internal pull resistor |
| `RX_AUDIO_ADC_PIN` | ADC1 / GPIO34 | RX audio input, ADC1 channel 6, sampled by the DMA monitor |
| `UART2_TX_PIN` | CS / GPIO15 | Second hardware UART, mode-switched between FSK2 and Winkey |
| `UART2_RX_PIN` | GPIO36 | Second hardware UART RX; input-only is suitable, no pull resistor available |
| `LED_RX_PIN` (current firmware) | OUT1 / GPIO2 | RX indicator; hardware plan moves it to MCP23017 before GPIO2 is used by the encoder |
| `ENC_A_PIN` (confirmed hardware assignment) | OUT1 / GPIO2 | Encoder quadrature A; intended for ESP32 PCNT; encoder firmware not implemented |
| `ENC_B_PIN` (confirmed hardware assignment) | CLK / GPIO14 | Encoder quadrature B; replaces the unused legacy `ON_PIN`; encoder firmware not implemented |
| `CW_KEY_PIN` (confirmed dedicated CW output) | MISO / GPIO12 | Active HIGH; GPIO12 is the MTDI flash-voltage strap and must remain LOW at reset. The external load must not pull it HIGH; firmware initializes it LOW early in setup. |

The encoder pin assignment is final for the hardware plan, but encoder
handling is not implemented in firmware. The current firmware still drives
`LED_RX_PIN` on GPIO2; move that LED to the MCP23017 before connecting the
encoder. The unused legacy `ON_PIN` assignment and its initialization have
been removed so GPIO14 is available for encoder B.

GPIO35 and GPIO36 are no longer spare: the current firmware uses GPIO35
for `CPU_INH_PIN` (external pull-up required) and GPIO36 for UART2 RX.
GPIO34 remains the proposed, currently unassigned ADC audio input. The
confirmed encoder and dedicated CW output assignments use the available
direct pins. GPIO12 is used despite its strapping role only with a load that
does not pull it HIGH at reset. Any further physical controls remain planned
for the MCP23017, whose allocation is described below; the expander driver
itself is not implemented yet.

## Web UI architecture (config + monitoring + manual TX)

- **Stack: ESPAsyncWebServer + AsyncWebSocket** (user's choice over the
  built-in synchronous `WebServer.h` + polling alternative). Live
  status/text push over the WebSocket; config load/save and the "Send"
  action as regular HTTP endpoints (e.g. JSON body). Adds AsyncTCP +
  ESPAsyncWebServer as dependencies.
- **Send-during-active-TX behavior (user confirmed): append.** If a web
  Send arrives while already transmitting (from any source — serial or a
  previous web send), the typed text is appended to the existing send
  buffer, same as text arriving over serial mid-TX today — it does not
  interrupt or restart the current keying sequence. A web Send when idle
  behaves like a self-contained `[` + text + `]`: key up, send the text,
  auto-unkey when done.
- **Access control: none for now** (user's choice — "bez hesla", relying
  on the wired LAN being trusted). Worth revisiting later if the device
  ever ends up reachable beyond a trusted local segment.
- **UI language: English (user confirmed).** All page text, labels,
  hint text, and validation-error messages are in English, independent
  of the language this design conversation is conducted in.
- Live status feed content mirrors what the LCD already shows: callsign,
  PTT source (`FSK ..` vs `RTS DIGI`), RX/TX/INHIBIT state, and live TX
  text — natural to drive both the LCD and the WebSocket feed from the
  same underlying state-change events rather than duplicating logic.

### Web UI visual/structural design (reference: "Band Magic" config page)

- User provided, as a style/structure reference, the complete single-page
  web UI of "Band Magic" — a sibling ESP32 project by the same author
  (an antenna/band controller), not code meant to be reused as-is (its
  domain/functionality is unrelated to EasyFSK).
- **Fonts — decided: same pairing as the reference, via Google Fonts:**
  - **Lora** (serif) for headings/display text.
  - **Montserrat** (sans) for body text and UI controls.
- **Color scheme — decided: reuse the reference's token-based theming
  mechanism and its core colors:**
  - CSS custom properties for all colors (no hard-coded colors in
    component CSS), light theme by default, with a `data-theme="dark"`
    block overriding the same token names for dark mode.
  - Confirmed anchor tokens from the reference, carried over as-is:
    `--text: #120d38` (dark navy — primary text color, light theme) and
    `--accent: #dba41c` (warm gold — primary action color: buttons,
    active tab/state, highlights, focus rings).
  - The remaining tokens in the full set (backgrounds, card surfaces,
    borders, muted/secondary text, and their dark-theme overrides) are
    to be carried over verbatim from the reference file's `:root` /
    `[data-theme="dark"]` CSS blocks when implementation starts — same
    palette, not reinvented, just re-scoped to EasyFSK's own page
    content.
- **Also adopt** (structural/UX conventions, not the domain logic):
  - Card-based layout with tab/section navigation between logical groups
    of controls, rather than one long scrolling form.
  - Live status over WebSocket with reconnect/backoff handling and a
    visible connection-state indicator — matches the "Live status feed"
    decision above; reuse the reconnect pattern.
  - JSON REST endpoint conventions for config get/set and one-shot
    actions (mirrors the HTTP+JSON decision already made above).
  - Hint text under every control explaining what it does/what values
    are valid — good fit for PTT/PA timing fields, which are exactly the
    kind of setting that benefits from inline explanation.
  - JSON config backup/restore (download current config, upload to
    restore) — directly reuses the already-decided LittleFS+JSON config
    storage; the same `/config.json` can be exposed for download/upload
    largely as-is.
- **Do NOT adopt** — Band Magic-specific domain functionality that has no
  EasyFSK equivalent: antenna/band profile switching, IP-relay bit-pattern
  math, BCD inputs, CAT/FlexRadio/TrxNet integration, broadcast-to-LAN,
  and its OTA update flow. TCI is implemented as an optional parallel
  ExpertSDR3 transmit path; see the dedicated feature section at the end.
- **Proposed EasyFSK page/tab structure** to carry this visual treatment:
  1. **Status / Monitoring** — live via WebSocket: callsign, PTT source,
     RX/TX/INHIBIT state, live TX text, LAN link state.
  2. **Configuration** — callsign, PTT/PA lead-tail timing values, baud
     rate and other EasyFSK settings, LAN settings — each control with
     Band-Magic-style hint text underneath.
  3. **Send / TX** — text entry + Send/End/Abort buttons, following the
     already-decided TX semantics; shows current buffer/keying state;
     respects `CPU_INH_PIN` same as every other path.
  4. **Backup / Restore** — download/upload `/config.json`, following the
     reference's JSON backup/restore convention.

### Web UI: routes and JSON shapes (first draft)

Config fields below are taken directly from the AVR source's actual
persisted values (the `EE_*` EEPROM addresses and their load/validate
logic in `TinyFSK_ZAW_01.cpp`), not invented — same defaults and ranges
the firmware already enforces:

- `callsign` — string, ≤6 printable ASCII chars (0x20-0x7E), default "".
  Not used by the TX engine itself, display/reporting only (no auto-ID).
- `baudRate` — enum `45.45 | 50 | 75`, default `45.45`.
- `polarity` — enum `"markHigh" | "markLow"`, default `"markHigh"`.
- `pttLeadMs`, `pttTailMs`, `paLeadMs`, `paTailMs` — int, range 0-9999 ms
  each (`TIMING_MIN_MS`/`TIMING_MAX_MS` in the AVR source), out-of-range
  or blank rejected rather than silently clamped.
- `liveLcdText` — bool, default `true`.
- **Not exposed** (compile-time only in the AVR original — `usos` fixed
  at `USOS_MMTTY_HACK`, stop bits fixed at 1.5 — kept as compile-time
  constants for parity rather than added as new user-facing settings;
  trivial to promote to config fields later if wanted).
- **New for ESP32** — network settings, user confirmed both DHCP and
  static IP should be selectable:
  - `network.hostname` — string.
  - `network.dhcp` — bool.
  - `network.staticIp`, `network.gateway`, `network.subnet`,
    `network.dns` — strings, required/used only when `dhcp` is `false`;
    ignored (but still stored) when `dhcp` is `true`. The Configuration
    page shows/hides these fields based on the DHCP toggle.
- **Planned for the tuning indicator / RTTY decoder (not in the current
  firmware config)**:
  - `tune.enabled` — bool, default `false`.
  - `tune.shiftHz` — int, 50-1000, default 170. Informational label only.
  - `tune.markHz`, `tune.spaceHz` — int, 300-3000 each, must differ,
    defaults 2125/2295. The actual two frequencies the Goertzel/I-Q
    detector (and, once built, the RTTY decoder) would look for. Deliberately
    plain editable fields rather than a Shift-driven preset dropdown,
    since the Shift→tone-pair convention for anything other than
    170Hz@2125/2295Hz varies by software/club/region and shouldn't be
    hard-coded as if it were standardized. Earlier notes saying these
    fields were implemented are stale: current `Config`/`ConfigStore`
    do not contain `tune.*`.
- **Planned for the RX/TX digipot trim (not in the current firmware
  config or code)**:
  - `rxLevelPct`, `txLevelPct` — int, 0-100 each, default TBD (probably
    50, pending the digipot part decision). The intended behavior is to
    persist and restore the levels once a digipot and encoder are
    implemented; no wiper writes or level readouts exist in the current
    firmware.
- **Current CW keying / Winkey configuration (implemented)**:
  - `uart2Mode` — enum `"fsk2" | "cw"`, default `"fsk2"`. This selects
    UART2's role only; the primary UART1/ESP32 UART0 link remains
    unconditionally FSK/RTTY.
  - `cwSpeedWpm` — integer, 5-99, default 20, persisted as the
    power-on/default speed. Winkey speed commands change the running
    speed only and do not write the host's choice to flash. The planned
    encoder does not exist yet.
- **Historical, superseded CW configuration (do not implement)** (added
  2026-09-03; superseded
  2026-09-27 — see "UART1/UART2 split" in the CW/Winkey section. As
  implemented, the field is `uart2Mode: "fsk2" | "cw"` and selects
  UART2's role; UART0/J1 always stays the RTTY link. This entry is kept
  only as decision history)**:
  - `serialMode` — enum `"rtty" | "cw"`, default `"rtty"`. Selects which
    protocol UART0/J1's RXD-TXD link currently speaks — see the "CW
    keying via Winkey protocol emulation" section for why these can't
    be simultaneous (9600/8-N-1 RTTY control vs. Winkey's fixed
    1200/8-N-2). Settable from the Configuration page (a toggle) and
    from the encoder pushbutton's long-press gesture — the web toggle
    and the physical long-press both just write this same field.
  - The former `serialMode` field and the encoder-based live speed
    control described in this historical draft are not the current
    configuration/API. See the implemented fields above and the
    UART1/UART2 split below.

Endpoints:

- `GET /api/config` → returns the full config object above (one JSON
  document, mirrors `/config.json` on LittleFS).
- `POST /api/config` → body = the full config object (the Configuration
  page submits the whole form as one save, not per-field auto-save).
  Server validates every field with the same rules the AVR firmware
  already enforces. Response:
  - `{"ok": true, "config": {...}}` — echoes the now-active config.
  - `{"ok": false, "errors": {"pttLeadMs": "must be 0-9999", ...}}` —
    validation failure, nothing applied.
  - `{"ok": false, "deferred": true, "message": "..."}` — TX was active
    (`ptt == true`) when the save was requested, per the already-decided
    flash-write-gated-during-TX rule; UI shows "will save once TX ends"
    (English, per the UI-language decision above) and can retry.
- `GET /api/config/backup` → same JSON, served as
  `Content-Disposition: attachment; filename="easyfsk-config.json"` for
  the Backup/Restore page's Download button.
- `POST /api/config/restore` → body = an uploaded config.json's raw
  contents; same full validation as `POST /api/config`; on success
  replaces the LittleFS file and applies live (same TX-gating rule).
- `GET /api/status` → one-shot snapshot, same shape as the WebSocket
  push below — used for the initial page load before the socket
  connects, and as a fallback if the socket drops.
- `GET /api/system` → read-only diagnostic info for a footer/info
  section: firmware version string, MAC address, IP address, flash
  size, free heap.
- **TX actions** — each passes through the same `CPU_INH_PIN` check as
  every existing path and posts into the TX Manager's queue exactly
  like a serial byte would; all respond `{"ok": true}` or
  `{"ok": false, "reason": "inhibited"}` etc.:
  - `POST /api/tx/send` `{"text": "CQ CQ DE OK2ZAW"}` → append to
    buffer; if idle, behaves like `[` + text (auto key-up); if already
    sending, appends (already-decided append semantics).
  - `POST /api/tx/end` → posts `TX_END` (`]`): buffered switch to RX
    once the buffer drains.
  - `POST /api/tx/abort` → posts `TX_ABORT` (`\`): immediate switch to
    the end-of-data path and clears the pending buffer; the established
    PA/PTT tail sequence still runs before RX. Only `CPU_INH_PIN` forces
    both outputs LOW immediately. (User confirmed: Send page gets explicit
    End and Abort buttons alongside Send, matching N1MM's
    {TX}/{END}/{ESC} trio, not just a bare Send box.)

`WebSocket /ws` — **server → client push only.** The client never sends
TX commands over the socket; those go through the REST TX actions above
instead, so every action gets a definite HTTP success/failure response
rather than an ambiguous fire-and-forget over a socket that might be
mid-reconnect. Two message types:
- Pushed immediately on every state-change event (mirrors what already
  drives the LCD today):
  `{"type":"status","state":"idle"|"tx","pttActive":bool,"paActive":bool,`
  `"pttSource":"serial"|"uart2fsk"|"winkey"|"rts"|"web",`
  `"inhibited":bool,"callsign":"...",`
  `"char":"H","bufferPending":12}`
  This is the currently implemented shape, with UART2's FSK and Winkey
  sources in addition to UART1, RTS, and web TX. The previously proposed
  `rxLevelPct`, `txLevelPct`, and `signalQualityPct` fields are not
  present yet because the digipot and `TuneMonitor` features are not
  implemented.
- A lightweight heartbeat roughly every 1s regardless of TX activity, so
  the UI's connection indicator doesn't depend on RTTY traffic ever
  happening:
  `{"type":"heartbeat","uptimeMs":...,"heapFree":...,"linkUp":true}`.
  These status and heartbeat messages are implemented; no Tune-tab
  telemetry or binary waterfall frames are emitted yet.

Page ↔ endpoint mapping:
1. **Status/Monitoring** — connects `/ws`, falls back to
   `GET /api/status`; connection indicator driven by the heartbeat.
2. **Configuration** — `GET /api/config` on load, `POST /api/config` on
   Save; static-IP fields shown/hidden based on the `dhcp` toggle.
3. **Send/TX** — `POST /api/tx/send` / `/end` / `/abort`; reflects live
   state from the same `/ws` feed as the Status page.
4. **Backup/Restore** — `GET /api/config/backup` (download),
   `POST /api/config/restore` (upload).

## Firmware task architecture (as implemented)

1. **Half-bit timer ISR** (`FskTimer.cpp`, hardware timer via
   `timerBegin`/`timerAttachInterrupt`/`timerAlarmWrite`, `ARDUINO_ISR_ATTR`).
   Ports `processHalfBit()`'s bit-position/stop-bit state machine.
   Reads a 4-slot ring buffer of pre-computed Baudot symbols (fed by
   TxManager, one character ahead), toggles `FSK_PIN` directly from the
   ISR. Never touches `PTT_PIN`/`PTT_PA_PIN`, I2C, or flash. An
   ISR-local `s_halted` latch parks the ISR cleanly at the last stop-bit's
   mark level once `TX_END_FLAG` is dequeued, instead of idle-diddling
   while TxManager notices. Underrun falls back to idle-diddle (LTRS_SHIFT)
   exactly like the AVR original.
2. **TxManager task** (`TxManager.cpp`), pinned to core 1 (same as the
   timer ISR), priority `configMAX_PRIORITIES - 2`. Sole owner of the
   send buffer, PTT/PA state, and the lead/tail sequencing. A FreeRTOS
   queue (depth 128) carries messages from every producer — UART1 serial,
   UART2 (FSK2 or Winkey CW), the RTS-style hardware input, and the web
   Send handler — exactly per the concurrency design above. Only one
   session is on air at a time; see "Session hand-off" below for what
   happens to requests that arrive while another session is running or
   ending. PTT/PA lead-tail sequencing is the
   non-blocking `millis()`-based state machine confirmed in the original
   design (abort/end serviced immediately even mid-lead-delay). Faithful
   ports of two AVR nuances found by re-reading the source carefully:
   RTS deassert does NOT force an immediate unkey — it clears the RTS
   session and lets any serial-typed text already in the buffer drain
   through the normal end-of-data path first; and `TX_ABORT` is NOT
   instantaneous — it still runs the PA/PTT tail delay, same as a
   graceful end (only the hardware `CPU_INH_PIN` path is truly instant).
3. **SerialControl** (`SerialControl.cpp`) — thin per-byte state machine
   mirroring the AVR's config-menu command letters
   (`0`/`1`/`4`/`5`/`7`/`?`/`D`/`d`/`L`/`T`/`l`/`t`/`C`/`~`) plus
   `[`/`]`/`\` → TxManager queue messages.
4. **WebInterface** (`WebInterface.cpp` + `data/index.html`) —
   ESPAsyncWebServer + AsyncWebSocket exactly per the routes/JSON shapes
   drafted above. A push task pinned to core 0 polls TxManager's status
   and broadcasts a `status` WS message on every change plus a 1s
   `heartbeat`. TX actions post into the same TxManager queue serial
   uses.
5. **ConfigStore** (`ConfigStore.cpp`) — mutex-protected shared `Config`,
   LittleFS + ArduinoJson v7 (`/config.json`), refuses to persist (and
   reports it via the API's `errors` shape) while a TX is active, per
   the flash-write-during-TX rule. Apply actions that belong to another
   task are handed over rather than executed from the saving task: CW
   timing goes through the TxManager queue, and a UART2 mode switch is
   performed by `WinkeyEmulator::poll()` in `loop()`, which owns UART2.
6. **WinkeyEmulator** (`WinkeyEmulator.cpp`) — owns UART2, polled from
   `loop()` right after SerialControl (UART1 therefore has priority).
   In `fsk2` mode it forwards the same `[`/`]`/`\` protocol as UART1; in
   `cw` mode it parses the Winkey host protocol and feeds TxManager's CW
   engine (`Morse`/`CwTimer`, `CW_KEY_PIN` used as the dedicated key line).

**Naming note:** "UART1"/"FSK1" in the code and LCD means the primary
logger link, which is the ESP32's hardware **UART0** (`Serial`, J1
RXD/TXD). "UART2"/"FSK2" is `Serial2`. The labels follow the product's
user-facing numbering, not the ESP32 peripheral numbers.

### Session hand-off (implemented 2026-09-28)

Producers don't wait for each other, so TxManager decides what happens to
a request that arrives while the transmitter is busy. The rules live in
`include/TxHandoff.h` (pure functions, host-tested):

- **RTTY TX_ON during the PTT/PA tail is deferred, not dropped.** The tail
  belongs to a session that is already over, and its buffer is cleared
  when PA drops. A `[` arriving then (N1MM sending the next message right
  after the previous one, or the web Send button) is remembered together
  with a following `]`, its text is kept, and it starts as a new session,
  with a new lead-in, as soon as PA has dropped. Before this, such a
  message was silently lost.
- **RTTY TX_ON during a CW session is deferred** in the same way and starts
  after the CW session. A redundant TX_ON for the engine already on air is
  still ignored, as in the AVR original.
- **CW arriving during an RTTY session waits in the CW buffer** and starts
  as the next session after RTTY. The Winkey status reports BUSY while it
  waits. When both are waiting, RTTY goes first (UART1 priority).
- **CW arriving during its own session's PTT tail resumes keying without
  dropping PTT**, as a real WinKey does, instead of releasing PTT and
  running the lead-in again for every burst of typed text.
- **Web Send appends to the active session only while an RTTY session is
  actually sending.** During a tail or a CW session it sends its own
  `[text]`, which is then deferred as above.

## Current implementation status (2026-10-12)

This section supersedes the dated 2026-09-01 bring-up notes and any
earlier statements that the repository is empty, the first ESP32 compile
is still pending, or the tuning fields already exist in the current config.

- The current firmware targets PlatformIO `espressif32 @ 7.1.3`, which
  supplies Arduino-ESP32 2.0.17. The timer code intentionally uses that
  core's `timerBegin(timer, divider, countUp)`,
  `timerAttachInterrupt(..., edge)`, and `timerAlarmWrite` API. Upgrade
  the platform and port both timer modules together; the 3.x timer API
  described in some earlier research notes is not what this build uses.
- The firmware build is verified in CI. The recorded release build uses
  971,821 bytes of the 1,966,080-byte application slot (49.4%) and
  45,904 bytes of RAM (14.0%). CI also runs the host-side Unity tests;
  the current README records 127 passing tests. Hardware bring-up and
  timing validation on an actual Impero32 remain outstanding.
- Implemented: direct GPIO FSK transmit and PTT/PA sequencing, UART1
  RTTY control, UART2 selectable as FSK2 or Winkey, the CW/Morse engine,
  the web status/configuration/backup/manual-TX endpoints, and
  LittleFS/JSON configuration. Configuration persistence uses a
  temporary file and atomic replacement; network changes take effect
  after restart. The current web UI does not expose Tune, RX/TX trim, or
  encoder controls.
- UART2 Winkey command parsing and the CW path are implemented. The
  hardware `MODE_SEL_PIN` is not: `ModeSelect` currently tracks the
  requested state only because the MCP23017 driver and hardware are not
  present. No encoder, digipot, ADC audio receiver, RTTY decoder, LED
  bargraph, Lissajous, or waterfall is implemented.
- The live persisted `Config` currently contains the AVR-compatible
  transmit settings, network settings, `uart2Mode` (default `fsk2`), and
  `cwSpeedWpm` (default 20). The configured CW speed is restored at
  startup; Winkey host speed commands change the running engine speed
  without persisting each host command. There are no `tune.*`,
  `rxLevelPct`, or `txLevelPct` fields in the firmware yet.
- `include/Pins.h`, `include/Config.h`, `src/`, `data/index.html`,
  `platformio.ini`, the CI workflow, and the README describe the
  implementation that currently exists. The remaining sections below
  are decisions/research for planned work unless explicitly marked
  implemented.

## Planned feature: RTTY audio tuning indicator + spectrum waterfall (confirmed 2026-09-02, not started)

User's idea (2026-09-02): add an audio-input "tuning eye" — sample the
receiver's RX audio (~300Hz-3kHz band), detect the two RTTY tones (Mark
and Space, separated by the station's Shift), and show a Lissajous-style
figure on the web UI (and ideally something on the LCD) so the operator
can visually zero-beat/tune the radio, the way classic terminal-unit
scopes worked. A scrolling spectrum ("waterfall") display was then
discussed as a natural extension. **User has confirmed both the
Lissajous tuning indicator and the waterfall as planned features — this
whole section is now "yes, build it," not just an idea under review.
Research and architecture sketched below; no DSP/ADC code or `tune.*`
configuration fields have been written yet.** This is a
new module, additive to everything above, not a change to the TX path.

### Technical findings (verified via web research, not assumed)

- **ADC sampling**: `analogRead()` (blocking, one-shot) is too slow/
  jittery for audio-rate sampling. The firmware now uses the pinned
  Arduino-ESP32 2.0.17 core's ESP-IDF ADC digital DMA API
  (`adc_digi_initialize`/`adc_digi_controller_config`/
  `adc_digi_read_bytes`) for GPIO34 at 20kHz; do not copy Arduino-ESP32
  3.x `analogContinuous*` calls into this core unchanged. The raw samples
  are currently consumed by the live input-level/clipping monitor. The
  same stream remains the planned source for the RTTY detector, tuning
  display, and FFT waterfall. Docs:
  https://docs.espressif.com/projects/arduino-esp32/en/latest/api/adc.html
- **Continuous mode is ADC1-only.** GPIO34/35/36 are ADC1 channels
  (GPIO34=ADC1_CH6, GPIO35=ADC1_CH7, GPIO36=ADC1_CH0), but the current
  firmware already assigns GPIO35 to `CPU_INH_PIN` and GPIO36 to UART2
  RX. **Proposed audio pin: GPIO34** (J1's "ADC1" position), which is
  currently unassigned.
- **Sample-rate floor, real gotcha**: the ESP32's `adc_continuous`
  driver enforces a minimum `sample_freq_hz` of **20 kHz**
  (`SOC_ADC_SAMPLE_FREQ_THRES_LOW`) — you cannot ask for a "nice" 8kHz
  audio rate directly. Not a problem here (Nyquist for a 3kHz signal
  only needs >6kHz), just means the detector runs at 20kHz sample rate
  and does its own filtering/decimation in software rather than relying
  on a slow hardware rate.
- **Detection method — decided: Goertzel-family / quadrature (I/Q)
  detection per target tone, NOT a full FFT (for the Lissajous).** We
  only ever care about two known frequencies there, not a spectrum: a
  per-frequency Goertzel (or the closely related digital lock-in /
  synchronous I-Q downconversion used for the continuous version below)
  costs roughly O(N) per target tone vs. O(N log N) for a full-spectrum
  FFT — several times cheaper for exactly two bins, and needs no FFT/
  windowing/bit-reversal machinery. Existing plain-C Arduino examples
  worth a look (not ESP32-specific, port trivially):
  https://github.com/jacobrosenthal/Goertzel and
  https://github.com/AI5GW/Goertzel (explicitly aimed at multi-tone/FSK
  decoding). The waterfall (see its own section below) is the opposite
  case — it genuinely needs an FFT, since it wants many bins, not two.
  `espressif/esp-dsp` (https://github.com/espressif/esp-dsp) is
  Espressif's current DSP library (FFT, filters, etc.) but ships as an
  ESP-IDF component, not a PlatformIO/Arduino Library-Manager package —
  doable to pull in but likely unnecessary; a from-scratch Goertzel/I-Q
  implementation for the tuning indicator, plus `kosme/arduinoFFT` (a
  normal PlatformIO package, see the waterfall section) for the
  spectrum, cover both needs without touching esp-dsp at all.
- **ESP32 ADC signal-quality caveat (real, not hypothetical)**: the
  built-in ADC has documented nonlinearity (especially near the low/high
  ends of each attenuation range), unit-to-unit variation, and real
  noise on raw counts — well-documented community pain, not FUD (e.g.
  https://esp32.com/viewtopic.php?t=2881,
  https://github.com/espressif/esp-idf/issues/164). See the dedicated
  **"Analog front-end circuit for GPIO34"** section below for what this
  means concretely for the hardware side.

### Decisions made (user confirmed, 2026-09-02)

- **Display goal: a true, continuously-updating Lissajous figure**, not
  a simpler "two bargraphs/needles" tuning eye. This means the firmware
  needs to track each tone's instantaneous amplitude *and* phase over
  time (a continuous I/Q "complex envelope" per tone via synchronous
  downconversion + low-pass filtering), not just one magnitude value per
  analysis block — an off-frequency signal should visibly rotate/distort
  the figure, a correctly-tuned signal should hold it steady, matching
  how classic XY-scope RTTY tuning indicators behave.
- **Mark/Space tone pairs: independently editable Hz fields (not a
  preset-driven dropdown), confirmed 2026-09-02.** The planned
  Configuration-page controls are plain `Shift (Hz)` / `Mark tone (Hz)` /
  `Space tone (Hz)` number fields, defaulting to 170/2125/2295 — the one
  combination treated as a
  genuinely established convention. This replaced the earlier idea of a
  Shift-driven preset dropdown that would auto-fill Mark/Space for
  200/425/850Hz, since that tone-pair convention varies by software/
  club/region and would have meant guessing values into the firmware.
- **Waterfall: confirmed as a planned feature alongside the Lissajous**
  (2026-09-02), sharing the same audio pipeline — see its own section
  below.
- **LED bargraph: confirmed as a third planned display alongside the
  Lissajous and waterfall (2026-09-02)** — see its own section below.
  The web/LCD Lissajous+waterfall plan above is unaffected; the bargraph
  is a fourth consumer of the same I/Q data, not a replacement for
  anything above.

### Proposed architecture sketch (not yet implemented)

- New module (working name **`TuneMonitor`**), its own FreeRTOS task
  pinned to **core 0** (with WebInterface, well away from the timing-
  critical `FskTimer` ISR / `TxManager` task on core 1) — this is a
  receive-side helper, zero interaction with the TX path or its timing
  guarantees.
- ADC continuous mode on GPIO34 at 20kHz; each incoming sample is fed
  through two per-tone synchronous I/Q mixers (multiply by local
  cos/sin references at `cfg.tune.markHz`/`spaceHz`, then low-pass
  filter each product) producing continuously-updating I(t)/Q(t) for
  each tone — the "complex envelope." Cheap per-sample (a couple of
  multiplies + filter updates per tone), same computational family as
  Goertzel just run continuously rather than block-by-block. The same
  sample stream also feeds the waterfall's periodic FFT blocks (see
  below), the RTTY decoder's tone comparator (see its own section
  further down), and now the LED bargraph (see its own section) — one
  ADC pipeline, four consumers.
- Downsampled I/Q points (e.g. ~30-50 updates/sec — plenty for a smooth
  on-screen figure, light on WebSocket bandwidth) pushed to a new **Tune**
  tab's `<canvas>` via a new WS message type, drawn as a scrolling/
  fading XY trace — a natural fit for the already-decided WebSocket-push
  architecture (same pattern as the existing `status`/`heartbeat`
  messages).
- **LCD (16x2 character display) cannot show a real Lissajous figure.**
  Needs its own, cruder representation if wanted there too — e.g. a
  simple bar/needle approximation of relative Mark/Space tone strength,
  or skip the LCD entirely for this feature and treat the web Tune tab
  as the only display. **Not yet decided which** — though see the RTTY
  decoder section below, which does have a concrete LCD plan (reusing
  the existing live-text line), and the new LED bargraph section below,
  which is itself effectively "the LCD's missing Lissajous," in hardware
  form.
- Config: `tune.enabled`/`shiftHz`/`markHz`/`spaceHz` are proposed
  settings, not present in current `Config` or the web UI yet.

### Confirmed: running spectrum / waterfall on the web UI (2026-09-02)

User asked how complex it would be to also add a scrolling spectrum
("waterfall") display next to/instead of the Lissajous, then confirmed
it should be included in the plan. **Verdict: moderate incremental
complexity — reuses the same ADC audio pipeline already needed for the
tuning indicator, no new hardware, but is a genuinely different DSP
technique (FFT, not Goertzel/I-Q) with its own small chunk of new code.**

- **Why it's a different technique**: Goertzel/I-Q (chosen above for the
  Lissajous) is cheap specifically *because* it only evaluates two known
  frequencies. A waterfall needs magnitude across many frequency bins,
  which is what FFT is for — there's no way to get a full spectrum out
  of the two-tone detector; this is an additive feature, not a re-use of
  the same math.
- **Library choice — verified**: `kosme/arduinoFFT`
  (https://github.com/kosme/arduinoFFT) is actively maintained (latest
  release Nov 2024) and is a normal PlatformIO registry package
  (`lib_deps = kosme/arduinoFFT@^2.0.4`) — unlike `esp-dsp`, no
  ESP-IDF-component wrangling needed, fits the existing `lib_deps` list
  the same way LiquidCrystal_I2C/ArduinoJson/ESPAsyncWebServer already
  do.
- **Performance headroom — verified**: Espressif's own published
  benchmarks (https://docs.espressif.com/projects/esp-dsp/en/latest/esp32/esp-dsp-benchmarks.html)
  show a 1024-point complex FFT costs ~113k cycles on the ESP32's
  Xtensa LX6 (≈0.47ms at 240MHz) with their hand-optimized assembly
  routines; `arduinoFFT` (plain C++, no SIMD) will be several times
  slower but even a pessimistic 5-10x penalty is only ~2-5ms — trivial
  against a ~50ms frame budget (see next point). Plenty of headroom to
  share core 0 with the web server and the Lissajous I/Q processing.
- **Sizing at the already-fixed 20kHz ADC sample rate**: a 1024-point
  FFT block = 51.2ms of audio → ~19-20 spectrum updates/sec, resolution
  ≈19.5Hz/bin (plenty for RTTY tones). The 300Hz-3kHz band of interest
  is only ~140 of the 512 usable bins — a small slice to actually push
  to the browser each frame. A smaller FFT (e.g. 512) would halve
  resolution but double the update rate if a snappier waterfall is
  preferred.
- **Data path**: same ADC continuous-mode sample stream already needed
  for the Lissajous feeds both consumers — the per-sample I-Q mixers
  *and*, once every ~1024 samples, a windowed (Hann, to reduce spectral
  leakage) FFT block. Result bins (magnitude, ideally scaled/clamped to
  dB and quantized to one byte per bin) pushed as a **binary** WebSocket
  frame (AsyncWebSocket supports binary sends directly — better than
  JSON here: ~140 bytes/frame × ~20/sec ≈ 2.8KB/s, trivial on a wired
  LAN, and no JSON parse overhead client-side for something rendered
  every frame).
- **Browser rendering**: standard scrolling-waterfall canvas technique —
  map each new magnitude row through a color palette (e.g. a simple
  blue→yellow→red heat gradient) into one row of pixels, shift the
  previous image down/up a row (`drawImage` self-copy is the usual
  lightweight trick), draw the new row on top. Plain `<canvas>` +
  vanilla JS, no charting library needed — fits the existing
  single-file `data/index.html` style. Would live in the same new
  **Tune** tab as the Lissajous scope (or a sub-tab), since both draw
  from the same underlying audio task.
- **Shared caveat**: the same ESP32 ADC noise/nonlinearity concern
  applies here too, arguably more visibly — a noisy front end shows up
  as visible speckle/noise floor in a waterfall more obviously than in
  a Lissajous shape, so the analog front-end quality question (see next
  section) matters equally for both displays.
- **Net assessment**: doable as an extension of the same `TuneMonitor`
  module rather than a separate subsystem — no new hardware/pins beyond
  what the Lissajous already needs, one new library dependency
  (`arduinoFFT`), one new small block of firmware code (FFT + windowing
  + dB scaling + binary WS framing), and one canvas renderer on the web
  side. Not "free," but meaningfully cheaper than doing it as a
  from-scratch second feature, since the ADC sampling and analog front
  end are shared costs already being paid for the Lissajous.

### Confirmed: Signal Quality metric on the Tune page (2026-09-03)

While drafting the Tune page's web mockup, a "Signal Quality" meter was
sketched next to the Lissajous/waterfall; the user then asked how it
should actually be calculated. **Decided 2026-09-03: an SNR-style
metric — tone energy vs. broadband noise floor — deliberately kept
orthogonal to tuning accuracy, which the Lissajous shape and the Hz-
deviation readout already show.**

- **Reuses the existing I/Q pipeline, one small addition.** `magM =
  sqrt(I_mark² + Q_mark²)` and `magS = sqrt(I_space² + Q_space²)` are
  already computed continuously for the Lissajous (see the
  `TuneMonitor` architecture sketch above) — that's the tone energy.
  The only new piece is a noise-floor estimate: one more running RMS
  accumulator (`magTotal`) over the same raw 20kHz ADC sample stream,
  with no per-tone filtering — a broadband envelope of everything in
  the passband, tone energy included. Cheap (one more sum-of-squares
  running average), no new sampling or hardware.
- **Formula**: `SNR_dB = 20 * log10( (magM + magS) / max(magTotal -
  (magM + magS), floor) )` — tone energy over what's left after
  removing it from the broadband total (noise + anything else in the
  passband). `floor` is a small epsilon to avoid divide-by-zero/log(0)
  when there's effectively no residual.
- **Mapping to 0-100%: confirmed fixed in firmware, not configurable**
  (user's choice over adding two more Configuration-page fields) —
  0 dB → 0%, ~30 dB → 100%, linear in between, clamped outside that
  range. Not meant to be a lab-accurate SNR figure, just a monotonic,
  stable bar the operator can watch while peaking the receiver — same
  spirit as an S-meter.
- **Deliberately excludes tone dominance/contrast** (e.g. `magM` vs.
  `magS` ratio). That signal is already visible in the Lissajous's
  shape (an off-frequency or unbalanced signal distorts/rotates the
  figure) and the Hz-deviation readout next to it — folding it into
  "quality" too would double up on the same information and could
  make a strong-but-mistuned signal read as worse than it electrically
  is, which isn't what this meter is for.
- **Web display**: extends the WebSocket `status` message with one more
  field, e.g. `"signalQualityPct": 82`, pushed on the same cadence as
  the I/Q-derived fields already planned for the Tune page — see the
  updated JSON shape in "Web UI: routes and JSON shapes" below. Shown
  as a labeled bar on the Tune page's "Signal" card (already sketched
  in the web mockup) — text label (e.g. "Good") is a simple threshold
  lookup over the same percentage, exact wording not yet decided.
- **Not yet decided**: exact label thresholds/wording for the bar's
  text (e.g. Poor/Fair/Good/Excellent cut points), and whether this
  value is also useful as an input to the RTTY decoder (e.g. to flag
  low-confidence decoded characters) — today it's display-only,
  computed by `TuneMonitor`, not consumed anywhere else.

### Analog front-end circuit for GPIO34 (hardware note, refined 2026-09-02)

The user asked for detail on the earlier "needs a decent analog front
end" flag, then confirmed the intended approach: **use the op-amp itself
to set the DC bias, and fold the anti-alias low-pass filter into the
same op-amp stage** rather than a separate passive RC network ahead of a
plain buffer. This is a **hardware task, not firmware** — same spirit as
the opto-isolated USB-serial adapter note elsewhere in this doc.

**Why a front end is needed at all:**
- The ESP32 ADC's internal reference is only ~1.1V (nominally, with
  1000-1200mV manufacturing spread), and each attenuation setting scales
  the usable input range from there: roughly 0-1.1V at 0dB attenuation
  up to roughly 0-3.1V at the widest (11dB/12dB, naming varies by
  arduino-esp32 version) setting — verified against Espressif's current
  ADC docs (https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/adc/index.html),
  though the exact per-attenuation voltage table itself is only in the
  chip datasheet, not that page. Radio audio output is AC (swings
  positive and negative around 0V) — fed directly into the ADC it would
  spend half its swing below 0V, invisible to the ADC — so it needs DC
  bias to sit inside that 0-3.1V window before it can be digitized.
- Community-documented ESP32 ADC guidance (e.g.
  https://esp32.com/viewtopic.php?t=2881) is that the ADC gives more
  accurate, repeatable readings from a **low-impedance source** — a bare
  resistor-divider bias network feeding the pin directly is a common
  cause of noisy/inaccurate readings, hence driving it through an
  op-amp.

**Combined bias + filter + buffer, one active stage (the chosen
approach):**

Rather than a separate passive RC filter followed by a unity-gain
buffer, both the DC bias and the anti-alias filtering are folded into
the same op-amp stage using the standard single-supply "AC-coupled,
biased, filtered" input trick:

1. Radio audio out → **series coupling capacitor** C_in (e.g. 1-10µF,
   blocks the source's own DC) → **filter resistor** Rf (e.g. 10kΩ) →
   op-amp's non-inverting (+) input.
2. Same (+) node also receives the **DC bias** through a much larger
   resistor Rbias (e.g. 100kΩ, so it barely loads the AC path) from a
   simple bias reference (e.g. a 100kΩ/100kΩ divider off 3.3V, ≈1.65V,
   optionally decoupled with a cap for cleanliness).
3. A **filter capacitor** Cf (e.g. 4.7nF) from that same (+) node to the
   bias/AC-ground rail, forming a single-pole low-pass together with Rf
   — corner frequency f = 1/(2π·Rf·Cf), e.g. ≈3.4kHz with the values
   above. Rbias being ~10x Rf makes its loading effect on that corner
   small (a few percent), fine to ignore for this application.
4. Op-amp output (as a unity-gain follower, or with modest gain via a
   feedback network if the signal needs boosting) drives GPIO34
   directly — one active stage does bias, filtering, *and* buffering at
   once, replacing the earlier "passive RC filter + separate buffer"
   sketch with something simpler and equally effective.
5. Firmware sets the pin's attenuation to the widest setting (11dB/12dB)
   to use the full ~0-3.1V window.

**Filter order**: this is a single-pole (6dB/octave) filter. Given the
signal of interest tops out around 3kHz and the fixed 20kHz ADC sample
rate puts Nyquist at 10kHz (more than an octave of margin), a single
pole should be plenty — no need to chase a steeper rolloff for this
application. If more attenuation of anything above the passband is
wanted later, the same single op-amp can be reconfigured as a two-pole
Sallen-Key low-pass (one extra R + C, same part count otherwise), but
that's an optimization, not a requirement given the margin here.

**Simplification path (unchanged from before)**: if the actual audio
tap used is already a dedicated, roughly fixed-level, lowish-impedance
source (e.g. a modern rig's dedicated "data"/ACC audio output, as
opposed to raw speaker output), an even simpler coupling-cap + bias-
divider network without any active stage might be "good enough" for a
two-tone Goertzel detector and a waterfall display, neither of which
needs lab-grade absolute accuracy — worth trying on the bench before
committing to the op-amp version, though the op-amp version above is
the one the user has settled on.

This remains a **starting point for the user's own hardware review**,
not a finished/verified design — exact component values and the final
schematic are for the user to work out and bench-test.

### Not yet decided (Lissajous/waterfall)
- Final analog front-end component values (see dedicated section above)
  — needs the user's own bench verification; topology (combined bias +
  filter + buffer in one op-amp stage) is settled, exact R/C values are
  a starting suggestion only.
- LCD representation for the Lissajous itself (if any) — real Lissajous
  isn't possible on a 16x2 character display. (The RTTY decoder section
  below does reuse the LCD's existing live-text line for decoded text,
  which is a separate, already-planned use of that line; the new LED
  bargraph section below is the more likely answer to "give the
  Lissajous a physical/LCD-adjacent presence.")
- WS message shape/update rate for the I/Q point stream, and exact
  low-pass filter time constant for the I/Q "complex envelope" (trades
  off figure smoothness vs. how quickly it reacts to retuning).
- Waterfall FFT size/update-rate tradeoff (1024 vs 512), color palette,
  and whether it lives on its own sub-tab or stacked with the Lissajous.
- Whether ADC-continuous DMA activity measurably perturbs the existing
  `FskTimer` ISR's jitter in practice — nothing in the ESP-IDF docs
  guarantees isolation between them; needs bench verification once both
  exist in the same firmware image.
- Not yet started in code (beyond the `tune.*` config fields) — this
  whole section is design/research only.

## Planned feature: RTTY receive decoder (confirmed 2026-09-02, not started)

User's idea (2026-09-02): reuse the same ADC audio input as the tuning
indicator to actually **decode** received RTTY text (not just show a
tuning display), using the already-configured baud rate and
`tune.markHz`/`spaceHz`. Decoded text would show on the web UI and on
the LCD's second line — the same "live text" line already used for TX
text — switching meaning by state: **TX text while transmitting, RX
decoded text while idle/receiving.** This is the natural next step after
the tuning indicator/waterfall and shares almost its entire front end.

### ADC input-level meter and overload warning (confirmed 2026-10-13)

- **Implemented for the live ADC meter:** GPIO34 is sampled through the
  existing firmware's 20kHz ADC DMA path. A 400-sample window (20ms)
  estimates the peak audio amplitude around that window's measured DC bias;
  the result is normalized to the 12-bit ADC full-scale amplitude and
  sent to connected browsers as a `type: "adc"` WebSocket update every
  100ms. A raw sample within 12 counts of either ADC rail marks that
  window clipped. The web UI shows unavailable/stale data distinctly,
  maps the level onto the bar, and shows a prominent overload warning.
- The RTTY decoder, Lissajous, and FFT/waterfall visuals elsewhere on
  this page are still demonstration previews; the level meter is the
  first live view of GPIO34 samples. The physical 9-LED MCP23017
  bargraph/overload blink remains unimplemented because the MCP23017
  driver and hardware path are not yet present.
- The web UI view that presents RTTY decoding and the FFT/waterfall must
  also show a live **ADC input-level bar**. Its purpose is to let the
  operator set the receiver's audio level high enough for useful
  reception without overdriving the ESP32 ADC.
- Show the level as a percentage of the usable ADC input range, with a
  **5% reference marker** for the approximate no-signal RX noise level.
  The operator can adjust the RX audio level so the noise floor sits
  around that marker. This is a setup reference, not a universal
  calibration: actual noise and analog-front-end gain vary by receiver
  and hardware.
- Color the upper end of the bar **red** to make near-full-scale input
  obvious. The meter must also detect ADC overload/clipping from samples
  near the ADC limits; merely reaching the top of a smoothed display is
  not enough to establish clipping.
- On overload, visibly flag the condition in the web UI and override
  the physical 9-LED TUNE bargraph so **all nine LEDs blink together**,
  toggling at 500ms intervals, until the ADC is no longer overloaded.
  This overload warning takes priority over the bargraph's normal
  tuning indication.
- The level meter and clipping detector consume the existing GPIO34 ADC
  sample stream; they must not add another ADC sampling path. The
  implementation should use a short rolling measurement window so the
  displayed level is stable while clipping remains promptly detectable.

### Manual receive monitor / TUNE selection (2026-10-14)

- `rxMonitorMode` is a persisted `rtty`/`cw` choice independent of
  `uart2Mode`. Selecting either receive-monitor tab or changing the
  Configuration control selects that decoder/display mode and the matching
  planned physical TUNE LED interpretation. UART2 continues to select only
  its RTTY-input or Winkey-CW role; it does not force the receive monitor.
- The setting is synchronized to web clients through the existing status and
  heartbeat messages. The nine physical TUNE LEDs still cannot follow it:
  their MCP23017 driver and DSP/tuning pipeline have not yet been implemented.
- The CW monitor currently provides the UI and input-level display only.
  CW spectrum, pitch detection, automatic WPM synchronization, and decoded
  receive text remain unavailable until the receive DSP is implemented.

### Technical findings (verified via web research, not assumed)

- **Standard demod approach — confirmed as the right model**: for two
  known tones with continuously-available magnitude, the classic
  approach is a **tone comparator** (whichever of Mark/Space has larger
  magnitude right now is the current bit) plus **edge-triggered clock
  recovery** (detect the mark→space transition into a start bit, then
  sample the comparator at the nominal bit-period intervals). This is
  the textbook algorithm behind classic software decoders such as Jesús
  Arias's Linux `rtty` (https://www.ele.uva.es/~jesus/rtty/rtty-2.1.pdf).
- **Meaningfully better variant for noisy HF signals — adopted as the
  plan**: rather than sampling the comparator once at mid-bit,
  **oversample and vote/integrate** across several comparator readings
  spanning each bit period, and **resync the bit-phase counter at every
  detected start-bit edge** (and again at the stop bit) instead of
  free-running an independent RX clock. This is described in detail in
  the TAPR DCC 2014 paper "A Radioteletype Over-Sampling Software
  Decoder for Amateur Radio" (K0JJR):
  https://files.tapr.org/meetings/DCC_2014/DCC2014-Radioteletype-Over-Sampling-Decoder-K0JJR.pdf
  — it runs a Goertzel tone detector ~5-8 times per bit period and
  votes across the bit-interior samples. Since the continuous I/Q
  magnitude stream already planned for the Lissajous *is* effectively a
  continuous Goertzel/synchronous detector, adopting this means
  widening the existing mid-bit sample into a short averaging window
  rather than adding new machinery — cheap, and gives real noise
  immunity over naive point-sampling.
- **Existing references confirming the approach is proven on weak
  hardware** (none are ESP32-specific RTTY *decoders*, but together
  confirm the algorithm class is viable well below ESP32's power):
  an AVR-class Arduino proof-of-concept using FFT tone detection +
  `delay()`-based start-bit/mid-bit timing and a Baudot lookup table
  (https://forum.arduino.cc/t/arduino-rtty-decoder/82177 — author notes
  it's PoC-only with some decode errors, i.e. a lower bar than what the
  oversampling/voting approach above should achieve); an interrupt-
  driven Arduino RTTY *modulator* for reference on the encode side
  (https://github.com/kg4sgp/arduino-rtty); an ASCII→Baudot encoder
  (https://github.com/ok1hra/serial2fsk); general AFSK/RTTY protocol
  notes from `minimodem` (https://mintlify.wiki/kamalmostafa/minimodem/protocols/rtty).
- **Task-context timing is adequate — no ISR needed for RX.** The bit
  decision comes from the magnitude values themselves (computed
  precisely and continuously from I/Q, independent of when a task
  happens to run), not from exact interrupt timing — scheduling jitter
  only affects *which* magnitude sample gets read, not the underlying
  signal's phase reference. At 45.45-75 baud, bit periods are
  13.3-22ms; typical FreeRTOS task jitter on ESP32 is a small fraction
  of that, especially once mid-bit sampling is widened to an averaging
  window (point above) that absorbs a few ms of slop for free. **This
  means the whole decoder lives in ordinary task context on core 0,
  never touching the timing-critical `FskTimer` ISR or `TxManager` task
  on core 1** — same isolation property as the Lissajous/waterfall.

### Reference implementation: WIFILT `rtty-codec.js` (researched 2026-09-27)

User pointed at [ok1hra/wifilt](https://github.com/ok1hra/wifilt), an
ESP32/native web interface for Icom LAN transceivers by the same author as
`serial2fsk` already cited above, and asked whether its RTTY spectrum display
and decoder contain anything worth adopting. The project's `SOFTWARE.md` and
RTTY demodulator source, `data/rtty-codec.js`, were reviewed. That source
credits horusdemodlib/Project Horus for the Goertzel, bit-sync, and
continuous-phase technique. The decoder-side findings below are adopted;
WIFILT's AFC/AUTOTUNE feature is explicitly declined because it is not needed
for this design.

- **Sliding-window Goertzel instead of block-by-block processing.** WIFILT
  uses a 188-sample window (23.5ms) re-evaluated every 8 samples rather than
  every 188, so tone magnitudes update continuously, hop by hop. This is
  directly applicable to our I/Q-mixer pipeline in the `TuneMonitor`
  architecture above, which is already continuous by construction, and
  therefore confirms that choice rather than changing it. WIFILT also sizes
  the window so the Mark/Space separation falls on an exact null of the other
  tone's filter response (its 170Hz shift is exactly four bins at 188
  samples). The same technique should be applied when sizing our I/Q
  low-pass/window against `tune.shiftHz`, instead of choosing an arbitrary
  round window length.
- **ATC (Adaptive Threshold Control), adopted instead of a plain magnitude
  comparator.** Rather than selecting whichever of Mark or Space is larger at
  the current instant, WIFILT tracks each tone magnitude through an envelope
  follower with asymmetric attack and decay: fast attack at approximately
  one quarter of a bit period and slow decay over several bit periods. It
  subtracts an estimated noise floor before comparing the tones. This is
  adopted for our tone comparator because it should be substantially more
  robust to fading/QSB than a bare magnitude comparison and is only a small
  addition to the I/Q magnitudes already being computed.
- **Two simultaneous decoder variants (DEC1/DEC2), adopted as an optional
  refinement.** DEC1 samples the ATC decision value at the exact bit midpoint;
  DEC2 integrates or averages it over the middle approximately 70% of the bit
  for additional noise immunity. WIFILT displays both side by side so the
  operator can compare copy quality. This maps directly to our K0JJR-derived
  plan to oversample and vote across the bit interior: DEC2 is that plan and
  DEC1 is the simpler fallback. Both are worth implementing because DEC1 is a
  useful real-hardware sanity check against DEC2, not merely an alternative.
- **DPLL/flywheel for continuous traffic, adopted as an enhancement rather
  than a requirement.** For uninterrupted text without a clean gap between
  characters, WIFILT predicts the next start-bit edge exactly 7.5 bit periods
  after the previous one and closes on that prediction when no explicit
  mark→space edge is detected. This preserves decoding through the low-value
  stop-bit region without waiting for a transition that may not arrive
  cleanly. It complements rather than replaces the existing plan to resync at
  every detected start-bit edge; the flywheel fills in only when an edge is
  missed.
- **Squelch by SNR rather than absolute level, adopted as the squelch model.**
  A smoothed `|markMag - spaceMag|` signal proxy versus
  `min(markMag, spaceMag)` noise proxy produces an SNR estimate with roughly
  1dB hysteresis to prevent threshold chatter. This has the same shape as the
  already-decided Signal Quality calculation
  (`SNR_dB = 20*log10((magM+magS) /
  max(magTotal-(magM+magS), floor))`), but gates the decoder instead of only
  driving a display. The decoder should therefore consume the existing Signal
  Quality SNR value rather than calculate a separate second estimate.
- **AFC/AUTOTUNE considered and explicitly declined (2026-09-27).** WIFILT's
  S&P-only AUTOTUNE mode (Alt+T) nudges the radio's actual dial frequency to
  align a received signal with the configured markers after five consistent
  measurements over at least two seconds. That is genuine automatic
  frequency control, which the Honest limitation below identifies as absent.
  This design deliberately continues to use the Lissajous display and LED
  bargraph for manual operator tuning. The limitation therefore remains an
  accepted scope boundary rather than a future gap to close.
- **Not applicable here, recorded for completeness.** WIFILT receives audio
  over the network from an Icom radio rather than through a local ADC, so its
  sample chain, waterfall renderer, and UI framework do not transfer to our
  GPIO34 ADC continuous-mode pipeline. Only the demodulation algorithm —
  Goertzel, ATC, bit sync, DEC1/DEC2, DPLL, and SNR squelch — is adopted, not
  its sampling or transport code.

### Twin Peak Filter — considered, not pursued (2026-09-03)

User asked whether a "Twin Peak Filter" (the narrowband dual-bandpass
audio filter found on Icom rigs — IC-7300/7600/9700/705/756PROIII —
for RTTY reception) could be implemented for the decoder. Researched
before answering (not assumed):

- **What it actually is, confirmed via research**: two narrow bandpass
  filters centered exactly on the Mark and Space tones, rejecting
  everything else — historically called a "comb filter" / "dual peak
  filter" (Gaudé, 1963) before the modern "Twin Peak Filter" name.
  Sources: [rttycontesting.com](https://www.rttycontesting.com/lagniappe/icom-ic-756-pro-iii/twin-peak-filter/),
  [W7AY — RTTY Demodulators](http://www.w7ay.net/site/Technical/RTTY%20Demodulators/).
- **Verdict: already effectively what this design does.** The
  already-planned I/Q synchronous detection on `markHz`/`spaceHz` (see
  the `TuneMonitor` architecture above) *is* the digital equivalent —
  two narrowband detectors centered on Mark/Space, rejecting
  everything else, feeding the tone comparator. Not a new feature to
  add on top of the decoder; it's the same principle the design
  already uses.
- **Caveat surfaced during research, worth having on record**: W7AY's
  page cites Gaudé's 1963 finding that this narrowband-filter approach
  can *increase* distortion under selective fading versus a plain
  limiter-discriminator (37ms/7ms vs. 25ms/19ms) — part of why modern
  software decoders lean on synchronous/oversampled detection (our
  K0JJR-paper-based plan, see above) rather than a sharper standalone
  filter.
- **Decided (2026-09-03): not pursued as a separate feature** — user
  agreed the existing I/Q pipeline already covers it; no additional
  pre-filter stage added to the design.

### Honest limitation (not a research gap, a real property of this design)

This decoder has **no automatic frequency control (AFC)** — it looks
for energy at exactly `cfg.tune.markHz`/`spaceHz` and nothing else.
Professional RTTY software (fldigi, MMTTY, etc.) actively tracks small
drift/mistuning; this design instead leans on the **Lissajous tuning
indicator (and now also the LED bargraph, see below) to get the
operator's dial tuning exactly right first**, after which the
fixed-frequency decoder should work well. This is a deliberate scope
choice, not an oversight — adaptive frequency tracking would be a
substantial future enhancement, not part of this first version.

Reconfirmed 2026-09-27: WIFILT's AUTOTUNE implementation would be the natural
reference if real AFC were added later, but the user explicitly decided
against it. This remains a deliberate scope boundary, not a TODO.

### Proposed architecture sketch (not yet implemented)

- Lives inside the same `TuneMonitor` module/task (core 0) as the
  Lissajous/waterfall/bargraph — a fourth consumer of the same ADC/I-Q
  pipeline, not a new task or new hardware.
- **Tone comparator + oversampled bit sync**: at each I/Q update, note
  which of Mark/Space currently has larger magnitude. On a mark→space
  transition, start a bit-phase counter (using `cfg.baudRate` — the
  *same* baud-rate setting already used for TX, on the assumption that
  a station transmits and expects to receive at one fixed configured
  speed). For each subsequent bit slot (5 data bits + the fixed 1.5
  stop bits, mirroring the TX side's `STOP_BIT_HALF_PERIODS`/USOS
  handling in `Baudot.h`), average/vote the comparator over a window
  centered on the bit's midpoint rather than a single instant. Resync
  the phase counter again at the next detected start-bit edge, so small
  drift never accumulates across characters.
- **Baudot → ASCII decode**: a new `baudotToAscii` table (inverse of
  the existing `Baudot::asciiToBaudot`) plus RX-side LTRS/FIGS shift-
  state tracking — structurally the mirror image of the already-built
  TX-side `SendBuffer` shift-state logic in `Baudot.cpp`, so this is
  known, symmetric work rather than a new algorithm.
- **Own-TX handling**: while `TxManager` reports an active TX, the
  decoder's output is not shown on the shared LCD line (which is
  already busy showing the outgoing TX text) — whether the decoder
  itself keeps running in the background during TX (for a possible web
  RX log) or is paused entirely is **not yet decided**; pausing is
  simpler and avoids any risk of the decoder mistaking sidetone/
  transmitted audio bleed for a received signal.
- **Display**:
  - **LCD**: extends the existing `StatusDisplay` live-text line
    (`resetTxLine()`/`appendTxChar()`, today TX-only) to also accept
    decoded RX characters when idle — one line, meaning switches with
    TX/RX state, exactly as the user described ("při vysílání TX text,
    při RX by to byl RX dekódovaný text").
  - **Web**: a new WS field/message carries decoded RX characters,
    naturally paired with the existing `status`/live-TX-text push
    architecture — likely the same "Live transmitted text" area on the
    Status tab, similarly switching meaning with state, or a small
    separate "RX text" panel if keeping TX/RX visually distinct reads
    better. **Not yet decided which.**
- **Config**: the current firmware has no `tune.*` fields. The future
  decoder is expected to use the existing top-level `baudRate` plus
  planned `tune.markHz`/`spaceHz`; whether it needs its own enable flag
  separate from the planned `tune.enabled` setting (e.g. Lissajous on
  but decoding off) is **not yet decided**.

### Not yet decided (RTTY decoder)
- Whether RX decoding pauses entirely during own TX, or keeps running
  silently in the background for a web-only RX log.
- Whether decode gets its own enable flag or shares `tune.enabled`.
- Exact web display treatment (shared "live text" area vs. a separate RX
  panel). The WIFILT reference suggests a concrete option: dual DEC1/DEC2
  columns shaded by per-character signal strength. It is not yet decided
  whether to adopt that display treatment or only its underlying algorithm.
- **Narrowed 2026-09-27:** adopt ATC as the adaptive-threshold tone decision
  and implement both DEC1 (mid-bit sample) and DEC2 (middle approximately 70%
  bit average, corresponding to the K0JJR oversample/vote idea), rather than
  choosing only one. The exact DEC2 averaging-window width and ATC
  attack/decay constants still require bench tuning with a real signal.
- Whether a short RX text history (not just the current line) is worth
  keeping/scrolling on the web page — the LCD obviously can't do this,
  but the web page isn't limited to one line.
- **Resolved 2026-09-27: no AFC/AUTOTUNE.** WIFILT implements it, but the user
  explicitly decided this design does not need it; it is no longer open.
- Not yet started in code — this whole section is design/research only,
  same status as the Lissajous/waterfall section above.

## Planned feature: LED bargraph tuning indicator (confirmed 2026-09-02, not started)

User's idea (2026-09-02): add a physical, discrete-LED bargraph as a
hardware tuning aid, complementing the web Lissajous/waterfall with
something visible without opening a browser — the kind of tuning eye
classic hardware RTTY terminal units had built in. Originally proposed
as 10 LEDs, then **refined to 11 LEDs (2026-09-02)** — an odd count so
there's a genuine, unambiguous center LED representing "tuned," rather
than two adjacent center LEDs with no single unambiguous position. The
user's own framing was that this "would require FFT to run continuously
during receive," and asked whether an I2C GPIO expander (MCP23017) or a
shift register (74HC595) is the right way to drive the LEDs.
**Driver chip decided (2026-09-02): MCP23017** — see the hardware
section below for the full comparison and rationale.

**LED count revised again, later the same day (2026-09-02): 9 LEDs**,
down from 11 — freeing MCP23017 pins to make room for three new
encoder mode-select indicator LEDs (RX volume / TX volume / CW speed —
see the dedicated section below) rather than growing the total pin
commitment further. Still an odd count (center + 4 either side), so the
"genuine, unambiguous center LED" property is unchanged, just with
slightly coarser resolution on either side of center than the earlier
11-LED plan.

### Key finding: a full running FFT is likely NOT required

Two genuinely different designs answer "put an LED bargraph on the
tuning signal," and they have very different DSP costs — worth
separating before picking hardware:

- **Option A — two-tone discriminator bargraph (classic design, no FFT
  needed).** This is how most real hardware RTTY terminal units built a
  bargraph/tuning-eye historically (and is not a compromise — it's the
  standard approach, not a simplified one): derive a single position
  value each update from the *already-planned* Mark/Space I/Q magnitudes
  — e.g. `position = markMagnitude - spaceMagnitude` (or a normalized
  ratio) — and light LEDs outward from the center LED toward one end or
  the other depending on which tone currently dominates and by how much.
  Correct tuning (both tones present and balanced in the classic
  "mark-space crossover" sense, or one cleanly dominant during its bit,
  per the existing tone-comparator logic) settles the lit LED near
  center; drifting off-frequency skews it toward one end. This is
  **literally the same I/Q data already being computed for the
  Lissajous and the RTTY decoder** — the bargraph becomes a fourth
  consumer of the existing pipeline, at near-zero extra CPU cost (a
  subtraction/ratio and a lookup from magnitude to "how many LEDs lit,"
  done once per I/Q update, ~30-50 times/sec). No FFT, no new sampling
  requirement, nothing beyond what's already planned for the Lissajous.
- **Option B — spectral-position bargraph (genuinely needs frequency
  bins).** If instead the intent is to show *where in the audio
  spectrum* the actual received energy sits — i.e. a compressed,
  segment version of the waterfall, useful for spotting nearby QRM or
  a station that's drifted outside the expected passband rather than
  just "is my tuning centered" — that does need multiple frequency bins,
  and there are two ways to get them, in cost order:
  - If the **waterfall is also being built**, the FFT it already
    computes covers this for free — just resample ~9 of its existing
    bins (spanning the mark/space region) down to 9 LED brightness/on-
    off values once per FFT block (~20/sec per the waterfall sizing
    above). No new DSP.
  - If the waterfall is *not* built (or the bargraph should work
    independently of it), a **small bank of ~9 Goertzel filters**
    tuned to 9 fixed frequencies spanning the mark/space region is
    cheaper than a full FFT (Goertzel cost scales with the number of
    bins you actually want, not with `N log N` over the whole
    spectrum) — a genuine "FFT-lite" tailored to exactly this use case,
    not full continuous FFT.
- The user's own description — LEDs with the **center one representing
  "tuned"** — is naturally satisfied by **either** option, and works
  cleanly with 9 (odd) LEDs: Option A's center LED represents "tones
  balanced" (which sits conceptually at the midpoint when mark/space
  energy is equal), Option B's center LED would represent an actual
  frequency bin at the numeric midpoint between Mark and Space. Either
  way, the odd count gives exactly one LED that unambiguously means
  "locked on," with 4 LEDs available on each side to show degree/
  direction of mistuning (down from 5 each side in the earlier 11-LED
  plan — a modest resolution trade for freeing 2 pins).
  Option A is recommended as the default/first build (reuses existing
  pipeline, zero new DSP, matches classic hardware practice) unless the
  user specifically wants the "see nearby interference" spectral
  behavior that only Option B provides. **Not yet decided which the
  user wants** — this is independent of the MCP23017 hardware choice,
  since either option feeds the same LED driver, just with different
  numbers to write.

### Hardware: MCP23017 (I2C) — decided 2026-09-02

**Confirmed by the user: MCP23017.** For reference, this is why it won
out over the two alternatives considered:

- **GPIO budget was the deciding factor, and it strongly favored I2C.**
  In the current firmware, GPIO15 is already UART2 TX, GPIO35 is the
  inhibit input, and GPIO36 is UART2 RX; GPIO34 is reserved by this
  design for future ADC audio input. GPIO12 is now assigned to the
  dedicated CW key output, with the constraint that it must be LOW at reset
  because it is the flash-voltage strap. There is, in practice, **no spare
  general-purpose output pin left on this board** for a shift register's 3
  control lines (data/clock/latch) without reclaiming an already-assigned
  signal. This ruled out
  **74HC595**, not because the chip is bad, but because this specific
  board's pin budget doesn't have room for it. (Cascading multiple
  74HC595s for >8 outputs doesn't help the pin problem — the daisy
  chain still needs the same 3 initial GPIOs.) The planned future
  confirmed encoder allocation for GPIO2/GPIO14 still needs firmware
  implementation. The
  MCP23017 decision for the bargraph stands regardless: I2C needs zero
  additional ESP32 GPIO pins.
- **MCP23017 needs zero additional GPIO pins** — it sits on the
  already-existing I2C bus (SDA=GPIO32/SCL=GPIO33), the same bus the
  status LCD's PCF8574 backpack is already on. 16 GPIO pins on one chip
  is comfortably more than the (now 9) bargraph LEDs need, with
  headroom for the other MCP23017-hosted indicators this doc adds
  below.
- **LM3914** (analog dot/bar driver) was also considered and would have
  needed no driver firmware at all, but its fixed 10-segment (decade)
  architecture doesn't cleanly extend to the odd, symmetric 11-LED
  display — a genuine bidirectional-from-center layout would have
  needed two LM3914s plus a separately-driven center LED, eroding its
  main appeal. MCP23017 handles 11 LEDs exactly as easily as it would
  have handled 10.

**Remaining open items for the MCP23017 implementation:**
- The MCP23017's address is set by 3 pins (A0-A2), giving 8 possible
  addresses (0x20-0x27 typical). The LCD's PCF8574 backpack commonly
  defaults to 0x27 (sometimes 0x3F) — the MCP23017's address pins must
  be strapped to avoid colliding with whatever the actual LCD module
  uses. **Not yet confirmed which address the LCD module in hand
  actually uses** — check with an I2C scanner sketch before finalizing
  the MCP23017's address strapping.
- Refresh rate is a non-issue: writing all 11 (or 16) I/O states is one
  or two register writes per update; even at the bargraph's natural
  ~20-50Hz update rate this is trivial I2C traffic, sharing the bus
  fine with the LCD's already-infrequent (per-character) writes — same
  "non-critical, task-context I2C" category already established for
  the LCD, nowhere near the TX-timing-critical path.
- **Current budget worth checking at board-design time**: MCP23017
  pins can source/sink on the order of ~25mA each, but the package has
  an overall current limit well below "25mA × 16" — fine if the
  bargraph mostly lights a handful of LEDs at once (dot mode, or a bar
  mode with modest per-LED current via series resistors, e.g. 3-5mA),
  worth a quick datasheet check if all 11 might be lit simultaneously
  at high brightness.
- **Pin budget updated 2026-09-02 (revised twice the same day), then
  again 2026-09-03**: the MCP23017 now hosts:
  - **9** bargraph LEDs (down from 11, see above),
  - **1** `LED_RX_PIN` (RX-activity indicator, moved off direct GPIO2),
  - **3** encoder mode-select LEDs — `ENC_SEL_RX_LED`/`ENC_SEL_TX_LED`/
    `ENC_SEL_CW_LED` (RX volume / TX volume / CW speed — new, see the
    dedicated section below),
  - **1** encoder pushbutton (mode-cycle button — now decided as needed,
    see the encoder-GPIO subsection below),
  - **1** `MODE_SEL_PIN` (new 2026-09-03, corrected same day — NOT an
    LED, see below — a hardware mode-select output: LOW for RTTY, HIGH
    for CW/Winkey — see the "CW keying via Winkey protocol emulation"
    section below).

  **15 of 16 pins committed, 1 spare.** The digipot-trim encoder's
  quadrature A/B lines are on **direct ESP32 GPIOs** instead (`ENC_A_PIN`/
  GPIO2, `ENC_B_PIN`/GPIO14, PCNT-decoded — see the GPIO mapping section),
  so they don't draw from this budget. The RX/TX digipot-switch-position-
  read line item from the earlier plan is now moot — see the encoder
  mode-select section below, which replaces the manual switch with
  firmware-cycled selection entirely.

### Not yet decided (LED bargraph)
- **LED count: decided — 9** (confirmed 2026-09-02: 10 → 11 → 9 over
  the course of the day), specifically so there's a single unambiguous
  center "locked" LED, now with 4 either side (down from 5, to make
  MCP23017 room for the new encoder mode-select LEDs below).
- **Driver chip: decided — MCP23017** (confirmed 2026-09-02, see
  hardware section above).
- Option A (discriminator/balance) vs. Option B (spectral-position)
  behavior — see above; doesn't block firmware/hardware bring-up but
  does determine exactly what value gets written to the MCP23017 each
  update.
- MCP23017 I2C address strapping vs. the LCD's actual PCF8574 address
  (needs an I2C scan on real hardware, not yet done).
- LED current/resistor values and whether "dot" mode (one LED lit) or
  "bar" mode (all LEDs up to the current level lit) is wanted — bar mode
  draws more total current and reads more like a classic S-meter.
- Whether the bargraph runs continuously (RX and TX both) or only when
  not transmitting, mirroring the still-open TX-handling question for
  the RTTY decoder above.
- Not yet started in code or hardware — design/research only, same
  status as the other `TuneMonitor` features above.

## Planned feature: encoder mode-select LEDs — RX volume / TX volume / CW speed (confirmed 2026-09-02, not started)

User's idea (2026-09-02): add **3 LEDs on the MCP23017**, one per target
the digipot-trim rotary encoder can adjust, with exactly one lit at a
time to show which target is currently active: **RX volume**, **TX
volume**, and **CW speed** (the user's own words: "pozdější využití" —
reserved for later use, not a scoped feature yet). This sits alongside,
and is named distinctly from, the existing `LED_RX_PIN` (the unrelated
RX-*activity* indicator LED already on the MCP23017) — to avoid any
naming confusion between "currently receiving" and "encoder currently
adjusts RX volume," these three are named `ENC_SEL_RX_LED`,
`ENC_SEL_TX_LED`, `ENC_SEL_CW_LED`.

- **RX volume / TX volume** map directly onto the two digipot channels
  already planned in the section below (RX audio trim feeding GPIO34,
  and the still-not-fully-specified "TX audio" trim) — selecting one of
  these two modes is exactly "which pot the encoder is adjusting" from
  the original digipot design.
- **CW speed will eventually adjust a speed value (confirmed 2026-09-02,
  by the user, in this same follow-up)** — direction confirmed ("bude
  nastavovat rychlost"), exact mechanism/range/where-it's-used still
  **deferred to a later design pass** ("dořešíme později"). Still true
  as before: no digipot, no firmware behind it yet; selecting this mode
  today just lights its LED and the encoder's rotation is not consumed
  by anything.
- **Mode selection is button-cycled, not a manual switch, and switches
  BOTH the indicator LED AND the active control loop (confirmed
  2026-09-02, by the user, in this same follow-up)** — with 3 (and
  possibly more later) targets sharing one encoder, a **short press of
  the encoder's pushbutton** cycles RX volume → TX volume → CW speed →
  RX volume → ..., and each press does two things atomically: (a) lights
  the corresponding `ENC_SEL_*_LED`, and (b) re-points the encoder's
  rotation handler at the matching control loop — the loop that reads
  PCNT counts and issues the actual RX-pot or TX-pot I2C write. These
  are explicitly **not independent** — the LED is a direct reflection of
  which loop is live, never just a label with its own state. This
  resolves the earlier open "does the encoder need a pushbutton"
  question from the encoder-GPIO section above: **yes, decided
  2026-09-02** — its purpose is mode-cycling, not a plain
  click-to-confirm action. The pushbutton itself still lands on a spare
  MCP23017 pin (see the encoder-GPIO subsection below and the updated
  MCP23017 pin budget above), same as already anticipated.

### Architectural implication: this supersedes the manual DPDT switch plan

The **"Proposed hardware solution: let the RX/TX switch double as the
I2C bus router"** section below was designed around a **manual,
2-position physical switch** the operator flips by hand. A 3-way,
button-cycled, firmware-driven selection (as just described) is a
fundamentally different control — there's no natural physical-switch
equivalent of "third position does nothing electrically" (CW speed has
no digipot behind it at all), and mixing a manual switch for 2 positions
with a firmware mode for a 3rd would be an awkward, inconsistent UI.

**This effectively rules out the manual-DPDT-switch + 2×MCP4017 plan as
originally conceived.** The two options that remain fully compatible
with button-cycled, firmware-driven selection are, in the order already
discussed below:
1. **Single dual-wiper digipot** (AD5243 or MCP4651, see the
   "Alternative hardware solution" section below) — firmware just
   writes to whichever internal channel (RX/TX) is currently selected;
   the CW-speed mode simply performs no I2C write at all. No switch, no
   mux, cleanest fit for this control model.
2. **Two MCP4017s behind a firmware-controlled I2C mux** (TCA9548A/
   PCA9548A, mentioned as an alternative in the same section below) —
   firmware selects the mux channel before writing, same "CW speed does
   nothing" behavior, but with an extra chip versus option 1.

**Not yet decided which of these two the user wants** — but the plain
mechanical-switch approach (this section's original "recommendation")
is now superseded by this same-day design change and should not be
built as originally written. Flagging clearly since it changes which
parts get ordered for the digipot BOM.

### LCD feedback while trimming RX/TX level or CW speed (confirmed 2026-09-02, extended 2026-09-03)

**Current implementation note:** the encoder, digipots, and transient
LCD readout described here are not implemented. `cwSpeedWpm` is a
persisted configuration default and Winkey host speed commands update
the running CW engine only; neither path currently drives an encoder
readout on the LCD.

User's idea: while the encoder is in RX-volume or TX-volume mode, the
LCD's bottom line should show the current level as **0-100%** — a
quick numeric readout of where the wiper sits, without needing the web
UI open. **Extended 2026-09-03** to the encoder's third mode too: while
in CW-speed mode, the same line shows the current CW speed the same
way. This also resolves the CW/Winkey section's previously-open
question of whether the encoder's CW-speed mode does anything —
**confirmed: yes**, turning the encoder in that mode adjusts a local
speed value with the same LCD feedback pattern already built for
RX/TX level (see the open question below on exactly how that value
feeds into Winkey's own speed setting).

- **Percentage source (RX/TX)**: the currently-selected digipot's wiper
  position, scaled to 0-100% of its full range. The exact step count
  depends on which digipot part is finally chosen (see the "Alternative
  hardware solution" section above — **not yet decided** between
  AD5243/MCP4651, 256/257 steps, or a mux'd pair of MCP4017s, 128 steps)
  — the percentage math (`wiper * 100 / maxSteps`) is the same either
  way, just the divisor changes.
- **Speed source (CW), added 2026-09-03**: a new `cwSpeedWpm` value,
  range 5-99 to match Winkey's own speed range (`<02><nn>` in the
  protocol — see the CW/Winkey section above), adjusted by encoder
  ticks while in CW-speed mode. Step size per detent not yet decided
  (same open item as the RX/TX encoder step-size question below).
- **Format: confirmed 2026-09-02, CW speed format added 2026-09-03** —
  `RX LEVEL: 67%` / `TX LEVEL: 42%` / `CW SPEED: 25WPM` (all three fit
  comfortably within the 16-char line; the label is deliberately
  8 characters in all three cases for visual consistency).
- **This is now a FOURTH consumer of the LCD's bottom "live text"
  line** (RX LEVEL/TX LEVEL/CW SPEED count as one state for arbitration
  purposes — only one of the encoder's three modes is ever active at a
  time), which already switches meaning between TX text (while
  transmitting) and RX decoded text (while idle/receiving, per the
  RTTY decoder section above). Adding "currently trimming a
  level/speed" needed an explicit priority/arbitration rule rather than
  an ad hoc one — **confirmed as future behavior on 2026-09-02,
  extended 2026-09-03; not implemented**:
  1. TX active → TX text (existing rule, unchanged — highest priority;
     in CW mode this is the CW-sent text, per the CW/Winkey section's
     LCD-display bullet — same slot, same priority).
  2. **A change to the readout value** — the encoder actively turning
     in any of its three modes (RX volume, TX volume, or CW speed), or
     a future host-driven CW speed change — switches the line to the
     level/speed readout; **revert to the normal RX/TX text 2 seconds
     after the last change** (user-confirmed exact value, replacing the
     earlier "~1.5-2s, not yet confirmed" placeholder). A host-driven
     speed change while actively transmitting still waits behind tier 1
     like everything else — it becomes visible once TX/keying pauses,
     same as an encoder-driven level change already would.
  3. Otherwise (idle, no recent change) → RX decoded text (if the RTTY
     decoder is built) or blank, exactly as already planned.
  The 2s auto-revert means the readout doesn't permanently occupy the
  line just because the operator left the encoder sitting in one of its
  modes without actually turning it — it appears on any change and
  clears itself 2s later.
- **Firmware ownership**: extends `StatusDisplay`'s existing shared
  live-text-line mechanism with a third writer (alongside `TxManager`
  and the future RTTY decoder) — the three-way priority above
  (TX text > level/speed readout on change, 2s hold > RX text/
  blank) is now the confirmed spec to implement, not just a proposal.
- **Resolved 2026-09-03 — `cwSpeedWpm` is live and bidirectional, not
  display-only.** `cwSpeedWpm` is the single value the CW keying engine
  actually uses, with two writers:
  - **Encoder** (planned): ticks in CW-speed mode would set the running
    CW speed; the encoder hardware and handler do not exist yet.
  - **Host, via Winkey's own speed command** (`<02><nn>`, see the
    CW/Winkey section above): an explicit `nn` in 5-99 changes the
    running CW speed; `nn == 0` ("speed from potentiometer") leaves the
    running speed unchanged. The host command does not overwrite the
    persisted `cwSpeedWpm` default. No transient LCD readout is currently
    triggered by this command.

### Also shown on the web UI (confirmed 2026-09-02, later the same day)

**Not implemented yet:** the read-only level fields and live WebSocket
updates below depend on the future digipot/encoder hardware.

User confirmed the RX/TX level percentage should also appear in the
web interface, not just the LCD.

- **Live push, not just a static config value**: extends the existing
  WebSocket `status` message (see "Web UI: routes and JSON shapes"
  below) with two new fields, e.g. `"rxLevelPct":67,"txLevelPct":42` —
  pushed on every wiper change exactly like every other `status` field
  already is, so a connected browser sees the level update in real time
  as the encoder turns, mirroring the LCD behavior. No 2s-hide logic is
  needed on the web side — unlike the LCD's single shared line, the web
  page has room to show both levels permanently (see placement below),
  so there's nothing to arbitrate.
- **Placement**: shown on the **Configuration** page, in a new small
  "RX/TX Levels" readout near the existing "Tuning indicator" section
  (both are `TuneMonitor`-adjacent hardware settings) — **read-only
  display**, not a web-based control; the only way to change the level
  is still the physical encoder. Whether a future version should also
  let the web UI *set* the level (e.g. a slider issuing digipot writes)
  is **not asked/not scoped** — today's ask is display-only.
- **Important related catch, surfaced by this change**: every digipot
  candidate discussed so far (MCP4017, AD5243, MCP4651) is **volatile**
  — it loses its wiper position on power-down (see the "power-holding"
  note in the digipot section below: it survives a bus disconnect, but
  not a power cycle). Once the level becomes something the user relies
  on seeing/trusting (LCD + web), an unannounced reset to an undefined
  wiper position after every power-up is a real usability problem, not
  just a cosmetic one. **Future behavior decided 2026-09-02 — persist
  and restore**: the last-set `rxLevelPct`/`txLevelPct` should be saved
  in `Config`/`config.json` (same LittleFS+JSON mechanism already used
  for other settings), and firmware should **write them back to the
  digipot's wiper at boot** (right after the digipot is initialized,
  before `TuneMonitor`/anything else touches it), so the trim survives
  a power cycle instead of resetting to an undefined position. This
  remains a future requirement; those fields are not yet in the current
  config or API.

## Planned feature: digital-pot (MCP4017) audio level trim for RX/TX (proposed 2026-09-02, not started)

User's idea (2026-09-02): add two **MCP4017T-103E** I2C digital
rheostats (10 kΩ, 128-step) as adjustable elements inside op-amp
stages — one trimming the RX audio path (feeding the already-planned
GPIO34 ADC front end), one for a "TX audio" path described as a
different use, not yet fully specified (see open questions). Both would
be adjusted by a **single rotary encoder**, with a **switch** selecting
which of the two digipots the encoder currently controls.

### Technical findings (verified via web research)

- **Part confirmed**: MCP4017 is a 7-bit (128-step), I2C, volatile
  digital rheostat, available in 5k/10k/50k/100k full-scale values.
  "-103" in the part number is the standard resistor code for
  10×10^3 Ω = 10 kΩ (matches "MCP4017T-103E": T = tape-and-reel, E =
  extended temperature grade). Sources:
  [MCP4017-19 Datasheet | Digi-Key](https://www.digikey.hk/htmldatasheets/production/547140/0/0/1/mcp4017-19.html),
  [MCP4018T-103E/LT | Microchip Direct](https://www.microchipdirect.com/product/MCP4018T-103E/LTY?samples=true),
  [MCP4017 Datasheet | RadioLocman](https://radiolocman.com/datasheet/data.html?%2FMCP4017=&di=425549).
- **Fixed I2C address, no address-select pins — confirmed: 0x2F for
  MCP4017.** Unlike the MCP23017 (which has A0-A2 address pins), the
  MCP4017 has none — its I2C address is hard-wired in silicon. Source:
  [SparkysWidgets SW_MCP4017 Arduino library](https://github.com/SparkysWidgets/SW_MCP4017-Library/blob/master/SW_MCP4017.h)
  (`#define MCP4017ADDRESS 0x2F`).
- **Checked whether the sibling parts (MCP4018/MCP4019) use different
  fixed addresses, hoping to sidestep the collision by mixing parts on
  the BOM — inconclusive, and the one concrete data point argues
  against it.** Microchip sells MCP4017/4018/4019 as pin/electrically
  similar siblings differing mainly in package/taper, which raised the
  hope that each might have a distinct fixed address (a documented
  trick with some digital-pot families). However, Marlin firmware's
  MCP4018 driver defines its address as the **same 0x2F** already
  confirmed for MCP4017 — see
  [Marlin digipot_mcp4018.cpp](https://github.com/Naesstrom/Marlin/blob/master/digipot_mcp4018.cpp)
  (`#define DIGIPOT_I2C_ADDRESS 0x2F`). This directly contradicts the
  "different address per sibling" hope. **Verdict: don't rely on mixed
  MCP4017/4018/4019 parts to dodge the address collision — verify
  against the current Microchip datasheet's address table before
  ordering anything on that assumption. The design below assumes the
  conservative case: both RX and TX chips would be at 0x2F and cannot
  be electrically present on the same active bus at once.**

### Proposed hardware solution: let the RX/TX switch double as the I2C bus router

**Superseded 2026-09-02 (later the same day) — kept for history, not the
current plan.** This section assumed a manual, 2-position physical
switch. Once the encoder gained a third mode (CW speed, button-cycled —
see the "encoder mode-select LEDs" section above), the whole premise of
a manual switch stopped fitting the control model. See that section's
"Architectural implication" for what replaces this: either the
single-dual-wiper-chip option or the I2C-mux option below, both
firmware-selected, no manual switch.

Since two 0x2F devices can't coexist on one bus, and the user already
wants a physical switch to pick "which pot the encoder is adjusting,"
the same switch can solve both problems at once:

- Use a **2-pole switch (DPDT)**: one pole routes **SDA**, the other
  routes **SCL**, to whichever MCP4017 is currently selected. The
  *other* chip's SDA/SCL are left disconnected while not selected —
  only one 0x2F device is ever electrically on the bus, so there's no
  address conflict, and firmware never needs to know which physical
  chip it's talking to: it always addresses 0x2F, and whichever chip
  is switched in receives it.
- I2C runs at ~100kHz and this is a manually-operated trim control, not
  a timing-critical signal, so a mechanical switch in the SDA/SCL path
  is electrically fine — keep the switch wiring short to avoid adding
  stray bus capacitance.
- MCP4017 is **volatile but power-holding**: it keeps its wiper setting
  as long as it's powered, even while its bus connection is physically
  disconnected (it only loses the setting on power-down, not on bus
  disconnect) — so switching away from one pot and back doesn't lose
  its trim.
- **Alternative**: a dedicated I2C bus mux (e.g. TCA9548A/PCA9548A)
  would let firmware select the active channel in software instead of
  mechanically — such mux chips have their own address-select pins, so
  no conflict there — extra chip and extra firmware (select the mux
  channel before every digipot write) versus the DPDT switch, but now
  that selection is firmware/button-driven anyway (see the mode-select
  LED section above), this option — or the single-dual-wiper-chip
  option in the section below — is what actually fits the current
  design, **not** the DPDT-switch recommendation this bullet originally
  ended on. **Superseded**: see the note at the top of this section.

### Alternative hardware solution: single dual-wiper digipot (researched 2026-09-02, not yet chosen)

User asked whether cheap I2C digipots exist with two independent
potentiometers in one package, avoiding the address-collision problem
at its root instead of routing around it with the DPDT switch above.
**Confirmed via datasheet research: yes, such parts exist and are not
expensive.**

- **Analog Devices AD5243** (dual, 256-position, volatile, available in
  2.5k/10k/50k/100k Ω, 10-lead MSOP): a genuine dual-wiper part on a
  **single fixed I2C address** (no address pins — only one AD5243 can
  be on a bus at a time). Each wiper is selected by one bit in the
  instruction byte (bit 7/A0: 0 = channel 1, 1 = channel 2), followed by
  the data byte with the wiper position — RX and TX levels become two
  independent writes to the same address, no bus switching needed at
  all. Sibling **AD5248** adds AD0/AD1 address pins (4 possible
  addresses) if more than one dual-pot chip is ever needed on the bus.
  Source: [AD5243/AD5248 Datasheet — Analog Devices](https://www.analog.com/media/en/technical-documentation/data-sheets/ad5243_5248.pdf).
- **Microchip MCP4651** (same datasheet family tree as the already-
  researched MCP4017 — part of the MCP453X/455X/463X/465X family):
  confirmed **dual-channel, volatile, 8-bit (257 positions**, vs.
  MCP4017's 128), I2C, available in 5k/10k/50k/100k Ω — so
  "MCP4651T-103E" would be the direct 10 kΩ dual equivalent of
  MCP4017T-103E. Each wiper (Wiper 0/Wiper 1) is a separate internal
  memory-mapped register, each with its own TCON (terminal control)
  register — independently writable/readable over I2C via different
  memory-address values in the command byte, all at the same slave
  address. Package: TSSOP-14. Sources:
  [MCP4651-503E/ST — RS Online](https://my.rs-online.com/web/p/digital-potentiometers/0598998),
  [MCP453X/455X/463X/465X Datasheet family — Microchip](https://ww1.microchip.com/downloads/en/DeviceDoc/22096b.pdf).

**Implication for this design**: either part could replace the two
MCP4017s + DPDT bus-router switch above with **one chip on one fixed
address**, controlling RX and TX trim independently via which internal
channel/register gets written — eliminating the address-collision
problem at its root rather than routing around it. The RX/TX selector
switch would then only need to be read as a plain GPIO input (telling
firmware which channel the encoder is currently adjusting), not used to
physically switch SDA/SCL at all. Trade-offs versus the two-MCP4017 +
DPDT-switch design:
- Loses the DPDT switch's hardware guarantee that only one pot is ever
  electrically live at a time — with a single dual chip, correctness
  depends entirely on firmware writing to the right internal channel
  (impossible to get wrong with the mechanical version).
- MSOP-10 (AD5243) or TSSOP-14 (MCP4651) are smaller/finer-pitch
  packages than MCP4017's SOT-23-6 — more demanding for hand assembly.
- Simpler BOM (one digipot chip instead of two) and simpler bus wiring
  (no switch in the SDA/SCL signal path at all).

**Updated 2026-09-02 (later the same day)**: the DPDT-switch + 2×MCP4017
option is now superseded (see the "encoder mode-select LEDs" section
above — selection is button-cycled/firmware-driven, not a manual
switch). The live choice is between this section's single dual-wiper
chip (AD5243 or MCP4651 — the simpler BOM, no extra chip) and the I2C
mux option (TCA9548A/PCA9548A, mentioned in the section above — extra
chip, same firmware-selection behavior). **Not yet decided which of
these two** the user wants.

### Encoder GPIO: decided 2026-09-02 — direct ESP32 pins (PCNT), superseding the MCP23017 plan

**Original problem (now resolved)**: per the GPIO budget as it stood
earlier, this board had zero free general-purpose GPIO, so the plan
below was to put the encoder's A/B (and optionally a pushbutton) on
spare MCP23017 pins, polled from firmware. That constraint no longer
holds: the confirmed hardware plan moves `LED_RX_PIN` to the MCP23017 and
removes `ON_PIN`, assigning OUT1/GPIO2 and CLK/GPIO14 to the encoder's
quadrature A/B lines (`ENC_A_PIN`/GPIO2, `ENC_B_PIN`/GPIO14 — see the GPIO
mapping table), intended for decoding by the ESP32's hardware **PCNT**
peripheral rather than polling over I2C. Encoder handling and the LED
migration are not implemented in firmware yet.

- **This is a strict upgrade over the MCP23017-polling plan**: PCNT
  decodes quadrature transitions in hardware, so the earlier "honest
  trade-off" (I2C polling can miss fast turns between polls) simply
  doesn't apply anymore — direct pins plus PCNT handle any turn speed a
  human can produce, with no polling-interval compromise and no core-0
  CPU time spent on it at all.
- **Pushbutton: decided 2026-09-02 (later the same day) — yes, needed.**
  There is no third free direct GPIO (both freed pins are spoken for by
  A/B), so it goes on a spare **MCP23017** pin, polled like any other
  slow digital input. Its purpose is now concrete: **short-press cycles
  the encoder's target mode** RX volume → TX volume → CW speed → ...,
  per the "encoder mode-select LEDs" section above — not a plain
  click-to-confirm action. Unlike quadrature, a pushbutton has no
  "missed step" failure mode from polling — a human press is
  milliseconds long, comfortably caught by the same 10-20ms poll
  cadence already planned for other MCP23017 reads.
- The RX/TX-selector-switch-position-read line item from earlier is now
  moot — there's no manual switch left to read a position from; mode is
  entirely tracked in firmware and reflected by the `ENC_SEL_*_LED`s
  instead (see the mode-select section above). See the updated
  MCP23017 pin-budget note above (14 of 16 committed, 2 spare) for the
  current full picture.

### Open questions
- **What exactly is the "TX audio" path?** Nothing in the design so far
  generates or uses audio for transmission — TX is direct FSK keying
  via `FSK_PIN` (GPIO5), not an audio tone. This request implies a new,
  not-yet-discussed feature: is this an audio-frequency AFSK-style
  output (for radios that take audio into a mic/data input rather than
  a direct FSK line — genuinely new tone-generation firmware, not just
  a level control), a sidetone/monitor output so the operator can hear
  their own keying, or something else? This determines whether the "TX
  MCP" is a simple level/gain trim (mirrors the RX case exactly) or
  needs real tone-generation firmware behind it (a materially bigger
  feature). **Not yet clarified — flagging before assuming either
  way.**
- **Exact placement of the digipot within "the operational amplifier
  with bias"** — three options, not yet confirmed which is intended:
  1. As the **feedback resistor** setting gain in the existing
     bias+filter+buffer op-amp stage already designed for GPIO34
     (recommended default: keeps the bias/filter network exactly as
     already documented, the digipot only trims gain, cleanly
     separating "how much audio gets in" from "where the DC operating
     point sits").
  2. As part of an **input attenuator** ahead of the stage (MCP4017 is
     a 2-terminal rheostat, so it needs a fixed resistor to ground
     alongside it to form an adjustable divider) — trims level before
     the signal reaches the bias/filter network at all.
  3. As part of the **bias divider itself** — not recommended, since
     adjusting gain would then also shift the DC bias point, entangling
     two things (signal level and operating point) that should stay
     independent.
- Whether RX and TX each need their own full bias/filter op-amp stage
  (two complete analog front-ends) or share one, switched — affects how
  much of the already-designed GPIO34 front-end circuit needs
  duplicating versus reusing.
- Encoder step size/acceleration (fixed step per detent vs. faster
  turns moving the wiper further) and whether the current trim level is
  shown on the LCD/web UI — not discussed yet.
- Not yet started in hardware or firmware — a fresh design/research
  topic, same "confirmed idea, nothing built" status as the other
  `TuneMonitor`-adjacent features.

## CW keying via Winkey protocol emulation (confirmed 2026-09-03, implemented 2026-09-27/28)

**Status 2026-09-28:** implemented and host-tested, not yet tested against
real logging software or hardware. The "shared UART0, mode-switched"
decision below was superseded by the UART1/UART2 split. The protocol-level
details are in `src/WinkeyEmulator.cpp`, and the README summarizes what the
emulator supports.

User's idea (2026-09-03): add CW (Morse) transmit capability, controlled
by N1MM/a logger the same way it already controls real K1EL Winkey
hardware — i.e. emulate the Winkey serial protocol rather than inventing
a new one, so existing logging software works unmodified. Researched
before designing (K3NG keyer, the official Winkey protocol, and this
board's own existing serial link) — see below — then the architecture
was narrowed down through a couple of quick confirmations from the user
into a dedicated CW output on GPIO12 (confirmed 2026-10-20), rather than
sharing the RTTY output.

### UART1/UART2 split (decided and implemented 2026-09-27)

- UART0/J1 (`Serial`, called "UART1"/"FSK1" in the product) stays the
  always-on RTTY control link at 9600/8-N-1. It is never reconfigured.
- UART2 (`Serial2`, GPIO15 TX / GPIO36 RX) is a second, independent input
  with two roles selected by `uart2Mode`:
  - `fsk2`: a second RTTY input, 9600/8-N-1, same `[`/`]`/`\` protocol,
    tagged `FSK2` on the LCD.
  - `cw`: Winkey host protocol, 1200/8-N-2, keying CW on `CW_KEY_PIN`
    (GPIO12; HIGH=key down, LOW=key up), separate from RTTY's `FSK_PIN`,
    tagged `CWK`.
- `uart2Mode` is switched from the web Configuration page, and later from
  the encoder long-press. Like every config change, it is refused during an
  active transmission. `MODE_SEL_PIN` follows it, but is still a stub until
  the MCP23017 driver exists.
- Both links feed the same TxManager queue, so RTTY and CW never transmit
  at the same time; UART1 is polled first each `loop()` pass. See "Session
  hand-off" in the task-architecture section for how overlapping requests
  are sequenced.
- The emulator reports itself as a WK2 (revision 23), because hosts enable
  WK3-only commands for revision 30 and above.

### Research findings (verified, not assumed)

- **K3NG keyer** (https://github.com/k3ng/k3ng_cw_keyer) — open-source
  AVR/Arduino CW keyer with a huge compile-time-configurable feature
  set. Ships its own Winkey emulation
  (`FEATURE_WINKEY_EMULATION`/`OPTION_WINKEY_2_SUPPORT`, Winkey 1 or 2),
  described by its own docs as "99.9% complete," tested against N1MM+,
  Win-Test, and Iambic Master
  (https://github.com/k3ng/k3ng_cw_keyer/wiki/370-Feature:-Winkey). Good
  reference for the emulation's behavior/state machine — not code to
  port verbatim (different MCU/toolchain), but confirms the protocol
  surface is fully implementable on modest embedded hardware.
- **Winkey protocol, confirmed from the official K1EL manual**
  (https://usermanual.wiki/Document/winkeyusbman.2294652917/html):
  fixed **1200 baud, 8 data bits, 2 stop bits, no parity**. Host opens
  with admin command `<00><02>`, keyer replies with its revision; text
  then streams as plain ASCII into a 128-byte FIFO that the keyer times
  and keys itself (host does not toggle individual dits/dahs). Notable
  overlap with what's already built: a **PTT lead/tail command**
  (`<04><lead><tail>`, 10ms steps) — conceptually identical to the
  already-implemented FSK PTT/PA lead-tail sequencer, just fed by CW key
  events instead of FSK bit toggles. Also relevant: a mode-register bit
  selects Iambic A/B/Ultimatic/Bug, and speed can be sourced either from
  the host (`<02><nn>`) or from a **physical potentiometer**
  (`<02><00>`).
- **Confirmed from this project's own AVR source**
  (`TinyFSK_ZAW_01.cpp`): the existing host serial link (UART0, J1
  RXD/TXD) runs at **9600 baud, 8-N-1** — a genuinely different framing
  from Winkey's fixed 1200/8-N-2, not just a different baud number. The
  two protocols cannot be "live" on the UART at the same time; switching
  between them means reconfiguring the port (`Serial.end()` +
  `Serial.begin()` with the new baud/format), not literally running
  both.

### Decisions made (user confirmed, 2026-09-03)

- **No local paddle — pure host-driven keying.** N1MM/the logger is the
  only source of CW text and timing; this board never runs an iambic
  paddle state machine. This is what makes the zero-new-pins design
  below possible.
- **Shared UART0, mode-switched (not simultaneous).** One new config
  field, `serialMode: "rtty" | "cw"`. Switching it reconfigures the same
  physical UART0/J1 RXD-TXD link between the existing 9600/8-N-1 RTTY
  control protocol and Winkey's 1200/8-N-2. Only one is ever active.
  **Trigger: two ways, confirmed** — (a) a toggle on the web UI, and (b)
  a **long press** of the encoder's pushbutton. This is a new gesture
  for that button, distinct from its existing **short press** (cycles
  the encoder's target: RX volume → TX volume → CW speed, see the
  mode-select LED section above) — firmware needs to discriminate
  press duration (a threshold around 700-800ms is a reasonable starting
  point; not yet bench-verified, see open questions).
- **Dedicated CW keying GPIO (confirmed 2026-10-20):** `CW_KEY_PIN`
  (MISO/GPIO12) is active HIGH for key-down and LOW for key-up. The
  external load must not pull GPIO12 HIGH during reset; it is the ESP32
  MTDI flash-voltage strap, and a HIGH strap can prevent boot with 3.3V
  flash. `FSK_PIN` (OUT2/GPIO5) remains dedicated to RTTY mark/space.
  - **`PTT_PIN`/`PTT_PA_PIN` reused as-is** — same lead/tail sequencer
    already built for FSK, just triggered by CW key-down/key-up events
    instead of FSK bit-toggle events. Winkey's own PTT lead/tail
    command maps directly onto parameters this sequencer already has.
  - **No sidetone** (confirmed 2026-09-03 — user's choice). Also
    practically forced: no direct GPIO was free for it anyway (the 2
    remaining MCP23017 pins are I2C, too slow to bit-bang an audio
    tone), and without a local paddle there's no clear local listener
    for it — the radio or the PC already provides sidetone.
  - **One new MCP23017 output, `MODE_SEL_PIN`** (confirmed 2026-09-03,
    **corrected the same day — this is not an LED/activity indicator**,
    it's a hardware routing control line). `MODE_SEL_PIN` tells downstream
    interface circuitry which operating mode is active (for example, to
    select the RTTY or CW signal path); the keying signals themselves now
    have separate GPIOs. It is driven directly from
    `serialMode` as a static level (not a per-dit/dah pulse): **LOW for
    RTTY, HIGH for CW/Winkey active**. Takes the last spare MCP23017
    pin — see the updated pin budget above (15 of 16 committed, 1
    spare).
- **LCD display (confirmed 2026-09-03)**:
  - The existing PTT-source field (today shows `FSK ..` vs `RTS DIGI`,
    see the Web UI architecture section above) gets a third value,
    **`CWK`**, shown whenever `serialMode == "cw"` — same field,
    same mechanism, just one more value.
  - The existing shared live-text bottom line (already TX text / RX
    decoded text / level-readout, see the "LCD feedback while trimming
    RX/TX level" section above) also carries **CW-sent text while in CW
    mode** — this is not a new priority tier, since `serialMode` being
    mutually exclusive means "TX text" and "CW text" are simply the
    same slot's meaning depending on the active mode, not two things
    competing for it at once. Everything else about that line's
    existing 3-tier arbitration is unaffected.

### Not yet decided (CW/Winkey)
- Exact long-press threshold for the encoder pushbutton's mode-switch
  gesture (proposed starting point ~700-800ms) — not bench-verified.
- ~~How much of the Winkey command set to implement~~ — **resolved
  2026-09-28**: the full WK2 host command set is parsed with the correct
  byte counts. Everything with a local equivalent is implemented; the rest
  (paddle, pot, sidetone, HSCW, dit/dah ratio, pin config, buffer-pointer
  editing, standalone messages) is accepted and ignored. Not implemented:
  9600-baud mode (WK3 admin `0x12`) and serial echo of sent characters.
- Which MCP23017 pin number gets `MODE_SEL_PIN` (just "the last spare
  one" so far, not assigned to a specific pin).
- **CW speed behavior in current firmware:** persisted `cwSpeedWpm` is the
  boot/default speed. Winkey `<02><nn>` changes the running speed only;
  `<02><00>` leaves it unchanged, and host speed commands are not written
  to flash. The earlier plan to have an encoder update the shared value
  and trigger an LCD readout is not implemented.
- **Still open for future encoder work:** whether the encoder should
  update only the running speed, the persisted default, or both. That
  choice does not affect the current Winkey speed-command behavior.

## Not yet decided / not yet discussed
- Whether the onboard USB chip stays populated/wired in the final build
  given the external isolated adapter plan (see bus-contention note above)
  — hardware/assembly question, not firmware.
- Full CSS custom-property token list beyond the two confirmed anchor
  tokens — implemented with a self-consistent palette inspired by the
  Band Magic reference (its full CSS wasn't retained verbatim in this
  session's context), not literally copied; revisit if the user wants
  pixel-parity with the reference.
- Exact validation-error message wording — implemented with plain
  English messages, not user-reviewed.
- The web Configuration form submits the complete config object. The
  backend validation/merge path also accepts omitted fields and preserves
  their current values.
- Which specific opto-isolated USB-serial adapter to use.
- Ring-buffer depth (implemented: 4) and FreeRTOS task priorities
  (implemented: TxManager `configMAX_PRIORITIES - 2` on core 1, web push
  task priority 1 on core 0) — not re-confirmed with the user, but
  consistent with the large timing margins involved.
- **Confirmed encoder pin allocation; firmware not implemented:** use
  GPIO2/GPIO14 for encoder A/B, move `LED_RX_PIN` from GPIO2 to the
  MCP23017, and remove the unused `ON_PIN` on GPIO14. Until the MCP23017
  LED migration lands, current firmware still drives `LED_RX_PIN` on
  GPIO2; see the GPIO mapping table above.
- The encoder pushbutton and its mode-cycle/long-press gestures are
  decided future hardware/firmware work. `cwSpeedWpm` and Winkey CW
  speed control already exist; encoder-based adjustment of that speed
  does not.
- **Which digipot approach replaces the now-superseded DPDT-switch
  plan** — single dual-wiper chip (AD5243/MCP4651) vs. a
  firmware-controlled I2C mux (TCA9548A/PCA9548A) — see the
  "Architectural implication" note in the mode-select LED section and
  the updated "Alternative hardware solution" section; not yet decided.

## AFSK-over-TCI parallel TX path (implemented 2026-10-12; hardware verification pending)

This is an optional transmit path for SunSDR/ExpertSDR3-compatible
transceivers. It adds software-generated AFSK audio sent over the existing
wired Ethernet connection using Expert Electronics' TCI v2.0 protocol.
It does **not** replace the existing direct-GPIO FSK output: both outputs
remain active for the same RTTY transmission, as explicitly confirmed by
the user. The direct FSK output remains the radio-independent baseline.

### TCI transport and audio flow

- The transceiver/ExpertSDR3 acts as the TCI WebSocket server; this device
  is a TCP/WebSocket client on the existing wired LAN. No Wi-Fi interface
  is required. The client uses `links2004/WebSockets @ 2.4.1`; its network
  loop runs in a separate low-priority task on core 0.
- The client configures audio with TCI text commands:
  `AUDIO_STREAM_SAMPLE_TYPE`, `AUDIO_STREAM_CHANNELS`,
  `AUDIO_STREAM_SAMPLES`, `AUDIO_SAMPLERATE`, and
  `TX_STREAM_AUDIO_BUFFERING`.
- TX audio uses a pull handshake: the server sends binary
  `TX_CHRONO` requests, and the client responds with binary
  `TX_AUDIO_STREAM` (`StreamType=2`) with the requested number of PCM
  samples. The 64-byte little-endian header and supported sample formats
  are serialized explicitly. Samples are generated in the WebSocket task
  from a bounded 63-symbol SPSC queue; the network task never holds a lock
  needed by the FSK ISR or TxManager. A queue underrun emits the same LTRS
  idle-diddle pattern as the direct FSK ISR and is reported on Serial.
- On transmit, the client sends `TRX:<idx>,true,tci;` to select TCI audio
  when the existing PTT sequencer asserts PTT; it sends
  `TRX:<idx>,false;` when the sequencer releases PTT. Direct GPIO FSK,
  GPIO PTT/PA timing, and the hardware inhibit remain controlled by the
  existing TX path. If TCI is not connected at key-up, or disconnects
  during transmission, direct GPIO FSK continues and an explanatory
  message is written to Serial.
- The supported audio choices in the researched TCI v2.0 spec are sample
  rates 8/12/24/48kHz, sample types int16/int24/int32/float32, one or two
  channels, and packet sizes 100-2048 samples. The packet size should
  represent at least 10ms of audio to avoid damaging the transmitted
  signal. TCI's client-side buffering timeout is 50-500ms (50ms default).

### Implemented AFSK generation and configuration

- `AfskModulator` consumes the same queued Baudot symbols as `FskTimer`,
  including LSB-first 5-bit data, a start bit, and the 1.5-bit stop period.
  Its continuous-phase oscillator uses the configured baud rate and keeps
  phase across Mark/Space changes. It renders the same character stream;
  it does not replace or disable `FSK_PIN`.
- The Configuration page persists the TCI enable flag, server IPv4/port,
  transceiver index, Mark/Space tones, sample rate, sample format, channel
  count, samples per frame, and buffering timeout. Defaults are disabled,
  `192.168.1.100:50001`, transceiver 0, Mark 2125 Hz, Space 2295 Hz,
  48 kHz, int16, mono, 480 samples, and 50 ms. The tone pair is
  independently editable; 2125/2295 Hz is the confirmed default.
- Validation constrains the protocol ranges and requires each frame to
  represent at least 10 ms of audio at the selected sample rate. TCI
  configuration changes are persisted through the existing ConfigStore
  path and applied only when no TX is active.

This resolves part of the old "TX audio" digipot question: the TCI audio
exists as digital PCM and leaves through Ethernet, so this path needs no
on-board DAC, analog output stage, or TX digipot. A separate analog AFSK
audio output for non-TCI radios is not part of this feature and has not
been requested.

### Verification still required before on-air use

- The PlatformIO CLI was unavailable in the implementation environment,
  so the ESP32 build against the pinned Arduino-ESP32 2.0.17 core and
  WebSockets library has not yet been run.
- TCI command spellings, binary interoperability, server-side TX source
  selection, sample format/channel interpretation, and end-to-end audio
  must be checked against the target ExpertSDR3/SunSDR version.
- Bench-check GPIO FSK and TCI AFSK start/stop alignment, sample-clock
  drift, audio buffering/latency, RF tone levels, and simultaneous output
  behavior on the actual radio before using the feature on air.
- Connection status is currently reported on Serial only; no browser
  indicator or radio-rejection acknowledgement is implemented.
