#include "StatusDisplay.h"
#include "Pins.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

namespace StatusDisplay {

namespace {
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
uint8_t s_txCol = 0;
} // namespace

void begin() {
  Wire.begin(LCD_SDA_PIN, LCD_SCL_PIN);
  lcd.init();
  lcd.backlight();
}

void showSplash(const char *callsign, const char *fwVersion, uint16_t pttLeadMs,
                 uint16_t pttTailMs) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("EasyFSK-PLUS32");
  lcd.setCursor(0, 1);
  lcd.print(callsign[0] != '\0' ? callsign : "by QRO.CZ");
  delay(1500);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Firmware:");
  lcd.setCursor(0, 1);
  lcd.print(fwVersion);
  delay(1000);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("PTT lead:");
  lcd.print(pttLeadMs);
  lcd.print("ms");
  lcd.setCursor(0, 1);
  lcd.print("PTT tail:");
  lcd.print(pttTailMs);
  lcd.print("ms");
  delay(1000);

  lcd.clear();
}

void showStatus(const char *line2) {
  lcd.setCursor(0, 1);
  lcd.print("                "); // clear 16 cols
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void refreshLine1(const char *callsign, const char *sourceLabel, bool markHigh) {
  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 0);
  lcd.print(callsign[0] != '\0' ? callsign : "------");
  lcd.setCursor(7, 0);
  lcd.print(sourceLabel);
  lcd.setCursor(15, 0);
  lcd.print(markHigh ? "H" : "L");
}

void resetTxLine() {
  s_txCol = 0;
  lcd.setCursor(0, 1);
  lcd.print("                ");
}

void appendTxChar(uint8_t asciiByte) {
  if (asciiByte < 0x20 || asciiByte > 0x7E) {
    return; // skip non-printable bytes (CR/LF etc.), matches AVR's practical behavior
  }
  if (s_txCol >= LCD_COLS) {
    s_txCol = 0;
    lcd.setCursor(0, 1);
    lcd.print("                ");
  }
  lcd.setCursor(s_txCol, 1);
  lcd.print(static_cast<char>(asciiByte));
  s_txCol++;
}

} // namespace StatusDisplay
