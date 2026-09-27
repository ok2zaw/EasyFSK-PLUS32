#include <unity.h>

#include "Arduino.h"
#include "Baudot.h"
#include "TxSequencer.h"

SerialMock Serial;

void setUp() { Serial.reset(); }
void tearDown() {}

void test_empty_buffer_ends_when_requested() {
  Baudot::SendBuffer buffer;
  buffer.reset();

  Baudot::QueuedSymbol symbol = buffer.nextSymbol();

  TEST_ASSERT_EQUAL_UINT8(Baudot::TX_END_FLAG, symbol.baudotCode);
  TEST_ASSERT_EQUAL_UINT8(0, symbol.asciiByte);
}

void test_idle_buffer_sends_letters_diddle() {
  Baudot::SendBuffer buffer;
  buffer.reset();
  buffer.endWhenBufferEmpty = false;

  Baudot::QueuedSymbol symbol = buffer.nextSymbol();

  TEST_ASSERT_EQUAL_UINT8(Baudot::LTRS_SHIFT, symbol.baudotCode);
  TEST_ASSERT_EQUAL_UINT8(0, symbol.asciiByte);
}

void test_letter_injects_ltrs_shift_then_payload() {
  Baudot::SendBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addByte('A'));

  Baudot::QueuedSymbol shift = buffer.nextSymbol();
  Baudot::QueuedSymbol payload = buffer.nextSymbol();

  TEST_ASSERT_EQUAL_UINT8(Baudot::LTRS_SHIFT, shift.baudotCode);
  TEST_ASSERT_EQUAL_UINT8(0, shift.asciiByte);
  TEST_ASSERT_EQUAL_UINT8(3, payload.baudotCode);
  TEST_ASSERT_EQUAL_UINT8('A', payload.asciiByte);
  TEST_ASSERT_EQUAL_size_t(0, buffer.pending());
  TEST_ASSERT_EQUAL_size_t(1, Serial.writeCount);
  TEST_ASSERT_EQUAL_UINT8('A', Serial.lastByte);
}

void test_figure_injects_figs_shift_then_payload() {
  Baudot::SendBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addByte('1'));

  Baudot::QueuedSymbol shift = buffer.nextSymbol();
  Baudot::QueuedSymbol payload = buffer.nextSymbol();

  TEST_ASSERT_EQUAL_UINT8(Baudot::FIGS_SHIFT, shift.baudotCode);
  TEST_ASSERT_EQUAL_UINT8(23, payload.baudotCode);
  TEST_ASSERT_EQUAL_UINT8('1', payload.asciiByte);
}

void test_figure_to_letter_injects_ltrs_shift() {
  Baudot::SendBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addByte('1'));
  TEST_ASSERT_TRUE(buffer.addByte('A'));

  TEST_ASSERT_EQUAL_UINT8(Baudot::FIGS_SHIFT, buffer.nextSymbol().baudotCode);
  TEST_ASSERT_EQUAL_UINT8(23, buffer.nextSymbol().baudotCode);
  TEST_ASSERT_EQUAL_UINT8(Baudot::LTRS_SHIFT, buffer.nextSymbol().baudotCode);
  TEST_ASSERT_EQUAL_UINT8(3, buffer.nextSymbol().baudotCode);
}

void test_mmtTY_usos_reasserts_figs_after_space() {
  Baudot::SendBuffer buffer;
  buffer.reset();
  TEST_ASSERT_TRUE(buffer.addByte('1'));
  TEST_ASSERT_TRUE(buffer.addByte(' '));
  TEST_ASSERT_TRUE(buffer.addByte('2'));

  TEST_ASSERT_EQUAL_UINT8(Baudot::FIGS_SHIFT, buffer.nextSymbol().baudotCode);
  TEST_ASSERT_EQUAL_UINT8(23, buffer.nextSymbol().baudotCode);
  TEST_ASSERT_EQUAL_UINT8(4, buffer.nextSymbol().baudotCode);
  TEST_ASSERT_EQUAL_UINT8(Baudot::FIGS_SHIFT, buffer.nextSymbol().baudotCode);
  TEST_ASSERT_EQUAL_UINT8(19, buffer.nextSymbol().baudotCode);
}

