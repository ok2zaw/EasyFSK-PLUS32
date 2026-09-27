#include <unity.h>

#include <initializer_list>
#include <utility>
#include <vector>

#include "Arduino.h"
#include "Pins.h"
#include "TxManager.h"
#include "WinkeyEmulator.h"

HardwareSerialMock Serial2;

namespace {

struct MockTxManager {
  int clearCount = 0;
  int backspaceCount = 0;
  int mergeCount = 0;
  int cancelSpeedCount = 0;
  int immediateCancelSpeedCount = 0;
  int abortCount = 0;
  int bufferedEndCount = 0;
  int tuneKeyCallCount = 0;
  int pttTimingCallCount = 0;
  uint8_t speed = 0;
  uint8_t weighting = 0;
  uint8_t farnsworth = 0;
  uint8_t keyComp = 0;
  uint8_t firstExtension = 0;
  bool tuneKeyDown = false;
  uint16_t pttLeadMs = 0;
  uint16_t pttTailMs = 0;
  uint16_t pending = 0;
  TxManager::Status status;
  std::vector<uint8_t> chars;
  std::vector<uint8_t> bufferedSpeeds;
  std::vector<std::pair<Morse::SideEffect, uint8_t>> sideEffects;
  std::vector<std::pair<uint8_t, TxManager::Source>> fskBytes;
  std::vector<TxManager::Source> keyUps;

  void reset() { *this = MockTxManager{}; }
};

MockTxManager mock;

void feed(uint8_t value) {
  Serial2.pushRx(value);
  WinkeyEmulator::poll();
}

void feed(std::initializer_list<uint8_t> values) {
  for (uint8_t value : values) feed(value);
}

void openHost() {
  feed({0x00, 0x02});
  TEST_ASSERT_EQUAL_size_t(1, Serial2.tx.size());
  TEST_ASSERT_EQUAL_UINT8(23, Serial2.tx[0]); // WK2 v2.3
}

} // namespace

namespace TxManager {

bool enqueueKeyUp(Source src) {
  mock.keyUps.push_back(src);
  return true;
}

bool enqueueBufferedEnd() {
  mock.bufferedEndCount++;
  return true;
}

bool enqueueAbort() {
  mock.abortCount++;
  return true;
}

bool enqueueByte(uint8_t value, Source src) {
  mock.fskBytes.emplace_back(value, src);
  return true;
}

bool cwEnqueueChar(uint8_t asciiByte) {
  mock.chars.push_back(asciiByte);
  return true;
}

bool cwEnqueueMergeMark() {
  mock.mergeCount++;
  return true;
}

bool cwEnqueueSideEffect(Morse::SideEffect effect, uint8_t value) {
  mock.sideEffects.emplace_back(effect, value);
  return true;
}

bool cwEnqueueBufferedSpeed(uint8_t wpm) {
  mock.bufferedSpeeds.push_back(wpm);
  return true;
}

bool cwEnqueueCancelBufferedSpeed() {
  mock.cancelSpeedCount++;
  return true;
}

void cwCancelBufferedSpeedOverride() { mock.immediateCancelSpeedCount++; }

void cwBackspace() { mock.backspaceCount++; }
void cwClearPendingBuffer() { mock.clearCount++; }
uint16_t cwBufferPending() { return mock.pending; }

void cwSetSpeedWpm(uint8_t wpm) { mock.speed = wpm; }
void cwSetWeightingPct(uint8_t pct) { mock.weighting = pct; }
void cwSetFarnsworthWpm(uint8_t wpm) { mock.farnsworth = wpm; }
void cwSetKeyCompMs(uint8_t ms) { mock.keyComp = ms; }
void cwSetFirstExtensionMs(uint8_t ms) { mock.firstExtension = ms; }

void cwSetTuneKeyDown(bool down) {
  mock.tuneKeyDown = down;
  mock.tuneKeyCallCount++;
}

void cwSetPttLeadTail(uint16_t leadMs, uint16_t tailMs) {
  mock.pttLeadMs = leadMs;
  mock.pttTailMs = tailMs;
  mock.pttTimingCallCount++;
}

Status getStatus() { return mock.status; }

} // namespace TxManager

