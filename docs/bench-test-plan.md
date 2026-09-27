# Minimum RTTY bench-test plan

Run these checks before connecting a transceiver or amplifier. Use a logic
analyser or oscilloscope and external pull resistors matching the hardware
notes in `README.md` and `include/Pins.h`.

## Required setup

- Power the Impero32 from an isolated bench supply.
- Leave the radio, amplifier, and relay driver loads disconnected.
- Fit external pull-downs on `PTT_PIN` (GPIO16) and `PTT_PA_PIN` (GPIO13).
- Fit external pull-ups on active-LOW `CPU_INH_PIN` (GPIO35) and
  `PTT_USB_RTS_PIN` (GPIO39); neither input may float.
- Observe GPIO5 (FSK), GPIO13 (PA), and GPIO16 (main PTT) simultaneously.

## 1. Boot safety

1. Capture all three outputs from power-on through completion of the splash.
2. Repeat with reset from the onboard USB interface and from the EN pin.
3. Confirm GPIO13 and GPIO16 never go HIGH.
4. Confirm GPIO5 reaches the configured Mark idle level after timer setup.

Pass: both PTT outputs remain LOW for the complete boot. Any pulse is a hard
failure and must be solved in hardware before a transmitter is attached.

## 2. UART1 RTTY path

At 9600/8-N-1 send `[TEST]` over UART1.

1. Confirm PA goes HIGH first.
2. Confirm main PTT goes HIGH after configured `paLeadMs`.
3. Confirm the first FSK start bit begins after configured `pttLeadMs`.
4. Decode GPIO5 and verify the ITA2 text `TEST`, including the initial LTRS
   shift and 1.5-stop-bit framing.
5. Confirm `cmd:` is returned only after the complete tail sequence.

Repeat at 45.45, 50, and 75 baud and with both Mark polarities.

## 3. End and abort

- Buffered end: send `[TEST]`; verify all queued text is transmitted before
  tail sequencing starts.
- Abort during PA lead: send `[TEST`, then `\`; verify no FSK text begins.
- Abort during PTT lead: verify the active transmission stops and follows the
  configured graceful tail behavior.
- Abort during FSK data: verify remaining buffered text is discarded.
- Abort during either tail state: verify both outputs still finish LOW.

Pass: no command sequence can leave GPIO13 or GPIO16 HIGH indefinitely.

## 4. Hardware inhibit

Exercise GPIO35 LOW while idle, during PA lead, during PTT lead, during data,
and during each tail state.

Pass in every case:

- GPIO13 and GPIO16 go LOW without configured tail delays.
- FSK output stops immediately.
- queued text and control commands are discarded;
- releasing inhibit does not restart the cancelled transmission;
- LCD/web status changes to `INHIBIT`, then back to RX after release.

## 5. External RTS input

1. Pull GPIO39 LOW and verify the normal PA/PTT lead sequence.
2. Confirm GPIO5 is held literally LOW throughout an RTS-owned session.
3. Release GPIO39 and verify a safe return to RX.
4. Assert hardware inhibit while RTS remains LOW and confirm immediate
   shutdown. The current level-driven behavior re-keys after inhibit is
   released if RTS is still LOW; confirm that this is the desired station
   behavior before connecting external equipment.

## Confirmed tail sequence

Confirmed by the user on 2026-09-27: main PTT must release first and PA PTT
must release only after its additional delay. The implemented sequence is:

```text
last FSK bit -> wait pttTailMs -> main PTT LOW -> wait paTailMs -> PA LOW
```

Pass: GPIO13 remains HIGH for the complete time GPIO16 is HIGH, and falls only
after GPIO16 plus the configured `paTailMs` delay.