void test_send_buffer_rejects_byte_501() {
  Baudot::SendBuffer buffer;
  buffer.reset();

  for (size_t i = 0; i < Baudot::SEND_BUFFER_SIZE; i++) {
    TEST_ASSERT_TRUE(buffer.addByte('A'));
  }

  TEST_ASSERT_EQUAL_size_t(Baudot::SEND_BUFFER_SIZE, buffer.pending());
  TEST_ASSERT_FALSE(buffer.addByte('B'));
  TEST_ASSERT_EQUAL_size_t(Baudot::SEND_BUFFER_SIZE, buffer.pending());
}

void test_tx_lead_sequence_honors_both_delays() {
  const TxSequencer::Timings timings{80, 150, 25, 80};

  TxSequencer::Transition t =
      TxSequencer::poll(TxSequencer::State::LeadPa, 79, 0, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::None),
                          static_cast<uint8_t>(t.event));

  t = TxSequencer::poll(TxSequencer::State::LeadPa, 80, 0, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::AssertPtt),
                          static_cast<uint8_t>(t.event));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::State::LeadPtt),
                          static_cast<uint8_t>(t.nextState));

  t = TxSequencer::poll(TxSequencer::State::LeadPtt, 229, 80, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::None),
                          static_cast<uint8_t>(t.event));

  t = TxSequencer::poll(TxSequencer::State::LeadPtt, 230, 80, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::StartSending),
                          static_cast<uint8_t>(t.event));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::State::Sending),
                          static_cast<uint8_t>(t.nextState));
}

void test_tx_tail_releases_ptt_before_pa() {
  const TxSequencer::Timings timings{80, 150, 25, 80};

  TxSequencer::Transition t =
      TxSequencer::poll(TxSequencer::State::TailPtt, 1024, 1000, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::None),
                          static_cast<uint8_t>(t.event));

  t = TxSequencer::poll(TxSequencer::State::TailPtt, 1025, 1000, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::ReleasePtt),
                          static_cast<uint8_t>(t.event));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::State::TailPa),
                          static_cast<uint8_t>(t.nextState));

  t = TxSequencer::poll(TxSequencer::State::TailPa, 1104, 1025, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::None),
                          static_cast<uint8_t>(t.event));

  t = TxSequencer::poll(TxSequencer::State::TailPa, 1105, 1025, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::ReleasePa),
                          static_cast<uint8_t>(t.event));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::State::Idle),
                          static_cast<uint8_t>(t.nextState));
}

void test_zero_tail_delays_still_release_in_order() {
  const TxSequencer::Timings timings{0, 0, 0, 0};

  TxSequencer::Transition first =
      TxSequencer::poll(TxSequencer::State::TailPtt, 500, 500, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::ReleasePtt),
                          static_cast<uint8_t>(first.event));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::State::TailPa),
                          static_cast<uint8_t>(first.nextState));

  TxSequencer::Transition second =
      TxSequencer::poll(first.nextState, 500, 500, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::ReleasePa),
                          static_cast<uint8_t>(second.event));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::State::Idle),
                          static_cast<uint8_t>(second.nextState));
}

void test_tx_sequence_handles_millis_wraparound() {
  const TxSequencer::Timings timings{20, 0, 0, 0};
  const uint32_t entered = UINT32_MAX - 9u;

  TxSequencer::Transition before =
      TxSequencer::poll(TxSequencer::State::LeadPa, 9u, entered, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::None),
                          static_cast<uint8_t>(before.event));

  TxSequencer::Transition due =
      TxSequencer::poll(TxSequencer::State::LeadPa, 10u, entered, timings);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(TxSequencer::Event::AssertPtt),
                          static_cast<uint8_t>(due.event));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_empty_buffer_ends_when_requested);
  RUN_TEST(test_idle_buffer_sends_letters_diddle);
  RUN_TEST(test_letter_injects_ltrs_shift_then_payload);
  RUN_TEST(test_figure_injects_figs_shift_then_payload);
  RUN_TEST(test_figure_to_letter_injects_ltrs_shift);
  RUN_TEST(test_mmtTY_usos_reasserts_figs_after_space);
  RUN_TEST(test_send_buffer_rejects_byte_501);
  RUN_TEST(test_tx_lead_sequence_honors_both_delays);
  RUN_TEST(test_tx_tail_releases_ptt_before_pa);
  RUN_TEST(test_zero_tail_delays_still_release_in_order);
  RUN_TEST(test_tx_sequence_handles_millis_wraparound);
  return UNITY_END();
}