void setUp() {
  Serial2.reset();
  mock.reset();
  Config cfg;
  cfg.uart2Mode = Uart2Mode::Cw;
  cfg.cwSpeedWpm = 23;
  WinkeyEmulator::begin(cfg);
}

void tearDown() {}

void test_begin_configures_winkey_uart_and_initial_speed() {
  TEST_ASSERT_EQUAL_UINT32(1200, Serial2.baud);
  TEST_ASSERT_EQUAL_UINT32(SERIAL_8N2, Serial2.config);
  TEST_ASSERT_EQUAL_INT8(UART2_RX_PIN, Serial2.rxPin);
  TEST_ASSERT_EQUAL_INT8(UART2_TX_PIN, Serial2.txPin);
  TEST_ASSERT_EQUAL_size_t(1, Serial2.beginCount);
  TEST_ASSERT_EQUAL_INT(1, mock.clearCount);
  TEST_ASSERT_EQUAL_UINT8(23, mock.speed);
}

void test_text_is_ignored_until_admin_open_and_after_close() {
  feed('A');
  TEST_ASSERT_TRUE(mock.chars.empty());

  openHost();
  feed('B');
  TEST_ASSERT_EQUAL_size_t(1, mock.chars.size());
  TEST_ASSERT_EQUAL_UINT8('B', mock.chars[0]);

  feed({0x00, 0x03}); // Admin Close
  TEST_ASSERT_EQUAL_INT(2, mock.clearCount);
  feed('C');
  TEST_ASSERT_EQUAL_size_t(1, mock.chars.size());
}

void test_admin_echo_returns_exactly_the_next_byte() {
  feed({0x00, 0x04, 0xA5});
  TEST_ASSERT_EQUAL_size_t(1, Serial2.tx.size());
  TEST_ASSERT_EQUAL_HEX8(0xA5, Serial2.tx[0]);
}

void test_immediate_timing_commands_collect_their_parameters() {
  feed({0x02, 31});
  feed({0x03, 62});
  feed({0x0D, 14});
  feed({0x10, 7});
  feed({0x11, 8});
  feed({0x04, 9, 12});

  TEST_ASSERT_EQUAL_UINT8(31, mock.speed);
  TEST_ASSERT_EQUAL_UINT8(62, mock.weighting);
  TEST_ASSERT_EQUAL_UINT8(14, mock.farnsworth);
  TEST_ASSERT_EQUAL_UINT8(7, mock.firstExtension);
  TEST_ASSERT_EQUAL_UINT8(8, mock.keyComp);
  TEST_ASSERT_EQUAL_UINT16(90, mock.pttLeadMs);
  TEST_ASSERT_EQUAL_UINT16(120, mock.pttTailMs);
  TEST_ASSERT_EQUAL_INT(1, mock.pttTimingCallCount);
}

void test_zero_speed_parameter_keeps_existing_speed() {
  TEST_ASSERT_EQUAL_UINT8(23, mock.speed);
  feed({0x02, 0});
  TEST_ASSERT_EQUAL_UINT8(23, mock.speed);
}

void test_key_immediate_tracks_key_up_and_down() {
  feed({0x0B, 1});
  TEST_ASSERT_TRUE(mock.tuneKeyDown);
  feed({0x0B, 0});
  TEST_ASSERT_FALSE(mock.tuneKeyDown);
  TEST_ASSERT_EQUAL_INT(2, mock.tuneKeyCallCount);
}

void test_buffer_commands_preserve_values_and_order() {
  feed({0x18, 1});
  feed({0x19, 4});
  feed({0x1A, 5});
  feed(0x1B);
  feed({0x1C, 36});
  feed(0x1F);

  TEST_ASSERT_EQUAL_size_t(4, mock.sideEffects.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::SideEffect::PttOn),
                          static_cast<uint8_t>(mock.sideEffects[0].first));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::SideEffect::KeyBuffered),
                          static_cast<uint8_t>(mock.sideEffects[1].first));
  TEST_ASSERT_EQUAL_UINT8(4, mock.sideEffects[1].second);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::SideEffect::Wait),
                          static_cast<uint8_t>(mock.sideEffects[2].first));
  TEST_ASSERT_EQUAL_UINT8(5, mock.sideEffects[2].second);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Morse::SideEffect::Nop),
                          static_cast<uint8_t>(mock.sideEffects[3].first));
  TEST_ASSERT_EQUAL_INT(1, mock.mergeCount);
  TEST_ASSERT_EQUAL_size_t(1, mock.bufferedSpeeds.size());
  TEST_ASSERT_EQUAL_UINT8(36, mock.bufferedSpeeds[0]);
}

