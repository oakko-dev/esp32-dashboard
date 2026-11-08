// Clock + sensors for Waveshare ESP32-S3-Touch-LCD-3.5(-C).
// Display/touch base from hello_touch; RTC, IMU and PMU usage from the repo's
// 05_pcf85063_time, 06_qmi8658_getdata and 02_axp2101_example.
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include "SensorPCF85063.hpp"
#include "SensorQMI8658.hpp"
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"

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
SensorPCF85063 rtc;
SensorQMI8658 qmi;
XPowersPMU power;
bool rtcOk, imuOk, pmuOk;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_GFX *gfx = new Arduino_ST7796(bus, LCD_RST, 0 /* rotation */, true, LCD_HOR_RES, LCD_VER_RES);

static const char *WDAY[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

void lcd_reset() {
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);
}

// Day of week, 0 = Sunday (Sakamoto's method).
int dayOfWeek(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// If the RTC is behind the sketch build time (e.g. first boot, dead backup cell),
// set it to the build time. shortcut: clock is only as accurate as the compile time, add NTP over Wi-Fi if needed.
void syncRtcToBuildTime() {
  static const char *MON = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char m[4];
  int d, y, hh, mm, ss;
  sscanf(__DATE__, "%3s %d %d", m, &d, &y);
  sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
  int mon = (strstr(MON, m) - MON) / 3 + 1;

  RTC_DateTime now = rtc.getDateTime();
  uint64_t rtcKey = (uint64_t)now.getYear() * 10000000000ULL + now.getMonth() * 100000000ULL +
                    now.getDay() * 1000000ULL + now.getHour() * 10000ULL + now.getMinute() * 100ULL + now.getSecond();
  uint64_t buildKey = (uint64_t)y * 10000000000ULL + mon * 100000000ULL + d * 1000000ULL + hh * 10000ULL + mm * 100ULL + ss;
  if (rtcKey < buildKey) {
    rtc.setDateTime(y, mon, d, hh, mm, ss);
    Serial.println("RTC set to build time");
  }
}

void drawLabels() {
  gfx->fillScreen(BLACK);
  gfx->drawFastHLine(10, 175, 300, DARKGREY);
  gfx->setTextSize(2);
  gfx->setTextColor(DARKGREY, BLACK);
  gfx->setCursor(10, 190); gfx->print("ACCEL (g)");
  gfx->setCursor(10, 280); gfx->print("GYRO (dps)");
  gfx->setCursor(10, 370); gfx->print("IMU TEMP");
  gfx->setCursor(170, 370); gfx->print("BATTERY");
}

void drawClock() {
  char buf[32];
  if (!rtcOk) {
    gfx->setTextSize(3);
    gfx->setTextColor(RED, BLACK);
    gfx->setCursor(10, 60);
    gfx->print("RTC not found");
    return;
  }
  RTC_DateTime t = rtc.getDateTime();

  gfx->setTextColor(WHITE, BLACK);
  gfx->setTextSize(8);
  gfx->setCursor(10, 30);
  snprintf(buf, sizeof(buf), "%02d:%02d", t.getHour(), t.getMinute());
  gfx->print(buf);

  gfx->setTextColor(CYAN, BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(262, 75);
  snprintf(buf, sizeof(buf), "%02d", t.getSecond());
  gfx->print(buf);

  gfx->setTextColor(LIGHTGREY, BLACK);
  gfx->setTextSize(3);
  gfx->setCursor(40, 120);
  snprintf(buf, sizeof(buf), "%s %04d-%02d-%02d", WDAY[dayOfWeek(t.getYear(), t.getMonth(), t.getDay())],
           t.getYear(), t.getMonth(), t.getDay());
  gfx->print(buf);
}

// Fixed-width fields so each redraw fully overwrites the previous value.
void drawXYZ(int y, float x, float yv, float z, const char *fmt) {
  char buf[16];
  gfx->setTextSize(2);
  gfx->setTextColor(GREEN, BLACK);
  float v[3] = {x, yv, z};
  for (int i = 0; i < 3; i++) {
    snprintf(buf, sizeof(buf), fmt, v[i]);
    gfx->setCursor(10 + i * 102, y);
    gfx->print(buf);
  }
}

void drawSensors() {
  char buf[24];
  float ax, ay, az, gx, gy, gz;
  if (imuOk && qmi.getDataReady() && qmi.getAccelerometer(ax, ay, az) && qmi.getGyroscope(gx, gy, gz)) {
    drawXYZ(220, ax, ay, az, "%+7.2f");
    drawXYZ(310, gx, gy, gz, "%+7.1f");
    gfx->setTextSize(3);
    gfx->setTextColor(ORANGE, BLACK);
    gfx->setCursor(10, 400);
    snprintf(buf, sizeof(buf), "%5.1fC", qmi.getTemperature_C());
    gfx->print(buf);
  } else if (!imuOk) {
    gfx->setTextSize(2);
    gfx->setTextColor(RED, BLACK);
    gfx->setCursor(10, 220);
    gfx->print("IMU not found");
  }

  gfx->setTextSize(3);
  gfx->setTextColor(YELLOW, BLACK);
  gfx->setCursor(170, 400);
  if (!pmuOk) {
    gfx->print("n/a  ");
  } else if (power.isBatteryConnect()) {
    snprintf(buf, sizeof(buf), "%3d%%%s", power.getBatteryPercent(), power.isCharging() ? "+" : " ");
    gfx->print(buf);
    gfx->setTextSize(2);
    gfx->setCursor(170, 435);
    snprintf(buf, sizeof(buf), "%4umV", power.getBattVoltage());
    gfx->print(buf);
  } else {
    gfx->print("USB  ");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("clock_sensors start");

  Wire.begin(I2C_SDA, I2C_SCL);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  lcd_reset();

  rtcOk = rtc.begin(Wire);
  Serial.println(rtcOk ? "PCF85063 OK" : "PCF85063 NOT found");
  if (rtcOk) syncRtcToBuildTime();

  imuOk = qmi.begin(Wire, QMI8658_L_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  Serial.println(imuOk ? "QMI8658 OK" : "QMI8658 NOT found");
  if (imuOk) {
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_62_5Hz, SensorQMI8658::LPF_MODE_0);
    qmi.configGyroscope(SensorQMI8658::GYR_RANGE_256DPS, SensorQMI8658::GYR_ODR_56_05Hz, SensorQMI8658::LPF_MODE_3);
    qmi.enableGyroscope();
    qmi.enableAccelerometer();
  }

  pmuOk = power.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  Serial.println(pmuOk ? "AXP2101 OK" : "AXP2101 NOT found");
  if (pmuOk) {
    // Only enable measurements; power rails are left as configured at boot.
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
  }

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  drawLabels();
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
}

void loop() {
  static uint32_t lastClock = 0, lastSensors = 0;
  uint32_t now = millis();
  if (now - lastClock >= 1000 || lastClock == 0) {
    lastClock = now;
    drawClock();
  }
  if (now - lastSensors >= 250) {
    lastSensors = now;
    drawSensors();
  }
  delay(10);
}
