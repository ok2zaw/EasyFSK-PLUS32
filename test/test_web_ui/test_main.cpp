#include <unity.h>

#include <fstream>
#include <sstream>
#include <string>

#include "Arduino.h"

SerialMock Serial;

namespace {

std::string html;

void assertContains(const char *text) {
  TEST_ASSERT_NOT_EQUAL(std::string::npos, html.find(text));
}

} // namespace

void setUp() {
  Serial.reset();
  std::ifstream file("data/index.html", std::ios::binary);
  std::ostringstream contents;
  contents << file.rdbuf();
  html = contents.str();
  TEST_ASSERT_FALSE(html.empty());
}

void tearDown() {}

void test_uart2_and_cw_controls_exist_with_backend_ranges() {
  assertContains("id=\"uart2Mode\"");
  assertContains("value=\"fsk2\"");
  assertContains("value=\"cw\"");
  assertContains("id=\"cwSpeedWpm\" min=\"5\" max=\"99\" required");
}

void test_config_load_populates_uart2_and_cw_controls() {
  assertContains("getElementById('uart2Mode').value = cfg.uart2Mode || 'fsk2'");
  assertContains("getElementById('cwSpeedWpm').value = cfg.cwSpeedWpm || 20");
}

void test_config_save_sends_uart2_and_cw_values() {
  assertContains("uart2Mode: document.getElementById('uart2Mode').value");
  assertContains("cwSpeedWpm: parseInt(document.getElementById('cwSpeedWpm').value, 10)");
}

void test_network_restart_requirement_is_visible_and_reported() {
  assertContains("pattern=\"[A-Za-z0-9](?:[A-Za-z0-9-]{0,29}[A-Za-z0-9])?\"");
  assertContains("Network changes take effect after restart.");
  assertContains("Restart to apply network changes.");
}

void test_storage_error_response_is_shown_to_user() {
  assertContains("data.reason === 'storage_error'");
  assertContains("data.message || 'Could not write configuration storage.'");
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_uart2_and_cw_controls_exist_with_backend_ranges);
  RUN_TEST(test_config_load_populates_uart2_and_cw_controls);
  RUN_TEST(test_config_save_sends_uart2_and_cw_values);
  RUN_TEST(test_network_restart_requirement_is_visible_and_reported);
  RUN_TEST(test_storage_error_response_is_shown_to_user);
  return UNITY_END();
}