void test_backspace_and_clear_are_forwarded() {
  feed(0x08);
  feed(0x0A);
  TEST_ASSERT_EQUAL_INT(1, mock.backspaceCount);
  TEST_ASSERT_EQUAL_INT(2, mock.clearCount); // begin reset + explicit clear
}

void test_status_reports_busy_and_xoff_bits() {
  mock.status.txActive = true;
  mock.status.pttSource = TxManager::Source::Winkey;
  mock.pending = static_cast<uint16_t>((Morse::BUFFER_SIZE * 2) / 3);
  feed(0x15);

  TEST_ASSERT_EQUAL_size_t(1, Serial2.tx.size());
  TEST_ASSERT_EQUAL_HEX8(0xC5, Serial2.tx[0]);
}

void test_status_ignores_activity_from_other_sources() {
  mock.status.txActive = true;
  mock.status.pttSource = TxManager::Source::Web;
  feed(0x15);
  TEST_ASSERT_EQUAL_HEX8(0xC0, Serial2.tx[0]);
}

void test_admin_reset_restores_winkey_defaults() {
  feed({0x00, 0x01});
  TEST_ASSERT_EQUAL_UINT8(20, mock.speed);
  TEST_ASSERT_EQUAL_UINT8(50, mock.weighting);
  TEST_ASSERT_EQUAL_UINT8(0, mock.farnsworth);
  TEST_ASSERT_EQUAL_UINT8(0, mock.keyComp);
  TEST_ASSERT_EQUAL_UINT8(0, mock.firstExtension);
  TEST_ASSERT_EQUAL_UINT16(0, mock.pttLeadMs);
  TEST_ASSERT_EQUAL_UINT16(0, mock.pttTailMs);
  TEST_ASSERT_EQUAL_INT(2, mock.clearCount);
}

void test_admin_load_eeprom_consumes_all_payload_bytes() {
  feed({0x00, 0x0D});
  for (int i = 0; i < 256; ++i) feed(0x02); // command-looking payload
  TEST_ASSERT_EQUAL_UINT8(23, mock.speed);

  openHost(); // parser is synchronized after exactly 256 payload bytes
  feed('Z');
  TEST_ASSERT_EQUAL_size_t(1, mock.chars.size());
  TEST_ASSERT_EQUAL_UINT8('Z', mock.chars[0]);
}

void test_load_defaults_applies_supported_fields() {
  feed({0x0F,
        0x00, // mode register
        28,   // speed
        0x05, // sidetone
        55,   // weight
        3,    // lead-in (x10 ms)
        4,    // tail (x10 ms)
        10,   // min WPM
        25,   // WPM range
        6,    // 1st extension
        7,    // key compensation
        15,   // farnsworth
        50,   // paddle setpoint
        50,   // dit/dah ratio
        0x06, // pin config
        0xFF}); // pot range

  TEST_ASSERT_EQUAL_UINT8(28, mock.speed);
  TEST_ASSERT_EQUAL_UINT8(55, mock.weighting);
  TEST_ASSERT_EQUAL_UINT16(30, mock.pttLeadMs);
  TEST_ASSERT_EQUAL_UINT16(40, mock.pttTailMs);
  TEST_ASSERT_EQUAL_UINT8(6, mock.firstExtension);
  TEST_ASSERT_EQUAL_UINT8(7, mock.keyComp);
  TEST_ASSERT_EQUAL_UINT8(15, mock.farnsworth);

  openHost(); // parser is synchronized after exactly 15 payload bytes
}

void test_load_defaults_zero_speed_keeps_existing_speed() {
  feed(0x0F);
  feed(0x00);
  feed(0x00); // speed 0 = "from pot"
  for (int i = 0; i < 13; ++i) feed(50);
  TEST_ASSERT_EQUAL_UINT8(23, mock.speed);
}

