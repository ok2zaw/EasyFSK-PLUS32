# EasyFSK-PLUS32 development plan

This file records the agreed implementation order and the current state so
future work can continue without reconstructing the plan from conversation.

## Current state

- The project is an integration prototype and has not yet been verified on
  target hardware.
- The architecture is split into input/control, `TxManager`, Baudot/Morse
  generation, and ISR-driven FSK/CW output.
- `ModeSelect` is still a software-only stub until the MCP23017 driver exists.
- `README.md` contains an older GPIO/status description and must be reconciled
  with `include/Pins.h` before hardware assembly.
- Build environment selected in milestone 1: PlatformIO Espressif32 7.1.3,
  Arduino-ESP32 2.0.17, with exact library versions in `platformio.ini`.

## Implementation order

### 1. Reproducible build environment

- [x] Pin the PlatformIO platform and library versions.
- [x] Use one hardware-timer API consistently in `FskTimer` and `CwTimer`.
- [x] Obtain a clean firmware build and record the resulting resource usage.
- [x] Keep the toolchain versions documented.

Completed 2026-09-27: `pio run` succeeds. The release build uses 45,856 bytes
of RAM (14.0%) and 967,337 bytes of the 1,966,080-byte application slot
(49.2%).

### 2. Minimum RTTY firmware

- Verify safe boot levels for PTT and PA.
- Verify UART1 to Baudot to FSK output.
- Verify hardware inhibit, key-up, buffered end, and immediate abort.
- Verify non-blocking PTT/PA lead and tail sequencing.
- Keep LCD work outside the timing-critical path.

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

- Test missing, valid, and corrupt LittleFS configuration files.
- Test web changes during RX and rejection during active TX.
- Test Ethernet loss and reconnect.
- Add explicit handling/reporting for full TX queues and oversized requests.

Completion criterion: configuration recovery and network failures cannot
disturb an active transmission.

### 5. UART2 and CW

- Validate UART2 in FSK2 mode first.
- Validate the basic Winkey handshake and character playback.
- Add and test speed, weighting, backspace, and buffered commands.
- Implement physical mode routing through MCP23017 and replace the
  `ModeSelect` stub.

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
