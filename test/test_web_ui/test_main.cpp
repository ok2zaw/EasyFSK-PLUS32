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

void test_tci_audio_controls_match_backend_ranges() {
  assertContains("id=\"tciEnabled\"");
  assertContains("id=\"tciMarkHz\" min=\"300\" max=\"3000\"");
  assertContains("id=\"tciSpaceHz\" min=\"300\" max=\"3000\"");
  assertContains("<option value=\"48000\">48 kHz</option>");
  assertContains("id=\"tciSamples\" min=\"100\" max=\"2048\"");
  assertContains("Defaults to 2125/2295 Hz");
}

void test_tci_config_is_loaded_and_saved() {
  assertContains("var tci = cfg.tci || {}");
  assertContains("getElementById('tciMarkHz').value = tci.markHz || 2125");
  assertContains("getElementById('tciSpaceHz').value = tci.spaceHz || 2295");
  assertContains("enabled: document.getElementById('tciEnabled').checked");
  assertContains("sampleRate: parseInt(document.getElementById('tciSampleRate').value, 10)");
}

void test_adc_level_meter_and_overload_state_are_connected_to_websocket() {
  assertContains("id=\"adcMeter\"");
  assertContains("id=\"adcLevelMeter\" role=\"meter\"");
  assertContains("5% · RX noise reference");
  assertContains("id=\"adcOverloadWarning\"");
  assertContains("function applyAdc(data)");
  assertContains("else if (data.type === 'adc') applyAdc(data)");
  assertContains("data.clipped === true");
}

void test_cw_monitor_and_builtin_transmitter_are_available_in_cw_mode() {
  assertContains("id=\"nav-cw\" data-tab=\"cw\"");
  assertContains("id=\"tab-cw\"");
  assertContains("function applyMonitorMode(mode)");
  assertContains("applyMonitorMode(cfg.rxMonitorMode || 'rtty')");
  assertContains("id=\"cwPitchHz\" min=\"300\" max=\"1200\"");
  assertContains("id=\"cwDecoderText\"");
  assertContains("No live spectrum data is currently provided by the firmware.");
  assertContains("id=\"cwBtnSend\"");
  assertContains("id=\"cwBtnAbort\"");
  assertContains("fetch('/api/tx/send'");
  assertContains("JSON.stringify({ text: textInput.value, mode: 'cw' })");
  assertContains("Queued for CW transmission.");
  assertContains("id=\"rxMonitorMode\"");
  assertContains("id=\"rxModeStatus\"");
  assertContains("function saveMonitorMode(mode, openMonitor)");
  assertContains("localRxModeOverride = mode;");
  assertContains("if (openMonitor) activateTab(mode);");
  assertContains("Selected ' + mode.toUpperCase() +");
  assertContains("s.rxMonitorMode && localRxModeOverride === null");
  assertContains("body: JSON.stringify({ rxMonitorMode: mode })");
  assertContains("Independent of UART2");
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
  RUN_TEST(test_tci_audio_controls_match_backend_ranges);
  RUN_TEST(test_tci_config_is_loaded_and_saved);
  RUN_TEST(test_adc_level_meter_and_overload_state_are_connected_to_websocket);
  RUN_TEST(test_cw_monitor_and_builtin_transmitter_are_available_in_cw_mode);
  RUN_TEST(test_network_restart_requirement_is_visible_and_reported);
  RUN_TEST(test_storage_error_response_is_shown_to_user);
  return UNITY_END();
}