void test_status_change_is_pushed_unsolicited_while_host_open() {
  openHost();
  Serial2.tx.clear();

  WinkeyEmulator::poll(); // no change -> nothing sent
  TEST_ASSERT_EQUAL_size_t(0, Serial2.tx.size());

  mock.status.txActive = true;
  mock.status.pttSource = TxManager::Source::Winkey;
  WinkeyEmulator::poll();
  TEST_ASSERT_EQUAL_size_t(1, Serial2.tx.size());
  TEST_ASSERT_EQUAL_HEX8(0xC4, Serial2.tx[0]);

  WinkeyEmulator::poll(); // same status -> not repeated
  TEST_ASSERT_EQUAL_size_t(1, Serial2.tx.size());

  mock.status.txActive = false;
  WinkeyEmulator::poll();
  TEST_ASSERT_EQUAL_size_t(2, Serial2.tx.size());
  TEST_ASSERT_EQUAL_HEX8(0xC0, Serial2.tx[1]);
}

void test_status_is_not_pushed_while_host_closed() {
  mock.status.txActive = true;
  mock.status.pttSource = TxManager::Source::Winkey;
  WinkeyEmulator::poll();
  TEST_ASSERT_EQUAL_size_t(0, Serial2.tx.size());
}

void test_status_is_not_pushed_mid_command() {
  openHost();
  Serial2.tx.clear();
  feed(0x02); // Set Speed, parameter still pending
  mock.status.txActive = true;
  mock.status.pttSource = TxManager::Source::Winkey;
  WinkeyEmulator::poll();
  TEST_ASSERT_EQUAL_size_t(0, Serial2.tx.size());

  feed(30); // command complete -> change reported right after
  TEST_ASSERT_EQUAL_size_t(1, Serial2.tx.size());
  TEST_ASSERT_EQUAL_HEX8(0xC4, Serial2.tx[0]);
}

void test_polled_status_is_not_repeated_unsolicited() {
  openHost();
  Serial2.tx.clear();
  mock.status.txActive = true;
  mock.status.pttSource = TxManager::Source::Winkey;
  Serial2.pushRx(0x15);
  WinkeyEmulator::poll();
  WinkeyEmulator::poll();
  TEST_ASSERT_EQUAL_size_t(1, Serial2.tx.size());
  TEST_ASSERT_EQUAL_HEX8(0xC4, Serial2.tx[0]);
}

void test_wk3_admin_commands_keep_parser_in_sync() {
  feed({0x00, 0x0F, 0x02}); // Load X1MODE <nn>
  feed({0x00, 0x16, 0x02}); // Load X2MODE <nn>
  feed({0x00, 0x19, 0x02}); // Sidetone volume <nn>
  feed({0x00, 0x13, 0x02, 0x02}); // RTTY registers <p1><p2>
  TEST_ASSERT_EQUAL_UINT8(23, mock.speed); // no payload byte ran as Set Speed

  feed({0x00, 0x15}); // Read Vcc
  feed({0x00, 0x17}); // FW minor revision
  feed({0x00, 0x18}); // IC type
  TEST_ASSERT_EQUAL_size_t(3, Serial2.tx.size());
  TEST_ASSERT_EQUAL_UINT8(3, Serial2.tx[1]);
}

void test_mode_and_ratio_changes_cancel_buffered_speed_override() {
  feed({0x0E, 0x00});
  feed({0x17, 50});
  TEST_ASSERT_EQUAL_INT(2, mock.immediateCancelSpeedCount);
}

void test_cancel_buffered_speed_is_forwarded() {
  feed({0x1C, 40});
  feed(0x1E);
  TEST_ASSERT_EQUAL_size_t(1, mock.bufferedSpeeds.size());
  TEST_ASSERT_EQUAL_INT(1, mock.cancelSpeedCount);
}

