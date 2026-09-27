#include "WinkeyEmulator.h"
#include "TxManager.h"
#include "Morse.h"
#include "Pins.h"
#include <Arduino.h>

// ---------------------------------------------------------------------------
// See WinkeyEmulator.h for the module's role. Command-byte values and
// per-command parameter-byte counts below are taken from the K1EL WinKey
// protocol manual (see the design doc's CW/Winkey section -- "Research
// findings, verified, not assumed"), not guessed: framing (1200 baud,
// 8 data bits, 2 stop bits, no parity), the Admin sub-command table, and
// the Immediate/Buffered command tables. A handful of corners with no
// local-hardware equivalent on this board (paddle/pot-related commands,
// HSCW, dit/dah ratio, pin config, the variable-length "pointer" buffer
// command, and the exact byte layout of Get Values/Dump EEPROM's replies)
// are ACCEPTED with the correct byte count -- so a real host's parser never
// desyncs talking to this board -- but are not fully acted on. Each is
// commented individually below; see also the design doc's "Not yet decided
// (CW/Winkey)" section.
// ---------------------------------------------------------------------------

namespace WinkeyEmulator {

namespace {

// --- Host command bytes (0x00-0x1F) ---
constexpr uint8_t CMD_ADMIN = 0x00;
constexpr uint8_t CMD_SIDETONE = 0x01;
constexpr uint8_t CMD_SET_SPEED = 0x02;
constexpr uint8_t CMD_SET_WEIGHT = 0x03;
constexpr uint8_t CMD_PTT_LEAD_TAIL = 0x04;
constexpr uint8_t CMD_SPEED_POT_SETUP = 0x05;
constexpr uint8_t CMD_PAUSE = 0x06;
constexpr uint8_t CMD_GET_SPEED_POT = 0x07;
constexpr uint8_t CMD_BACKSPACE = 0x08;
constexpr uint8_t CMD_PIN_CONFIG = 0x09;
constexpr uint8_t CMD_CLEAR_BUFFER = 0x0A;
constexpr uint8_t CMD_KEY_IMMEDIATE = 0x0B;
constexpr uint8_t CMD_SET_HSCW = 0x0C;
constexpr uint8_t CMD_SET_FARNSWORTH = 0x0D;
constexpr uint8_t CMD_SET_WK2_MODE = 0x0E;
constexpr uint8_t CMD_LOAD_DEFAULTS = 0x0F;
constexpr uint8_t CMD_SET_FIRST_EXT = 0x10;
constexpr uint8_t CMD_SET_KEY_COMP = 0x11;
constexpr uint8_t CMD_SET_PADDLE_SWITCHPOINT = 0x12;
constexpr uint8_t CMD_NULL = 0x13;
constexpr uint8_t CMD_SOFTWARE_PADDLE = 0x14;
constexpr uint8_t CMD_REQUEST_STATUS = 0x15;
constexpr uint8_t CMD_POINTER = 0x16;
constexpr uint8_t CMD_SET_RATIO = 0x17;
constexpr uint8_t CMD_BUF_PTT = 0x18;
constexpr uint8_t CMD_BUF_KEY = 0x19;
constexpr uint8_t CMD_BUF_WAIT = 0x1A;
constexpr uint8_t CMD_BUF_MERGE = 0x1B;
constexpr uint8_t CMD_BUF_SPEED = 0x1C;
constexpr uint8_t CMD_BUF_HSCW = 0x1D;
constexpr uint8_t CMD_BUF_CANCEL_SPEED = 0x1E;
constexpr uint8_t CMD_BUF_NOP = 0x1F;

// --- Admin (0x00) sub-command bytes ---
constexpr uint8_t ADMIN_CALIBRATE = 0x00;
constexpr uint8_t ADMIN_RESET = 0x01;
constexpr uint8_t ADMIN_OPEN = 0x02;
constexpr uint8_t ADMIN_CLOSE = 0x03;
constexpr uint8_t ADMIN_ECHO = 0x04;
constexpr uint8_t ADMIN_PADDLE_A2D = 0x05;
constexpr uint8_t ADMIN_SPEED_A2D = 0x06;
constexpr uint8_t ADMIN_GET_VALUES = 0x07;
constexpr uint8_t ADMIN_GET_CAL = 0x09;
constexpr uint8_t ADMIN_SET_WK1_MODE = 0x0A;
constexpr uint8_t ADMIN_SET_WK2_MODE = 0x0B;
constexpr uint8_t ADMIN_DUMP_EEPROM = 0x0C;
constexpr uint8_t ADMIN_LOAD_EEPROM = 0x0D;
constexpr uint8_t ADMIN_SEND_MSG = 0x0E;

// Arbitrary but plausible single-byte "revision" reply to Host Open --
// real hosts generally just check it's non-zero/sane, not a specific value.
constexpr uint8_t WK_REVISION_BYTE = 0x21;

// Sentinels for the generic param-collector's `s_pendingCmd` when the
// pending command came from the ADMIN (0x00 <sub>) path rather than the
// top-level 0x00-0x1F command byte -- needed because admin sub-codes reuse
// the same 0x00-0x1F numeric range as top-level commands (ADMIN_LOAD_EEPROM
// is 0x0D, the same byte value as CMD_SET_FARNSWORTH; ADMIN_SEND_MSG is
// 0x0E, same as CMD_SET_WK2_MODE) -- without these, onParamsComplete()
// would misfire the wrong handler once their parameter bytes finish
// collecting. Values are outside the 0x00-0x1F range so they can never
// collide with a real command byte.
constexpr uint8_t PENDING_ADMIN_LOAD_EEPROM = 0xD0;
constexpr uint8_t PENDING_ADMIN_SEND_MSG = 0xD1;

// UART1's own TX_ON/TX_END/TX_ABORT convention, reused verbatim for UART2's
// "fsk2" mode (SerialControl.cpp has the canonical copies of these).
constexpr char FSK2_TX_ON = '[';
constexpr char FSK2_TX_END = ']';
constexpr char FSK2_TX_ABORT = '\\';

// --- module state ---
Uart2Mode s_mode = Uart2Mode::Fsk2;
bool s_serial2Up = false;

// Winkey parser state
bool s_hostOpen = false;
bool s_wk2Mode = false;
bool s_echoNextByte = false;
uint8_t s_pendingCmd = 0;
uint16_t s_paramsRemaining = 0;
uint8_t s_paramBytes[2]; // only CMD_PTT_LEAD_TAIL needs both kept
uint8_t s_paramsCollected = 0;
bool s_pendingIsAdminSubCode = false;

void sendByte(uint8_t b) { Serial2.write(b); }

void resetWinkeyParser() {
  s_hostOpen = false;
  s_wk2Mode = false;
  s_echoNextByte = false;
  s_pendingCmd = 0;
  s_paramsRemaining = 0;
  s_paramsCollected = 0;
  s_pendingIsAdminSubCode = false;
  TxManager::cwClearPendingBuffer();
}

void beginParams(uint8_t cmd, uint16_t count) {
  s_pendingCmd = cmd;
  s_paramsRemaining = count;
  s_paramsCollected = 0;
}

void handleRequestStatus() {
  // Status byte format (WK1-compatible; see the design doc's fetched
  // summary): bits 7-6 always 0b11, bit4 WAIT, bit3 KEYDOWN(tune)/
  // WK2-pushbutton-flag, bit2 BUSY, bit1 BREAKIN, bit0 XOFF.
  uint8_t statusByte = 0xC0;
  TxManager::Status txStatus = TxManager::getStatus();
  bool busy = txStatus.txActive && txStatus.pttSource == TxManager::Source::Winkey;
  bool xoff = TxManager::cwBufferPending() >= ((Morse::BUFFER_SIZE * 2) / 3);
  if (busy) statusByte |= 0x04;
  if (xoff) statusByte |= 0x01;
  // WAIT (timed-event pending) and BREAKIN (paddle break-in) have no
  // equivalent in this host-only-keying implementation -- left clear. The
  // WK2 pushbutton-status variant of this byte (bit3=1) is likewise not
  // modeled: no pushbutton exists to report (see the design doc's encoder
  // section -- the encoder pushbutton, once built, is a mode-cycle/
  // long-press control, not a Winkey-reportable paddle-adjacent button).
  sendByte(statusByte);
}

void handleGetSpeedPot() {
  // Speed Pot Status Byte: bits 7-6 = 0b10, value 0-31. No physical pot on
  // this board (the design doc uses the rotary encoder instead) -- always
  // reports position 0, same "unsupported, returns 0" convention the
  // manual itself uses for Paddle A2D/Speed A2D.
  sendByte(0x80);
}

void handleAdminSub(uint8_t sub) {
  switch (sub) {
    case ADMIN_CALIBRATE:
      break; // historical, no longer required per the manual -- no-op
    case ADMIN_RESET:
      resetWinkeyParser();
      TxManager::cwSetSpeedWpm(20);
      TxManager::cwSetWeightingPct(50);
      TxManager::cwSetFarnsworthWpm(0);
      TxManager::cwSetKeyCompMs(0);
      TxManager::cwSetFirstExtensionMs(0);
      TxManager::cwSetPttLeadTail(0, 0);
      break;
    case ADMIN_OPEN:
      s_hostOpen = true;
      sendByte(WK_REVISION_BYTE);
      break;
    case ADMIN_CLOSE:
      s_hostOpen = false;
      TxManager::cwClearPendingBuffer();
      break;
    case ADMIN_ECHO:
      s_echoNextByte = true;
      break;
    case ADMIN_PADDLE_A2D:
    case ADMIN_SPEED_A2D:
    case ADMIN_GET_CAL:
      sendByte(0x00); // "unsupported, returns 0" -- no local paddle/pot
      break;
    case ADMIN_GET_VALUES:
      // TODO: the exact 15-byte reply layout isn't verified against real
      // K1EL hardware or a packet capture. Sending the correct COUNT (so a
      // strict host parser stays in sync) with placeholder zero content
      // rather than fabricating specific field values not confirmed
      // accurate. Revisit if a host is found that actually parses this
      // reply instead of treating it as a keepalive/probe.
      for (uint8_t i = 0; i < 15; i++) sendByte(0x00);
      break;
    case ADMIN_SET_WK1_MODE:
      s_wk2Mode = false;
      break;
    case ADMIN_SET_WK2_MODE:
      s_wk2Mode = true;
      break;
    case ADMIN_DUMP_EEPROM:
      // Same reasoning as Get Values above -- config lives in LittleFS/
      // JSON on this port (see ConfigStore), not raw EEPROM bytes, so
      // there's no real 256-byte image to dump.
      for (uint16_t i = 0; i < 256; i++) sendByte(0x00);
      break;
    case ADMIN_LOAD_EEPROM:
      // Must still consume exactly 256 bytes to stay in sync with the
      // host's stream, even though nothing is done with them. Uses the
      // PENDING_ADMIN_* sentinel, not the raw sub-code -- see its comment.
      beginParams(PENDING_ADMIN_LOAD_EEPROM, 256);
      break;
    case ADMIN_SEND_MSG:
      beginParams(PENDING_ADMIN_SEND_MSG, 1); // <msg_number> -- no stored messages in this implementation
      break;
    default:
      // Unrecognized admin sub-code: no extra parameter bytes assumed. If
      // a real host uses an admin sub-command not in the table above with
      // its own trailing bytes, this will desync -- none are known to
      // exist beyond what's implemented here per the manual consulted.
      break;
  }
}

void onParamsComplete(uint8_t cmd, uint8_t finalByte) {
  switch (cmd) {
    case CMD_SIDETONE:
      break; // no local sidetone (design doc, 2026-09-03: "user's choice") -- accepted, ignored
    case CMD_SET_SPEED:
      if (finalByte != 0) {
        TxManager::cwSetSpeedWpm(finalByte);
      }
      // finalByte == 0 ("speed from potentiometer"): no pot on this board
      // -- defers to whatever cwSpeedWpm already is, per the design doc's
      // confirmed nn==0 semantics (matches the encoder's last-set value).
      break;
    case CMD_SET_WEIGHT:
      TxManager::cwSetWeightingPct(finalByte);
      break;
    case CMD_SPEED_POT_SETUP:
      break; // no pot -- discard (3 bytes already consumed by the generic collector)
    case CMD_PAUSE:
      // TODO: not wired to a hard pause of CW playback yet -- accepted so
      // the parser never desyncs, but keying continues regardless of this
      // command today.
      break;
    case CMD_PIN_CONFIG:
      break; // this board's pin routing is fixed by hardware design -- discard
    case CMD_KEY_IMMEDIATE:
      TxManager::cwSetTuneKeyDown(finalByte != 0);
      break;
    case CMD_SET_HSCW:
      break; // high-speed-CW (letters-per-minute) timing base not implemented -- discard
    case CMD_SET_FARNSWORTH:
      TxManager::cwSetFarnsworthWpm(finalByte);
      break;
    case CMD_SET_WK2_MODE:
      // Mode register (Iambic A/B/Ultimatic/Bug + echo/autospace bits):
      // mostly inert here -- this board has no local paddle or iambic
      // keyer state machine (design doc: "pure host-driven keying"), which
      // is what most of this register's bits actually configure.
      break;
    case CMD_SET_FIRST_EXT:
      TxManager::cwSetFirstExtensionMs(finalByte);
      break;
    case CMD_SET_KEY_COMP:
      TxManager::cwSetKeyCompMs(finalByte);
      break;
    case CMD_SET_PADDLE_SWITCHPOINT:
      break; // no paddle -- discard
    case CMD_SOFTWARE_PADDLE:
      break; // no local paddle -- discard
    case CMD_POINTER:
      // KNOWN LIMITATION: the real Winkey buffer-"pointer" command's
      // payload is variable-length; only its one sub-op byte is consumed
      // here. A host that actually uses buffer-pointer manipulation (rare
      // -- most logging software just streams text) could desync after
      // this command. Flagged in the design doc; revisit if observed.
      break;
    case CMD_SET_RATIO:
      break; // dit/dah ratio deviation not implemented -- discard
    case CMD_BUF_PTT:
      TxManager::cwEnqueueSideEffect(finalByte != 0 ? Morse::SideEffect::PttOn
                                                     : Morse::SideEffect::PttOff);
      break;
    case CMD_BUF_KEY:
      TxManager::cwEnqueueSideEffect(Morse::SideEffect::KeyBuffered, finalByte);
      break;
    case CMD_BUF_WAIT:
      TxManager::cwEnqueueSideEffect(Morse::SideEffect::Wait, finalByte);
      break;
    case CMD_BUF_SPEED:
      TxManager::cwEnqueueBufferedSpeed(finalByte);
      break;
    case CMD_BUF_HSCW:
      break; // buffered HSCW rate change -- HSCW not implemented, discard
    case PENDING_ADMIN_LOAD_EEPROM:
      break; // 256 bytes discarded, see handleAdminSub()
    case PENDING_ADMIN_SEND_MSG:
      break; // message number discarded -- no stored messages
    default:
      break;
  }
}

void handleWinkeyByte(uint8_t b) {
  if (s_paramsRemaining > 0) {
    if (s_pendingCmd == CMD_PTT_LEAD_TAIL && s_paramsCollected < 2) {
      s_paramBytes[s_paramsCollected++] = b;
    }
    s_paramsRemaining--;
    if (s_paramsRemaining == 0) {
      if (s_pendingCmd == CMD_PTT_LEAD_TAIL) {
        uint16_t leadMs = static_cast<uint16_t>(s_paramBytes[0]) * 10;
        uint16_t tailMs = static_cast<uint16_t>(s_paramBytes[1]) * 10;
        TxManager::cwSetPttLeadTail(leadMs, tailMs);
      } else {
        onParamsComplete(s_pendingCmd, b);
      }
      s_pendingCmd = 0;
      s_paramsCollected = 0;
    }
    return;
  }

  if (s_pendingIsAdminSubCode) {
    s_pendingIsAdminSubCode = false;
    handleAdminSub(b);
    return;
  }

  if (s_echoNextByte) {
    s_echoNextByte = false;
    sendByte(b);
    return;
  }

  if (b <= 0x1F) {
    switch (b) {
      case CMD_ADMIN: s_pendingIsAdminSubCode = true; return;
      case CMD_SIDETONE: beginParams(b, 1); return;
      case CMD_SET_SPEED: beginParams(b, 1); return;
      case CMD_SET_WEIGHT: beginParams(b, 1); return;
      case CMD_PTT_LEAD_TAIL: beginParams(b, 2); return;
      case CMD_SPEED_POT_SETUP: beginParams(b, 3); return;
      case CMD_PAUSE: beginParams(b, 1); return;
      case CMD_GET_SPEED_POT: handleGetSpeedPot(); return;
      case CMD_BACKSPACE: TxManager::cwBackspace(); return;
      case CMD_PIN_CONFIG: beginParams(b, 1); return;
      case CMD_CLEAR_BUFFER: TxManager::cwClearPendingBuffer(); return;
      case CMD_KEY_IMMEDIATE: beginParams(b, 1); return;
      case CMD_SET_HSCW: beginParams(b, 1); return;
      case CMD_SET_FARNSWORTH: beginParams(b, 1); return;
      case CMD_SET_WK2_MODE: beginParams(b, 1); return;
      case CMD_LOAD_DEFAULTS: beginParams(b, 15); return;
      case CMD_SET_FIRST_EXT: beginParams(b, 1); return;
      case CMD_SET_KEY_COMP: beginParams(b, 1); return;
      case CMD_SET_PADDLE_SWITCHPOINT: beginParams(b, 1); return;
      case CMD_NULL: return;
      case CMD_SOFTWARE_PADDLE: beginParams(b, 1); return;
      case CMD_REQUEST_STATUS: handleRequestStatus(); return;
      case CMD_POINTER: beginParams(b, 1); return;
      case CMD_SET_RATIO: beginParams(b, 1); return;
      case CMD_BUF_PTT: beginParams(b, 1); return;
      case CMD_BUF_KEY: beginParams(b, 1); return;
      case CMD_BUF_WAIT: beginParams(b, 1); return;
      case CMD_BUF_MERGE: TxManager::cwEnqueueMergeMark(); return;
      case CMD_BUF_SPEED: beginParams(b, 1); return;
      case CMD_BUF_HSCW: beginParams(b, 1); return;
      case CMD_BUF_CANCEL_SPEED: return; // no-op -- see Morse::CwBuffer::addCancelBufferedSpeed()
      case CMD_BUF_NOP: TxManager::cwEnqueueSideEffect(Morse::SideEffect::Nop); return;
      default: return; // unreachable: every 0x00-0x1F value is handled above
    }
  }

  // 0x20-0x7E text byte -- only means anything once the host has opened
  // the port (Admin Open, above); bytes above 0x7E have no Morse
  // representation and are silently dropped either way.
  if (s_hostOpen && b >= 0x20 && b <= 0x7E) {
    TxManager::cwEnqueueChar(b);
  }
}

void pollFsk2() {
  if (Serial2.available() <= 0) return;
  uint8_t b = Serial2.read();
  if (b == FSK2_TX_ABORT) {
    TxManager::enqueueAbort();
  } else if (b == FSK2_TX_ON) {
    TxManager::enqueueKeyUp(TxManager::Source::Uart2Fsk);
  } else if (b == FSK2_TX_END) {
    TxManager::enqueueBufferedEnd();
  } else {
    TxManager::enqueueByte(b, TxManager::Source::Uart2Fsk);
  }
}

void pollCw() {
  if (Serial2.available() <= 0) return;
  handleWinkeyByte(Serial2.read());
}

void openSerial2ForMode(Uart2Mode mode) {
  if (s_serial2Up) Serial2.end();
  if (mode == Uart2Mode::Cw) {
    Serial2.begin(1200, SERIAL_8N2, UART2_RX_PIN, UART2_TX_PIN);
  } else {
    Serial2.begin(9600, SERIAL_8N1, UART2_RX_PIN, UART2_TX_PIN);
  }
  s_serial2Up = true;
}

} // namespace

void begin(const Config &cfg) {
  s_mode = cfg.uart2Mode;
  openSerial2ForMode(s_mode);
  resetWinkeyParser();
  TxManager::cwSetSpeedWpm(cfg.cwSpeedWpm);
}

void applyConfig(const Config &cfg) {
  if (cfg.uart2Mode == s_mode) {
    return; // nothing UART2-mode-relevant changed; leave the port/parser alone
  }
  s_mode = cfg.uart2Mode;
  openSerial2ForMode(s_mode);
  resetWinkeyParser();
}

void poll() {
  if (s_mode == Uart2Mode::Cw) {
    pollCw();
  } else {
    pollFsk2();
  }
}

} // namespace WinkeyEmulator
