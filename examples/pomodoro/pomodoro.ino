// Pomodoro timer for Waveshare ESP32-S3-Touch-LCD-3.5(-C).
// Display/touch base from hello_touch; beep via ES8311 + I2S as in the repo's 04_es8311_example.
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include "TouchDrvFT6X36.hpp"
#include "ESP_I2S.h"
#include "esp_check.h"
#include "es8311.h"

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

#define I2S_MCK_PIN 12
#define I2S_BCK_PIN 13
#define I2S_LRCK_PIN 15
#define I2S_DOUT_PIN 16
#define I2S_DIN_PIN 14
#define SAMPLE_RATE 16000
#define MCLK_FREQ_HZ (SAMPLE_RATE * 256)
#define VOICE_VOLUME 70

// Durations in minutes.
#define FOCUS_MIN 25
#define SHORT_BREAK_MIN 5
#define LONG_BREAK_MIN 15
#define ROUNDS_BEFORE_LONG 4

static const char *TAG = "pomodoro";

TCA9554 TCA(0x20);
TouchDrvFT6X36 touch;
I2SClass i2s;
bool touchOk = false, audioOk = false;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_GFX *gfx = new Arduino_ST7796(bus, LCD_RST, 0 /* rotation */, true, LCD_HOR_RES, LCD_VER_RES);

enum Mode { FOCUS, SHORT_BREAK, LONG_BREAK };
Mode mode = FOCUS;
bool running = false;
uint32_t totalMs, remainingMs, lastTick;
int roundsDone = 0;  // finished focus sessions in the current set of 4

struct Btn { int16_t x, y, w, h; };
const Btn BTN_START = {10, 310, 300, 70};
const Btn BTN_RESET = {10, 395, 96, 70};
const Btn BTN_SKIP = {112, 395, 96, 70};
const Btn BTN_TEST = {214, 395, 96, 70};

void lcd_reset() {
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);
}

static esp_err_t audio_init() {
  es8311_handle_t es = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  ESP_RETURN_ON_FALSE(es, ESP_FAIL, TAG, "es8311 create failed");
  const es8311_clock_config_t clk = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = MCLK_FREQ_HZ,
    .sample_frequency = SAMPLE_RATE
  };
  ESP_RETURN_ON_ERROR(es8311_init(es, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "es8311 init failed");
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(es, VOICE_VOLUME, NULL), TAG, "volume failed");
  i2s.setPins(I2S_BCK_PIN, I2S_LRCK_PIN, I2S_DOUT_PIN, I2S_DIN_PIN, I2S_MCK_PIN);
  ESP_RETURN_ON_FALSE(i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH),
                      ESP_FAIL, TAG, "i2s begin failed");
  return ESP_OK;
}

// Blocking sine tone; fine at session end when nothing else runs.
void playTone(int freq, int ms) {
  if (!audioOk) return;
  int16_t buf[2 * 160];  // 10 ms stereo at 16 kHz
  int chunks = ms / 10;
  uint32_t phase = 0;
  for (int c = 0; c < chunks; c++) {
    for (int i = 0; i < 160; i++, phase++) {
      int16_t s = (int16_t)(8000 * sinf(2 * PI * freq * phase / SAMPLE_RATE));
      buf[2 * i] = buf[2 * i + 1] = s;
    }
    i2s.write((uint8_t *)buf, sizeof(buf));
  }
}

void beepDone() {
  for (int i = 0; i < 3; i++) {
    playTone(880, 150);
    delay(100);
  }
}

uint16_t modeColor() {
  return mode == FOCUS ? RED : (mode == SHORT_BREAK ? GREEN : CYAN);
}

uint32_t modeMs(Mode m) {
  uint32_t min = m == FOCUS ? FOCUS_MIN : (m == SHORT_BREAK ? SHORT_BREAK_MIN : LONG_BREAK_MIN);
  return min * 60UL * 1000UL;
}

void drawButton(const Btn &b, const char *label, uint16_t color) {
  gfx->fillRoundRect(b.x, b.y, b.w, b.h, 12, color);
  gfx->setTextSize(3);
  gfx->setTextColor(BLACK);
  int tw = strlen(label) * 18;
  gfx->setCursor(b.x + (b.w - tw) / 2, b.y + (b.h - 24) / 2);
  gfx->print(label);
}

