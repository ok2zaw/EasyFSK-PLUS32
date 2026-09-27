#include <unity.h>

#include "Arduino.h"
#include "Baudot.h"

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
  TEST_ASSERT_EQUAL_SIZE_T(0, buffer.pending());
  TEST_ASSERT_EQUAL_SIZE_T(1, Serial.writeCount);
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

  TEST_ASSERT_EQUAL_SIZE_T(Baudot::SEND_BUFFER_SIZE, buffer.pending());
  TEST_ASSERT_FALSE(buffer.addByte('B'));
  TEST_ASSERT_EQUAL_SIZE_T(Baudot::SEND_BUFFER_SIZE, buffer.pending());
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
  return UNITY_END();
}
