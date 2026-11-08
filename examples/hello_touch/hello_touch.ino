// Hello + touch test for Waveshare ESP32-S3-Touch-LCD-3.5(-C).
// Pins and drivers copied from the repo's 08_gfx_helloworld and 11_lvgl_arduino_v8.
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include "TouchDrvFT6X36.hpp"

#define GFX_BL 6

#define SPI_MISO 2
#define SPI_MOSI 1
#define SPI_SCLK 5

#define LCD_CS -1
#define LCD_DC 3
#define LCD_RST -1
#define LCD_HOR_RES 320
#define LCD_VER_RES 480

#define I2C_SDA 8
#define I2C_SCL 7

TCA9554 TCA(0x20);
TouchDrvFT6X36 touch;
bool touchOk = false;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_GFX *gfx = new Arduino_ST7796(bus, LCD_RST, 0 /* rotation */, true, LCD_HOR_RES, LCD_VER_RES);

// LCD reset line is on TCA9554 pin 1, not a GPIO.
void lcd_reset() {
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);
}

void setup() {
  Serial.begin(115200);
  Serial.println("hello_touch start");

  Wire.begin(I2C_SDA, I2C_SCL);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  lcd_reset();

  touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS);
  Serial.println(touchOk ? "FT6X36 touch OK" : "FT6X36 touch NOT found");

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  gfx->fillScreen(BLACK);

  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(20, 20);
  gfx->println("Hello World!");
  gfx->setTextSize(2);
  gfx->setCursor(20, 70);
  gfx->println(touchOk ? "Touch: OK" : "Touch: NOT found");
  gfx->setCursor(20, 100);
  gfx->println("Tap the screen");
}

void loop() {
  if (!touchOk) return;

  int16_t x[1], y[1];
  if (touch.getPoint(x, y, 1)) {
    Serial.printf("touch x=%d y=%d\n", x[0], y[0]);
    gfx->fillCircle(x[0], y[0], 4, GREEN);
    gfx->fillRect(20, 440, 280, 20, BLACK);
    gfx->setCursor(20, 440);
    gfx->setTextColor(YELLOW);
    gfx->printf("x=%d y=%d", x[0], y[0]);
  }
  delay(20);
}
