// TOTP authenticator (RFC 6238) for Waveshare ESP32-S3-Touch-LCD-3.5-C.
// Time: NTP over Wi-Fi at boot, saved to the PCF85063 RTC as UTC. Without Wi-Fi the RTC is used.
// Accounts: tap [+] and point the OV5640 camera at an otpauth://totp QR code. Stored in NVS (Preferences).
// Delete: hold an account 1.5 s, then tap it again within 4 s.
// QR decoding: quirc (src/quirc, ISC license).
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include "esp_camera.h"
#include "mbedtls/md.h"
#include "TCA9554.h"
#include "TouchDrvFT6X36.hpp"
#include "SensorPCF85063.hpp"
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
#include "src/quirc/quirc.h"
#include "secrets.h"

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

#define NTP_TIMEOUT_MS 20000
#define TIME_VALID 1700000000  // anything earlier means the clock was never set
#define MAX_ACC 4
#define CAM_W 1280  // SXGA: lens is fixed-focus, so the QR must be held back (~30 cm) and still need pixels per module
#define CAM_H 1024
#define PREV_S 4  // preview = frame / 4 = 320x256
#define PREV_H (CAM_H / PREV_S)
#define SCAN_TIMEOUT_MS 120000

TCA9554 TCA(0x20);
TouchDrvFT6X36 touch;
SensorPCF85063 rtc;
XPowersPMU power;
Preferences prefs;
bool rtcOk, touchOk;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, SPI_SCLK, SPI_MOSI, SPI_MISO);
Arduino_GFX *gfx = new Arduino_ST7796(bus, LCD_RST, 0 /* rotation */, true, LCD_HOR_RES, LCD_VER_RES);

enum Algo : uint8_t { SHA1, SHA256, SHA512 };
static const mbedtls_md_type_t MD_TYPE[] = {MBEDTLS_MD_SHA1, MBEDTLS_MD_SHA256, MBEDTLS_MD_SHA512};
static const uint32_t POW10[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

struct Account {  // stored as-is in NVS; changing it invalidates saved accounts
  char label[27];  // 26 chars fit one line at text size 2
  uint8_t key[64];
  uint8_t keyLen;
  uint8_t digits;
  uint16_t period;
  uint8_t algo;
};
Account acc[MAX_ACC];
int nAcc = 0;
uint64_t lastCounter[MAX_ACC];
int armed = -1;  // row waiting for delete confirmation
uint32_t armedUntil = 0;
const char *timeSource = "no time";
bool testOk;

// After the globals: the macro defines a function, and Arduino inserts prototypes above the first one.
SET_LOOP_TASK_STACK_SIZE(32 * 1024);  // quirc_decode keeps ~9 KB on the stack

void lcd_reset() {
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);
}

// ---------- TOTP ----------

// RFC 4648 Base32, ignores spaces, '-' and '=' padding, case-insensitive. Returns byte count or -1.
int base32Decode(const char *in, uint8_t *out, int outMax) {
  uint32_t buf = 0;
  int bits = 0, n = 0;
  for (; *in; in++) {
    char c = toupper(*in);
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= '2' && c <= '7') v = c - '2' + 26;
    else if (c == ' ' || c == '-' || c == '=') continue;
    else return -1;
    buf = (buf << 5) | v;
    bits += 5;
    if (bits >= 8) {
      if (n >= outMax) return -1;
      out[n++] = buf >> (bits - 8);
      bits -= 8;
    }
  }
  return n;
}

// HOTP (RFC 4226), full 31-bit value; caller takes % 10^digits.
uint32_t hotp(const uint8_t *key, int len, uint8_t algo, uint64_t counter) {
  uint8_t msg[8], mac[64];
  for (int i = 7; i >= 0; i--, counter >>= 8) msg[i] = counter & 0xff;
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MD_TYPE[algo]);
  mbedtls_md_hmac(md, key, len, msg, 8, mac);
  int o = mac[mbedtls_md_get_size(md) - 1] & 0x0f;
  return ((mac[o] & 0x7f) << 24) | (mac[o + 1] << 16) | (mac[o + 2] << 8) | mac[o + 3];
}