void test_pointer_command_consumes_operand_when_sub_op_has_one() {
  openHost();
  feed({0x16, 0x00}); // reset pointers: no operand
  feed('A');
  feed({0x16, 0x01, 'X'}); // move pointer (overwrite) <nn>
  feed({0x16, 0x02, 'Y'}); // move pointer (append) <nn>
  feed({0x16, 0x03, 'Z'}); // add <nn> nulls
  feed('B');

  TEST_ASSERT_EQUAL_size_t(2, mock.chars.size());
  TEST_ASSERT_EQUAL_UINT8('A', mock.chars[0]);
  TEST_ASSERT_EQUAL_UINT8('B', mock.chars[1]);
}

void test_switch_to_fsk2_reconfigures_uart_and_routes_control_bytes() {
  Config cfg;
  cfg.uart2Mode = Uart2Mode::Fsk2;
  WinkeyEmulator::applyConfig(cfg);

  TEST_ASSERT_EQUAL_UINT32(9600, Serial2.baud);
  TEST_ASSERT_EQUAL_UINT32(SERIAL_8N1, Serial2.config);
  feed('[');
  feed('X');
  feed(']');
  feed('\\');

  TEST_ASSERT_EQUAL_size_t(1, mock.keyUps.size());
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxManager::Source::Uart2Fsk),
                          static_cast<uint8_t>(mock.keyUps[0]));
  TEST_ASSERT_EQUAL_size_t(1, mock.fskBytes.size());
  TEST_ASSERT_EQUAL_UINT8('X', mock.fskBytes[0].first);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxManager::Source::Uart2Fsk),
                          static_cast<uint8_t>(mock.fskBytes[0].second));
  TEST_ASSERT_EQUAL_INT(1, mock.bufferedEndCount);
  TEST_ASSERT_EQUAL_INT(1, mock.abortCount);
}

void test_apply_same_mode_does_not_reopen_uart_or_reset_parser() {
  openHost();
  const size_t beginCount = Serial2.beginCount;
  const int clearCount = mock.clearCount;
  Config cfg;
  cfg.uart2Mode = Uart2Mode::Cw;
  WinkeyEmulator::applyConfig(cfg);

  TEST_ASSERT_EQUAL_size_t(beginCount, Serial2.beginCount);
  TEST_ASSERT_EQUAL_INT(clearCount, mock.clearCount);
  feed('Q');
  TEST_ASSERT_EQUAL_size_t(1, mock.chars.size());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_begin_configures_winkey_uart_and_initial_speed);
  RUN_TEST(test_text_is_ignored_until_admin_open_and_after_close);
  RUN_TEST(test_admin_echo_returns_exactly_the_next_byte);
  RUN_TEST(test_immediate_timing_commands_collect_their_parameters);
  RUN_TEST(test_zero_speed_parameter_keeps_existing_speed);
  RUN_TEST(test_key_immediate_tracks_key_up_and_down);
  RUN_TEST(test_buffer_commands_preserve_values_and_order);
  RUN_TEST(test_backspace_and_clear_are_forwarded);
  RUN_TEST(test_status_reports_busy_and_xoff_bits);
  RUN_TEST(test_status_ignores_activity_from_other_sources);
  RUN_TEST(test_admin_reset_restores_winkey_defaults);
  RUN_TEST(test_admin_load_eeprom_consumes_all_payload_bytes);
  RUN_TEST(test_load_defaults_applies_supported_fields);
  RUN_TEST(test_load_defaults_zero_speed_keeps_existing_speed);
  RUN_TEST(test_status_change_is_pushed_unsolicited_while_host_open);
  RUN_TEST(test_status_is_not_pushed_while_host_closed);
  RUN_TEST(test_status_is_not_pushed_mid_command);
  RUN_TEST(test_polled_status_is_not_repeated_unsolicited);
  RUN_TEST(test_wk3_admin_commands_keep_parser_in_sync);
  RUN_TEST(test_mode_and_ratio_changes_cancel_buffered_speed_override);
  RUN_TEST(test_cancel_buffered_speed_is_forwarded);
  RUN_TEST(test_pointer_command_consumes_operand_when_sub_op_has_one);
  RUN_TEST(test_switch_to_fsk2_reconfigures_uart_and_routes_control_bytes);
  RUN_TEST(test_apply_same_mode_does_not_reopen_uart_or_reset_parser);
  return UNITY_END();
}
