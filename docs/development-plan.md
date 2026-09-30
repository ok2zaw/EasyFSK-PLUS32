# EasyFSK-PLUS32 development plan

This file records the agreed implementation order and the current state so
future work can continue without reconstructing the plan from conversation.

## Current state

- The project is an integration prototype and has not yet been verified on
  target hardware.
- The architecture is split into input/control, `TxManager`, Baudot/Morse
  generation, and ISR-driven FSK/CW output.
- `ModeSelect` is still a software-only stub until the MCP23017 driver exists.
- `README.md` and `include/Pins.h` describe the same current GPIO assignment;
  the future MCP23017/encoder reassignment remains clearly marked as planned.
- Build environment selected in milestone 1: PlatformIO Espressif32 7.1.3,
  Arduino-ESP32 2.0.17, with exact library versions in `platformio.ini`.
- Native tests run locally with WinLibs GCC/G++ 16.1.0 and in GitHub Actions;
  the current suite covers Baudot, TX sequencing, Morse generation, the
  UART2/Winkey parser, configuration validation/load/save behavior, guarded
  live configuration updates, web TX queue admission, and critical web-form
  bindings. The UART1 `SerialControl` parser and its serial configuration
  menu are covered as well; 113 host tests currently pass.

## Implementation order

### 1. Reproducible build environment

- [x] Pin the PlatformIO platform and library versions.
- [x] Use one hardware-timer API consistently in `FskTimer` and `CwTimer`.
- [x] Obtain a clean firmware build and record the resulting resource usage.
- [x] Keep the toolchain versions documented.

Verified 2026-09-28: `pio run` succeeds. The current release build uses 45,904
bytes of RAM (14.0%) and 971,821 bytes of the 1,966,080-byte application slot
(49.4%).

### 2. Minimum RTTY firmware

- [x] Drive PTT and PA LOW as the first operation in `setup()` and document
  the mandatory hardware pulls.
- [x] Cancel active and queued TX work immediately on hardware inhibit.
- [x] Make web Send/End/Abort report queue failures and prevent a missing End
  command from leaving the transmitter keyed.
- [x] Add host-side Baudot and PTT/PA sequencing unit tests, including zero
  delays and `millis()` wraparound.
- [x] Test UART1 byte routing and every serial `~` configuration-menu path,
  including input limits, cancellation, persistence errors, and queue refusal.
- [x] Run host tests and the ESP32 firmware build in GitHub Actions CI; the
  Windows workstation also has WinLibs `gcc`/`g++` 16.1.0 in `PATH`.
- [x] Add a repeatable no-radio bench-test checklist.
- Verify UART1 to Baudot to FSK output.
- Verify hardware inhibit, key-up, buffered end, and immediate abort.
- Verify non-blocking PTT/PA lead and tail sequencing on hardware.
- Keep LCD work outside the timing-critical path.

Tail order resolved 2026-09-27: after the final bit, wait `pttTailMs`, drop
main `PTT_PIN`, wait `paTailMs`, then drop `PTT_PA_PIN`. PA therefore remains
engaged for the entire period in which main PTT is active.

Completion criterion: all paths work on the bench without a connected radio.

### 3. Timing and safety bench tests

- Measure 45.45, 50, and 75 baud with a logic analyser or oscilloscope.
- Check mark/space polarity and stop-bit duration.
- Measure PTT and PA lead/tail delays.
- Exercise abort in every sequencing state.
- Force an empty symbol ring and verify underrun handling/counting.

Completion criterion: recorded measurements match configured values and every
failure mode leaves PTT/PA in the safe state.

### 4. Configuration and network

- [x] Test missing, valid, corrupt, and partly invalid LittleFS configuration
  files with a host-side filesystem double.
- [x] Test configuration apply/save while idle and rejection during active TX
  through `ConfigStore`; test storage failures without changing live state.
- Test the actual HTTP config handlers and responses with a web-server test
  harness.
- Test Ethernet loss and reconnect.
- [x] Add explicit handling/reporting for full TX queues and oversized
  requests.

Completion criterion: configuration recovery and network failures cannot
disturb an active transmission.

### 5. UART2 and CW

- Validate UART2 in FSK2 mode first.
- Validate the basic Winkey handshake and character playback.
- [x] Add host tests for the Winkey handshake/parser, speed, weighting,
  backspace, status, mode switching, and buffered commands.
- Implement physical mode routing through MCP23017 and replace the
  `ModeSelect` stub.

Host-test update 2026-09-28: 91 tests pass across the native, native-Winkey,
native-config, and native-config-store environments. The new Morse tests also
fixed complete word-gap timing, terminal generator-state reporting, and timing
changes that previously could split an in-flight character between old and new
settings. Configuration tests fixed per-field recovery from a partly invalid
persisted JSON file while preserving transactional validation for live API
updates. ConfigStore tests now guarantee that active TX and storage failures
cannot change live configuration. The web TX admission policy is also tested
at its text-length and queue-capacity boundaries. Persisted configuration now
uses a verified temporary file plus atomic rename; partial-write and rename
failure tests confirm that the previous file survives. Native test builds
treat all compiler warnings as errors. Web UI integration checks ensure the
UART2 mode and CW-speed controls remain wired to both config load and save,
and that network-restart/storage-error messages remain visible. Configuration
validation now rejects wrong JSON types and hostnames that start or end with a
hyphen. The shared HTTP configuration-response builder is tested for deferred
updates, per-field validation errors, persistence failures, and their status
codes; the POST controller is exercised through the real ConfigStore and owns
all returned JSON strings safely. Expanded Winkey coverage verifies Load
Defaults, unsolicited status changes, WK3-command parser synchronization,
buffer-pointer operands, Clear Buffer semantics, and temporary buffered-speed
restoration/cancellation.

Review and SerialControl update 2026-09-30: 113 host tests pass. The 13 new
UART1 parser tests cover one-byte polling, TX control routing, rejected queue
operations, every single-key and numeric config field, callsign limits,
cancellation, persistence errors, and state reset. Overlapping TX requests are
now sequenced instead of lost: an RTTY `[text]` that arrives during the PTT/PA
tail or during a CW session starts as the next session, CW waits behind
RTTY, and CW resumes from its own PTT tail without dropping PTT (rules in
`include/TxHandoff.h`, host-tested; the TxManager wiring still needs bench
verification). Also fixed: web Send during a tail or CW session, a UART2
mode switch from the web task racing `loop()`, config saves resetting the
logger's CW speed, the Winkey XOFF/BUSY bits during lead-in and while CW
waits, and the `liveLcdText` setting being ignored.

Completion criterion: both UART2 modes pass host and timing tests, and the
physical routing output always agrees with the selected mode.

### 6. Optional hardware and DSP features

- MCP23017 LEDs and bargraph.
- Encoder and digital potentiometer control.
- Tuning indicator and waterfall.
- RTTY receive decoder.
- OTA update flow.

These features start only after milestones 1-5 are stable.

## Working rule

Each milestone is complete only when the firmware builds, the relevant
behaviour is bench-tested, and README/pin documentation matches the code.