// ---------- otpauth:// parsing (QR content is untrusted input) ----------

String urlDecode(const String &s) {
  String out;
  for (unsigned i = 0; i < s.length(); i++) {
    if (s[i] == '%' && i + 2 < s.length() && isxdigit(s[i + 1]) && isxdigit(s[i + 2])) {
      out += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

// Parses otpauth://totp/LABEL?secret=..&issuer=..&algorithm=..&digits=..&period=..
bool parseOtpauth(const char *uri, Account &a, const char **err) {
  if (strncasecmp(uri, "otpauth://totp/", 15) != 0) {
    *err = strncasecmp(uri, "otpauth-migration:", 18) == 0 ? "Export QR not supported" : "Not a TOTP QR code";
    return false;
  }
  String s(uri + 15);
  int q = s.indexOf('?');
  if (q < 0) { *err = "QR has no secret"; return false; }
  String label = urlDecode(s.substring(0, q)), issuer, secret, algo = "SHA1";
  long digits = 6, period = 30;

  String query = s.substring(q + 1);
  while (query.length()) {
    int amp = query.indexOf('&');
    String kv = amp < 0 ? query : query.substring(0, amp);
    query = amp < 0 ? "" : query.substring(amp + 1);
    int eq = kv.indexOf('=');
    if (eq < 0) continue;
    String k = kv.substring(0, eq), v = urlDecode(kv.substring(eq + 1));
    k.toLowerCase();
    if (k == "secret") secret = v;
    else if (k == "issuer") issuer = v;
    else if (k == "algorithm") { algo = v; algo.toUpperCase(); }
    else if (k == "digits") digits = v.toInt();
    else if (k == "period") period = v.toInt();
  }

  memset(&a, 0, sizeof(a));
  if (algo == "SHA1") a.algo = SHA1;
  else if (algo == "SHA256") a.algo = SHA256;
  else if (algo == "SHA512") a.algo = SHA512;
  else { *err = "Unknown algorithm"; return false; }
  if (digits < 6 || digits > 8) { *err = "Unsupported digits"; return false; }
  if (period < 1 || period > 3600) { *err = "Bad period"; return false; }
  int n = base32Decode(secret.c_str(), a.key, sizeof(a.key));
  if (n <= 0) { *err = "Bad secret"; return false; }
  a.keyLen = n;
  a.digits = digits;
  a.period = period;

  if (issuer.length() && !label.startsWith(issuer)) label = issuer + ":" + label;
  if (!label.length()) label = "(no name)";
  strlcpy(a.label, label.c_str(), sizeof(a.label));
  return true;
}

// RFC 6238 Appendix B vectors (8 digits) plus one parser case.
bool selfTest() {
  const struct { const char *seed; uint8_t algo; uint64_t t; uint32_t code; } v[] = {
    {"12345678901234567890", SHA1, 59, 94287082},
    {"12345678901234567890", SHA1, 1111111109, 7081804},
    {"12345678901234567890", SHA1, 20000000000ULL, 65353130},
    {"12345678901234567890123456789012", SHA256, 59, 46119246},
    {"1234567890123456789012345678901234567890123456789012345678901234", SHA512, 59, 90693936}};
  for (auto &x : v) {
    if (hotp((const uint8_t *)x.seed, strlen(x.seed), x.algo, x.t / 30) % 100000000 != x.code) return false;
  }
  Account a;
  const char *err;
  if (!parseOtpauth("otpauth://totp/john%40x.com?secret=JBSWY3DPEHPK3PXP&issuer=ACME%20Co&digits=8&period=60&algorithm=sha256", a, &err))
    return false;
  if (strcmp(a.label, "ACME Co:john@x.com") || a.digits != 8 || a.period != 60 || a.algo != SHA256 || a.keyLen != 10) return false;
  return !parseOtpauth("https://example.com", a, &err) && !parseOtpauth("otpauth://totp/x?secret=0189", a, &err);
}

// ---------- storage ----------

void loadAccounts() {
  size_t len = prefs.getBytesLength("acc");
  nAcc = (len % sizeof(Account) == 0) ? min((int)(len / sizeof(Account)), MAX_ACC) : 0;
  if (nAcc) prefs.getBytes("acc", acc, nAcc * sizeof(Account));
}

void saveAccounts() {
  if (nAcc) prefs.putBytes("acc", acc, nAcc * sizeof(Account));
  else prefs.remove("acc");
}

// Same label replaces the old entry (re-enrolment).
bool addAccount(const Account &a, const char **msg) {
  int i = 0;
  while (i < nAcc && strcmp(acc[i].label, a.label)) i++;
  if (i == MAX_ACC) { *msg = "Full: delete one first"; return false; }
  *msg = i < nAcc ? "Replaced" : "Added";
  acc[i] = a;
  if (i == nAcc) nAcc++;
  saveAccounts();
  return true;
}

// ---------- time ----------

void setSystemTime(time_t t) {
  timeval tv = {t, 0};
  settimeofday(&tv, nullptr);
}

// Try NTP; on success copy UTC into the RTC. Otherwise fall back to the RTC.
void syncTime() {
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    uint32_t start = millis();
    while (time(nullptr) < TIME_VALID && millis() - start < NTP_TIMEOUT_MS) delay(200);
    // Radio off once synced: the board needs no network to show codes.
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    if (time(nullptr) >= TIME_VALID) {
      timeSource = "NTP";
      if (rtcOk) {
        time_t now = time(nullptr);
        tm u;
        gmtime_r(&now, &u);
        rtc.setDateTime(u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
      }
      return;
    }
  }
  if (rtcOk) {
    RTC_DateTime d = rtc.getDateTime();
    tm u = {};
    u.tm_year = d.getYear() - 1900;
    u.tm_mon = d.getMonth() - 1;
    u.tm_mday = d.getDay();
    u.tm_hour = d.getHour();
    u.tm_min = d.getMinute();
    u.tm_sec = d.getSecond();
    setSystemTime(mktime(&u));  // TZ is UTC (configTime(0,0) / default), so mktime acts as timegm
    if (time(nullptr) >= TIME_VALID) timeSource = "RTC";
  }
}

// ---------- camera + QR scan ----------

bool cameraStart() {
  static bool pmuOk = false;
  if (!pmuOk) {
    // Camera rails per the repo's 03_camera_web_server: BLDO1 1.5 V, BLDO2 2.8 V.
    pmuOk = power.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
    if (!pmuOk) return false;
    power.setBLDO1Voltage(1500);
    power.setBLDO2Voltage(2800);
  }
  power.enableBLDO1();
  power.enableBLDO2();
  delay(50);

  camera_config_t c = {};
  c.pin_d0 = 45;
  c.pin_d1 = 47;
  c.pin_d2 = 48;
  c.pin_d3 = 46;
  c.pin_d4 = 42;
  c.pin_d5 = 40;
  c.pin_d6 = 39;
  c.pin_d7 = 21;
  c.pin_xclk = 38;
  c.pin_pclk = 41;
  c.pin_vsync = 17;
  c.pin_href = 18;
  c.pin_sccb_sda = -1;  // share the Wire bus (port 0), as the repo's esp-idf camera port does
  c.pin_sccb_scl = -1;
  c.sccb_i2c_port = 0;
  c.pin_pwdn = -1;
  c.pin_reset = -1;
  c.xclk_freq_hz = 20000000;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pixel_format = PIXFORMAT_GRAYSCALE;
  c.frame_size = FRAMESIZE_SXGA;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.fb_count = 2;
  c.grab_mode = CAMERA_GRAB_LATEST;
  esp_err_t e = esp_camera_init(&c);
  if (e != ESP_OK) {
    Serial.printf("camera init failed 0x%x\n", e);
    power.disableBLDO1();
    power.disableBLDO2();
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);
  return true;
}

void cameraStop() {
  esp_camera_deinit();
  power.disableBLDO1();
  power.disableBLDO2();
}

void scanStatus(const char *msg, uint16_t col) {
  gfx->fillRect(0, 370, 320, 30, BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(col, BLACK);
  gfx->setCursor(10, 375);
  gfx->print(msg);
}

// Blocks until a TOTP QR is saved, the user taps CANCEL, or the timeout hits.
void scanMode() {
  static quirc_code code;  // ~4 KB and ~9 KB: keep off the stack
  static quirc_data data;

  gfx->fillScreen(BLACK);
  gfx->setTextSize(3);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(10, 15);
  gfx->print("SCAN QR");
  gfx->setTextSize(2);
  gfx->setTextColor(LIGHTGREY, BLACK);
  gfx->setCursor(10, 55);
  gfx->print("Hold the QR ~30 cm away");
  gfx->setCursor(10, 80);
  gfx->print("so the preview is sharp");
  gfx->fillRoundRect(60, 415, 200, 50, 8, DARKGREY);
  gfx->setTextColor(WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(106, 428);
  gfx->print("CANCEL");

  if (!cameraStart()) {
    scanStatus("Camera init failed", RED);
    delay(2500);
    return;
  }
  quirc *qr = quirc_new();
  uint16_t *preview = (uint16_t *)ps_malloc(320 * PREV_H * 2);
  if (!qr || quirc_resize(qr, CAM_W, CAM_H) < 0 || !preview) {
    scanStatus("Out of memory", RED);
    delay(2500);
    if (qr) quirc_destroy(qr);
    free(preview);
    cameraStop();
    return;
  }

  scanStatus("Looking...", LIGHTGREY);
  uint32_t start = millis();
  bool done = false;
  while (!done && millis() - start < SCAN_TIMEOUT_MS) {
    int16_t tx, ty;
    if (touchOk && touch.getPoint(&tx, &ty, 1) && ty >= 405) break;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) continue;
    if (fb->width != CAM_W || fb->height != CAM_H) {
      esp_camera_fb_return(fb);
      continue;
    }
    uint8_t *img = quirc_begin(qr, nullptr, nullptr);
    memcpy(img, fb->buf, CAM_W * CAM_H);
    esp_camera_fb_return(fb);

    // Downscaled grey preview for aiming.
    for (int y = 0; y < PREV_H; y++) {
      const uint8_t *row = img + (y * PREV_S) * CAM_W;
      for (int x = 0; x < 320; x++) {
        uint8_t g = row[x * PREV_S];
        preview[y * 320 + x] = ((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3);
      }
    }
    gfx->draw16bitRGBBitmap(0, 108, preview, 320, PREV_H);

    // Diagnostics: brightness (0-255) and sharpness (mean |dx|) of the centre row band.
    static uint32_t frames = 0, lastLog = 0;
    frames++;
    uint32_t sum = 0, grad = 0;
    for (int y = CAM_H / 2 - 40; y < CAM_H / 2 + 40; y += 4)
      for (int x = 1; x < CAM_W; x++) {
        sum += img[y * CAM_W + x];
        grad += abs(img[y * CAM_W + x] - img[y * CAM_W + x - 1]);
      }
    const char *lastErr = "-";

    quirc_end(qr);
    for (int i = 0; i < quirc_count(qr) && !done; i++) {
      quirc_extract(qr, i, &code);
      quirc_decode_error_t e = quirc_decode(&code, &data);
      if (e == QUIRC_ERROR_DATA_ECC) {  // mirrored image
        quirc_flip(&code);
        e = quirc_decode(&code, &data);
      }
      if (e) {
        lastErr = quirc_strerror(e);
        continue;
      }
      data.payload[min(data.payload_len, QUIRC_MAX_PAYLOAD - 1)] = 0;
      Account a;
      const char *msg;
      if (!parseOtpauth((const char *)data.payload, a, &msg)) {
        scanStatus(msg, ORANGE);
        continue;
      }
      done = addAccount(a, &msg);
      char buf[40];
      snprintf(buf, sizeof(buf), "%s: %.20s", msg, a.label);
      scanStatus(done ? buf : msg, done ? GREEN : RED);
      Serial.printf("scan: %s\n", done ? buf : msg);  // label only, never the secret
      delay(done ? 1500 : 2500);
    }
    if (millis() - lastLog >= 1000) {
      uint32_t px = 20 * (CAM_W - 1);
      Serial.printf("scan: %lu fps, bright %lu, sharp %lu.%lu, qr found %d, err %s\n", (unsigned long)frames,
                    (unsigned long)(sum / px), (unsigned long)(grad / px), (unsigned long)(grad * 10 / px % 10),
                    quirc_count(qr), lastErr);
      frames = 0;
      lastLog = millis();
    }
  }
  memset(quirc_begin(qr, nullptr, nullptr), 0, CAM_W * CAM_H);  // don't leave the secret QR image in RAM
  quirc_destroy(qr);
  free(preview);
  memset(&data, 0, sizeof(data));
  cameraStop();
}

// ---------- main screen ----------

int rowY(int i) { return 50 + i * 100; }

int rowAt(int y) {
  if (y < 50) return -1;
  int r = (y - 50) / 100;
  return r < nAcc ? r : -1;
}

void drawLabel(int i) {
  gfx->fillRect(0, rowY(i), 320, 18, BLACK);
  gfx->setTextSize(2);
  gfx->setCursor(10, rowY(i));
  if (i == armed) {
    gfx->setTextColor(RED, BLACK);
    gfx->print("TAP AGAIN TO DELETE");
  } else {
    gfx->setTextColor(LIGHTGREY, BLACK);
    gfx->print(acc[i].label);
  }
}

void drawCode(int i, uint64_t counter) {
  const Account &a = acc[i];
  uint32_t c = hotp(a.key, a.keyLen, a.algo, counter) % POW10[a.digits];
  char buf[12];
  if (a.digits == 6) snprintf(buf, sizeof(buf), "%03lu %03lu", (unsigned long)(c / 1000), (unsigned long)(c % 1000));
  else snprintf(buf, sizeof(buf), "%0*lu", a.digits, (unsigned long)c);
  gfx->fillRect(0, rowY(i) + 24, 320, 48, BLACK);
  gfx->setTextSize(a.digits == 6 ? 6 : 5);
  gfx->setTextColor(WHITE, BLACK);
  gfx->setCursor(10, rowY(i) + 24);
  gfx->print(buf);
}

void drawBar(int i, int left) {
  int p = acc[i].period;
  int y = rowY(i) + 80;
  gfx->fillRect(10, y, 300, 6, DARKGREY);
  gfx->fillRect(10, y, 300 * left / p, 6, left <= 5 ? RED : CYAN);
}

void drawMain() {
  gfx->fillScreen(BLACK);
  gfx->setTextSize(2);
  gfx->setCursor(10, 15);
  gfx->setTextColor(testOk ? GREEN : RED, BLACK);
  gfx->print(testOk ? "TOTP" : "SELFTEST FAIL");
  gfx->setTextColor(DARKGREY, BLACK);
  gfx->setCursor(170, 15);
  gfx->print(timeSource);
  gfx->fillRoundRect(266, 4, 48, 38, 6, nAcc < MAX_ACC ? BLUE : DARKGREY);
  gfx->setTextSize(4);
  gfx->setTextColor(WHITE);
  gfx->setCursor(279, 8);
  gfx->print("+");

  if (!nAcc) {
    gfx->setTextSize(2);
    gfx->setTextColor(LIGHTGREY, BLACK);
    gfx->setCursor(10, 200);
    gfx->print("No accounts yet.");
    gfx->setCursor(10, 230);
    gfx->print("Tap + to scan a QR code.");
  }
  for (int i = 0; i < nAcc; i++) {
    drawLabel(i);
    lastCounter[i] = UINT64_MAX;  // force code redraw
  }
}

void onTap(int x, int y) {
  if (x >= 260 && y < 45) {
    armed = -1;
    if (nAcc < MAX_ACC) scanMode();
    drawMain();
    return;
  }
  int r = rowAt(y);
  if (armed >= 0 && r == armed) {
    char label[27];
    strlcpy(label, acc[r].label, sizeof(label));
    memmove(&acc[r], &acc[r + 1], (nAcc - r - 1) * sizeof(Account));
    nAcc--;
    memset(&acc[nAcc], 0, sizeof(Account));
    saveAccounts();
    Serial.printf("deleted: %s\n", label);
    armed = -1;
    drawMain();
  } else if (armed >= 0) {
    int was = armed;
    armed = -1;
    drawLabel(was);
  }
}

void onLongPress(int y) {
  int r = rowAt(y);
  if (r < 0) return;
  if (armed >= 0) {
    int was = armed;
    armed = -1;
    drawLabel(was);
  }
  armed = r;
  armedUntil = millis() + 4000;
  drawLabel(r);
}

void pollTouch() {
  static bool down = false, longFired = false;
  static uint32_t t0;
  static int16_t sx, sy;
  int16_t x, y;
  bool pressed = touchOk && touch.getPoint(&x, &y, 1);
  if (pressed && !down) {
    down = true;
    longFired = false;
    t0 = millis();
    sx = x;
    sy = y;
  }
  if (pressed && down && !longFired && millis() - t0 >= 1500) {
    longFired = true;
    onLongPress(sy);
  }
  if (!pressed && down) {
    down = false;
    if (!longFired) onTap(sx, sy);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("totp start");

  Wire.begin(I2C_SDA, I2C_SCL);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  lcd_reset();
  touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS);

  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  gfx->fillScreen(BLACK);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
  gfx->setTextSize(2);
  gfx->setTextColor(LIGHTGREY);
  gfx->setCursor(10, 15);
  gfx->print("syncing time...");

  testOk = selfTest();
  Serial.println(testOk ? "self-test OK" : "self-test FAILED");

  prefs.begin("totp", false);
  loadAccounts();
  Serial.printf("%d account(s)\n", nAcc);

  rtcOk = rtc.begin(Wire);
  syncTime();
  Serial.printf("time source: %s, epoch %ld\n", timeSource, (long)time(nullptr));

  if (time(nullptr) < TIME_VALID) {
    gfx->fillScreen(BLACK);
    gfx->setTextSize(3);
    gfx->setTextColor(RED, BLACK);
    gfx->setCursor(10, 200);
    gfx->print("No valid time.");
    gfx->setTextSize(2);
    gfx->setCursor(10, 240);
    gfx->print("Check Wi-Fi, then reset.");
    while (true) delay(1000);  // wrong codes are worse than none
  }
  drawMain();
}

void loop() {
  static time_t lastSec = 0;
  pollTouch();

  if (armed >= 0 && (int32_t)(millis() - armedUntil) > 0) {
    int was = armed;
    armed = -1;
    drawLabel(was);
  }

  time_t now = time(nullptr);
  if (now != lastSec) {
    lastSec = now;
    for (int i = 0; i < nAcc; i++) {
      uint64_t counter = now / acc[i].period;
      if (counter != lastCounter[i]) {
        lastCounter[i] = counter;
        drawCode(i, counter);
      }
      drawBar(i, acc[i].period - now % acc[i].period);
    }
  }
  delay(20);
}