void drawStartButton() {
  drawButton(BTN_START, running ? "PAUSE" : "START", running ? ORANGE : modeColor());
}

void drawTime() {
  char buf[8];
  uint32_t s = (remainingMs + 999) / 1000;  // round up so 0:00 only shows at the end
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  gfx->setTextSize(10);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(10, 110);
  gfx->print(buf);

  int fill = (int)(300ULL * (totalMs - remainingMs) / totalMs);
  gfx->fillRect(10 + fill, 230, 300 - fill, 16, DARKGREY);
  gfx->fillRect(10, 230, fill, 16, modeColor());
}

void drawAll() {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(3);
  gfx->setTextColor(modeColor());
  const char *label = mode == FOCUS ? "FOCUS" : (mode == SHORT_BREAK ? "SHORT BREAK" : "LONG BREAK");
  gfx->setCursor((320 - strlen(label) * 18) / 2, 40);
  gfx->print(label);

  for (int i = 0; i < ROUNDS_BEFORE_LONG; i++) {
    int cx = 160 - (ROUNDS_BEFORE_LONG - 1) * 20 + i * 40;
    if (i < roundsDone) gfx->fillCircle(cx, 275, 10, RED);
    else gfx->drawCircle(cx, 275, 10, DARKGREY);
  }

  drawTime();
  drawStartButton();
  drawButton(BTN_RESET, "RESET", LIGHTGREY);
  drawButton(BTN_SKIP, "SKIP", LIGHTGREY);
  drawButton(BTN_TEST, "TEST", audioOk ? YELLOW : DARKGREY);
}

void setMode(Mode m) {
  mode = m;
  totalMs = remainingMs = modeMs(m);
  running = false;
  drawAll();
}

// Move to the next session. finished = true when the timer ran out (counts the round).
void nextSession(bool finished) {
  if (mode == FOCUS) {
    if (finished) roundsDone++;
    if (roundsDone >= ROUNDS_BEFORE_LONG) setMode(LONG_BREAK);
    else setMode(SHORT_BREAK);
  } else {
    if (mode == LONG_BREAK) roundsDone = 0;
    setMode(FOCUS);
  }
}

bool hit(const Btn &b, int16_t x, int16_t y) {
  return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
}

void handleTap(int16_t x, int16_t y) {
  if (hit(BTN_START, x, y)) {
    running = !running;
    lastTick = millis();
    drawStartButton();
  } else if (hit(BTN_RESET, x, y)) {
    setMode(mode);
  } else if (hit(BTN_SKIP, x, y)) {
    nextSession(false);
  } else if (hit(BTN_TEST, x, y)) {
    Serial.println(audioOk ? "Speaker test" : "Speaker test: audio init failed");
    beepDone();  // timer keeps correct time: loop() uses the millis() delta
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("pomodoro start");

  Wire.begin(I2C_SDA, I2C_SCL);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  lcd_reset();
  // EXIO7 = PA_CTRL (NS4150B speaker amp enable) per the V2 schematic; factory firmware drives it high.
  TCA.pinMode1(7, OUTPUT);
  TCA.write1(7, 1);

  touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS);
  Serial.println(touchOk ? "FT6X36 touch OK" : "FT6X36 touch NOT found");

  audioOk = audio_init() == ESP_OK;
  Serial.println(audioOk ? "Audio OK" : "Audio init failed (no beep)");
  playTone(1320, 80);  // short boot chirp confirms the speaker works

  if (!gfx->begin()) {
    Serial.println("gfx->begin() failed!");
  }
  setMode(FOCUS);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
}

void loop() {
  static bool wasTouched = false;
  if (touchOk) {
    int16_t x[1], y[1];
    bool touched = touch.getPoint(x, y, 1);
    if (touched && !wasTouched) handleTap(x[0], y[0]);  // act on press only, not hold
    wasTouched = touched;
  }

  if (running) {
    uint32_t now = millis();
    uint32_t dt = now - lastTick;
    lastTick = now;
    uint32_t prevSec = (remainingMs + 999) / 1000;
    remainingMs = dt >= remainingMs ? 0 : remainingMs - dt;
    if (remainingMs == 0) {
      drawTime();
      beepDone();
      nextSession(true);
    } else if ((remainingMs + 999) / 1000 != prevSec) {
      drawTime();
    }
  }
  delay(20);
}
