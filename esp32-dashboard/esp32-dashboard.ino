// ESP32 dashboard (Claude + Cursor quota, system stats) for Waveshare ESP32-S3-Touch-LCD-3.5(-C). Standalone: no PC needed.
// Every 2 minutes the board sends a 1-token request to the Claude Messages API with a
// `claude setup-token` token and reads the usage from the response headers:
// anthropic-ratelimit-unified-{5h,7d}-{utilization (0..1), reset (Unix epoch)}.
// Cursor: every 5 minutes GET cursor.com/api/usage-summary with a cookie built from the session JWT.
// The JWT comes from a QR-code login on the board (stored in NVS) and is renewed every ~10 days.
// UI: LVGL 8.4 scrolling tab bar on top (needs config/lv_conf.h copied to Arduino/libraries/, with Montserrat 12/14/20/36).
// To add a tab: write build/refresh functions and add one line to PAGES[] below.
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <time.h>
#include <Preferences.h>
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"
#include "TCA9554.h"
#include "TouchDrvFT6X36.hpp"
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
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

#define HOSTNAME "esp32-dashboard"
#define STALE_MS (10UL * 60UL * 1000UL)  // grey out data older than 10 min
#define POLL_MS (2UL * 60UL * 1000UL)    // each poll spends ~25 input + 1 output token
#define RETRY_MS (60UL * 1000UL)

#define API_URL "https://api.anthropic.com/v1/messages"
// shortcut: hardcoded model; if it is retired the API returns 404 - change it here.
#define API_MODEL "claude-haiku-5-5"

// Root CA for api.anthropic.com (Google Trust Services GTS Root R4, valid to 2036-06-22).
// shortcut: single pinned root; if Anthropic changes CA, TLS fails ("API error -1") - add the new root.
static const char GTS_ROOT_R4[] = R"PEM(-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)PEM";

#define CURSOR_URL "https://cursor.com/api/usage-summary"
#define CURSOR_POLL_MS (5UL * 60UL * 1000UL)
#define CURSOR_STALE_MS (20UL * 60UL * 1000UL)
#define CURSOR_API "https://api2.cursor.sh"
#define CURSOR_CLIENT_ID "KbZUR41cY7W6zRSdpSUJ7I7mLYBKOCmB"  // Cursor's public OAuth client id (from OpenUsage)
#define CURSOR_REFRESH_LEFT (50L * 86400L)  // tokens last 60 days; renew once fewer than 50 remain
#define LOGIN_POLL_MS 3000UL
#define LOGIN_TTL_MS (5UL * 60UL * 1000UL)  // Cursor waits ~5 min for a login, then a new QR is made

// Roots for api2.cursor.sh (Amazon Root CA 1, valid to 2038; Starfield G2 for the cross-signed chain).
static const char AMAZON_ROOTS[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF
ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6
b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL
MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv
b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj
ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM
9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw
IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6
VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L
93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm
jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC
AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA
A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI
U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs
N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv
o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU
5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy
rqXRfboQnoZsG4q5WTP468SQvvG5
-----END CERTIFICATE-----
-----BEGIN CERTIFICATE-----
MIID7zCCAtegAwIBAgIBADANBgkqhkiG9w0BAQsFADCBmDELMAkGA1UEBhMCVVMx
EDAOBgNVBAgTB0FyaXpvbmExEzARBgNVBAcTClNjb3R0c2RhbGUxJTAjBgNVBAoT
HFN0YXJmaWVsZCBUZWNobm9sb2dpZXMsIEluYy4xOzA5BgNVBAMTMlN0YXJmaWVs
ZCBTZXJ2aWNlcyBSb290IENlcnRpZmljYXRlIEF1dGhvcml0eSAtIEcyMB4XDTA5
MDkwMTAwMDAwMFoXDTM3MTIzMTIzNTk1OVowgZgxCzAJBgNVBAYTAlVTMRAwDgYD
VQQIEwdBcml6b25hMRMwEQYDVQQHEwpTY290dHNkYWxlMSUwIwYDVQQKExxTdGFy
ZmllbGQgVGVjaG5vbG9naWVzLCBJbmMuMTswOQYDVQQDEzJTdGFyZmllbGQgU2Vy
dmljZXMgUm9vdCBDZXJ0aWZpY2F0ZSBBdXRob3JpdHkgLSBHMjCCASIwDQYJKoZI
hvcNAQEBBQADggEPADCCAQoCggEBANUMOsQq+U7i9b4Zl1+OiFOxHz/Lz58gE20p
OsgPfTz3a3Y4Y9k2YKibXlwAgLIvWX/2h/klQ4bnaRtSmpDhcePYLQ1Ob/bISdm2
8xpWriu2dBTrz/sm4xq6HZYuajtYlIlHVv8loJNwU4PahHQUw2eeBGg6345AWh1K
Ts9DkTvnVtYAcMtS7nt9rjrnvDH5RfbCYM8TWQIrgMw0R9+53pBlbQLPLJGmpufe
hRhJfGZOozptqbXuNC66DQO4M99H67FrjSXZm86B0UVGMpZwh94CDklDhbZsc7tk
6mFBrMnUVN+HL8cisibMn1lUaJ/8viovxFUcdUBgF4UCVTmLfwUCAwEAAaNCMEAw
DwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMCAQYwHQYDVR0OBBYEFJxfAN+q
AdcwKziIorhtSpzyEZGDMA0GCSqGSIb3DQEBCwUAA4IBAQBLNqaEd2ndOxmfZyMI
bw5hyf2E3F/YNoHN2BtBLZ9g3ccaaNnRbobhiCPPE95Dz+I0swSdHynVv/heyNXB
ve6SbzJ08pGCL72CQnqtKrcgfU28elUSwhXqvfdqlS5sdJ/PHLTyxQGjhdByPq1z
qwubdQxtRbeOlKyWN7Wg0I8VRw7j6IPdj/3vQQF3zCepYoUz8jcI73HPdwbeyBkd
iEDPfUYd/x7H4c7/I9vG+o1VTqkC50cRRj70/b17KSa7qWFiNyi2LSr2EIZkyXCn
0q23KXB56jzaYyWf/Wi3MOxw+3WKt21gZ7IeyLnp2KhvAotnDU0mV3HaIPzBSlCN
sSi6
-----END CERTIFICATE-----
)PEM";

// Root CA for cursor.com (Let's Encrypt ISRG Root X1, valid to 2035-06-04).
// shortcut: single pinned root; if cursor.com changes CA, TLS fails ("Cursor: error -1") - add the new root.
static const char ISRG_ROOT_X1[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)PEM";

// Palette (dark slate + Claude coral accent).
#define COL_BG 0x0B0F17
#define COL_CARD 0x141A26
#define COL_BORDER 0x232B3B
#define COL_TEXT 0xF1F5F9
#define COL_MUTED 0x94A3B8
#define COL_DIM 0x64748B
#define COL_CORAL 0xD97757
#define COL_OK 0x2DD4BF
#define COL_WARN 0xFBBF24
#define COL_BAD 0xFB7185
#define COL_STALE 0x475569
#define COL_VIOLET 0xA78BFA  // secondary (inner) ring

TCA9554 TCA(0x20);
TouchDrvFT6X36 touch;
XPowersPMU power;
bool touchOk = false, pmuOk = false;

Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, SPI_SCLK, SPI_MOSI, SPI_MISO);
// Rotation 1 = landscape 480x320 (use 3 to flip it 180 degrees; then fix touchRead too).
Arduino_GFX *gfx = new Arduino_ST7796(bus, LCD_RST, 1 /* rotation */, true, LCD_HOR_RES, LCD_VER_RES);

struct Quota {
  float pct = -1;  // -1 = unknown
  time_t resets = 0;
};
Quota q5, q7;

// UI structs live up here: Arduino inserts function prototypes above the first function.
struct Pill {  // status pill: dot + text
  lv_obj_t *dot, *text;
};
struct QuotaCard {  // pace-bar card: big % left, main bar with pace mark, secondary bar, pace status
  lv_obj_t *pct, *unit, *badge, *l1, *r1, *bar1, *pace, *l2, *r2, *bar2, *statusDot, *status;
};
struct CursorUsage {
  float total = -1, autoPct = -1, apiPct = -1;  // -1 = unknown
  time_t starts = 0, resets = 0;
  char plan[12] = "";
};
struct Tile {  // system metric tile
  lv_obj_t *value, *sub, *bar;
};

uint32_t lastUpdateMs = 0;
bool haveData = false, dirty = true;
volatile uint8_t wifiReason = 0;  // last STA disconnect reason (esp_wifi wifi_err_reason_t)
char scanInfo[40] = "scan pending";
char apiError[40] = "";  // last Claude API failure, shown on screen; empty = OK
CursorUsage cur;
uint32_t cursorUpdateMs = 0;
bool cursorHave = false;
char cursorError[40] = "";  // last Cursor failure; empty = OK
String cursorToken;  // Cursor session JWT from NVS; empty = QR login needed
char loginVerifier[48], loginUuid[40];
uint32_t loginStartMs = 0;
bool loginActive = false;
lv_obj_t *loginPanel, *loginQr, *loginStatus;

void lcd_reset() {
  TCA.write1(1, 1);
  delay(10);
  TCA.write1(1, 0);
  delay(10);
  TCA.write1(1, 1);
  delay(200);
}

// ---------- API ----------

// Fills q from the two headers; leaves it unchanged if the utilization header is missing.
void readWindow(HTTPClient &http, const char *utilHdr, const char *resetHdr, Quota &q) {
  String u = http.header(utilHdr);
  if (u.length() == 0) return;
  float p = u.toFloat() * 100.0f;
  if (p < 0 || p > 100) return;
  q.pct = p;
  long r = http.header(resetHdr).toInt();
  q.resets = r > 0 ? (time_t)r : 0;
}

// Returns true when usage headers were read.
bool pollUsage() {
  NetworkClientSecure tls;
  tls.setCACert(GTS_ROOT_R4);
  HTTPClient http;
  if (!http.begin(tls, API_URL)) {
    snprintf(apiError, sizeof(apiError), "API connect failed");
    return false;
  }
  http.setTimeout(15000);
  http.addHeader("Authorization", "Bearer " CLAUDE_TOKEN);
  http.addHeader("anthropic-version", "2023-06-01");
  http.addHeader("anthropic-beta", "oauth-2025-04-20");
  http.addHeader("Content-Type", "application/json");
  const char *keys[] = {
    "anthropic-ratelimit-unified-5h-utilization", "anthropic-ratelimit-unified-5h-reset",
    "anthropic-ratelimit-unified-7d-utilization", "anthropic-ratelimit-unified-7d-reset",
  };
  http.collectHeaders(keys, 4);

  // Subscription tokens are only accepted with Claude Code's identity system prompt.
  int code = http.POST("{\"model\":\"" API_MODEL "\",\"max_tokens\":1,"
                       "\"system\":\"You are Claude Code, Anthropic's official CLI for Claude.\","
                       "\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}");
  bool ok = false;
  if (code > 0 && http.header(keys[0]).length() + http.header(keys[2]).length() > 0) {
    readWindow(http, keys[0], keys[1], q5);
    readWindow(http, keys[2], keys[3], q7);
    apiError[0] = 0;
    ok = true;
  } else if (code == 401) {
    snprintf(apiError, sizeof(apiError), "Token rejected (401)");
  } else if (code > 0) {
    snprintf(apiError, sizeof(apiError), "API error %d", code);
  } else {
    snprintf(apiError, sizeof(apiError), "API error %d (TLS/net)", code);
  }
  Serial.printf("poll: HTTP %d, 5h=%.1f%% 7d=%.1f%% %s\n", code, q5.pct, q7.pct, apiError);
  http.end();
  return ok;
}

// ---------- Cursor ----------

// Number after "key": in a flat JSON text; -1 if missing or null.
float jsonNum(const String &b, const char *key) {
  int i = b.indexOf(String("\"") + key + "\":");
  if (i < 0) return -1;
  String v = b.substring(i + strlen(key) + 3, i + strlen(key) + 3 + 24);
  v.trim();
  return v.startsWith("null") ? -1 : v.toFloat();
}

// String value after "key":"; empty if missing.
String jsonStr(const String &b, const char *key) {
  int i = b.indexOf(String("\"") + key + "\":\"");
  if (i < 0) return "";
  int start = i + strlen(key) + 4;
  return b.substring(start, b.indexOf('"', start));
}

// "2026-12-02T03:03:33.000Z" -> Unix epoch (TZ is UTC, set by configTime(0, 0, ...)).
time_t isoToEpoch(const String &iso) {
  struct tm t = {};
  if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) return 0;
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  return mktime(&t);
}

// ---------- Cursor session (QR login + refresh) ----------
// Flow from OpenUsage (MIT): PKCE login via cursor.com/loginDeepControl + api2 auth/poll,
// renewal via api2 oauth/token. The session JWT is stored in NVS, never in secrets.h.

String b64url(const uint8_t *in, size_t n) {
  unsigned char out[64];
  size_t olen = 0;
  mbedtls_base64_encode(out, sizeof(out) - 1, &olen, in, n);
  out[olen] = 0;
  String s((char *)out);
  s.replace('+', '-');
  s.replace('/', '_');
  s.replace("=", "");
  return s;
}

// Decoded JWT payload (JSON text); empty if malformed.
String jwtPayload(const String &tok) {
  int a = tok.indexOf('.'), b = tok.indexOf('.', a + 1);
  if (a < 0 || b < 0) return "";
  String p = tok.substring(a + 1, b);
  p.replace('-', '+');
  p.replace('_', '/');
  while (p.length() % 4) p += '=';
  static unsigned char out[768];
  size_t olen = 0;
  if (mbedtls_base64_decode(out, sizeof(out) - 1, &olen, (const unsigned char *)p.c_str(), p.length()) != 0) return "";
  out[olen] = 0;
  return String((char *)out);
}

long jwtExp(const String &tok) {
  String p = jwtPayload(tok);
  int i = p.indexOf("\"exp\":");
  return i < 0 ? 0 : p.substring(i + 6).toInt();
}

// Dashboard cookie built from the session JWT: <user_id>%3A%3A<jwt>, user_id = part of "sub" after '|'.
String cursorCookie() {
  String sub = jsonStr(jwtPayload(cursorToken), "sub");
  return "WorkosCursorSessionToken=" + sub.substring(sub.indexOf('|') + 1) + "%3A%3A" + cursorToken;
}

void saveCursorToken(const String &t) {
  cursorToken = t;
  Preferences p;
  p.begin("cursor", false);
  if (t.length()) p.putString("tok", t);
  else p.remove("tok");
  p.end();
}

// New PKCE verifier + uuid, and the QR code of the login URL.
void startCursorLogin() {
  uint8_t r[32], h[32];
  esp_fill_random(r, sizeof(r));
  snprintf(loginVerifier, sizeof(loginVerifier), "%s", b64url(r, sizeof(r)).c_str());
  mbedtls_sha256((const unsigned char *)loginVerifier, strlen(loginVerifier), h, 0);
  esp_fill_random(r, 16);
  r[6] = (r[6] & 0x0F) | 0x40;  // UUID v4
  r[8] = (r[8] & 0x3F) | 0x80;
  snprintf(loginUuid, sizeof(loginUuid), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
  String url = "https://cursor.com/loginDeepControl?challenge=" + b64url(h, sizeof(h)) + "&uuid=" + loginUuid +
               "&mode=login&redirectTarget=cli";
  lv_qrcode_update(loginQr, url.c_str(), url.length());
  loginStartMs = millis();
  loginActive = true;
  Serial.println("cursor: login QR shown");
}

// One auth/poll check; true once the user has logged in (token saved).
bool pollCursorLogin() {
  NetworkClientSecure tls;
  tls.setCACert(AMAZON_ROOTS);
  HTTPClient http;
  if (!http.begin(tls, String(CURSOR_API "/auth/poll?uuid=") + loginUuid + "&verifier=" + loginVerifier)) return false;
  http.setTimeout(10000);
  http.setUserAgent("esp32-quota");
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String tok = jsonStr(http.getString(), "accessToken");
    if (tok.length()) {
      saveCursorToken(tok);
      loginActive = false;
      cursorError[0] = 0;
      ok = true;
    }
  }
  if (code != 404 && code != 202) Serial.printf("cursor: login poll HTTP %d %s\n", code, ok ? "logged in" : "");
  http.end();
  return ok;
}

// Trades the session JWT for a new one (60 days from now). Cursor answers with access_token only;
// that token is also the next refresh token. shouldLogout = session ended -> back to QR login.
bool refreshCursorToken() {
  NetworkClientSecure tls;
  tls.setCACert(AMAZON_ROOTS);
  HTTPClient http;
  if (!http.begin(tls, CURSOR_API "/oauth/token")) return false;
  http.setTimeout(15000);
  http.setUserAgent("esp32-quota");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(String("{\"grant_type\":\"refresh_token\",\"client_id\":\"" CURSOR_CLIENT_ID "\",\"refresh_token\":\"") +
                       cursorToken + "\"}");
  bool ok = false;
  if (code == 200) {
    String b = http.getString();
    int i = b.indexOf("\"shouldLogout\"");
    String tok = jsonStr(b, "access_token");
    if (i >= 0 && b.substring(i, i + 22).indexOf("true") > 0) saveCursorToken("");
    else if (tok.length()) {
      saveCursorToken(tok);
      ok = true;
    }
  }
  Serial.printf("cursor: refresh HTTP %d %s\n", code, ok ? "ok" : (cursorToken.length() ? "failed, keeping token" : "logged out"));
  http.end();
  return ok;
}

// Returns true when usage was read.
bool pollCursor() {
  if (jwtExp(cursorToken) - (long)time(nullptr) < CURSOR_REFRESH_LEFT) refreshCursorToken();
  if (cursorToken.isEmpty()) {
    snprintf(cursorError, sizeof(cursorError), "logged out");
    return false;
  }
  NetworkClientSecure tls;
  tls.setCACert(ISRG_ROOT_X1);
  HTTPClient http;
  if (!http.begin(tls, CURSOR_URL)) {
    snprintf(cursorError, sizeof(cursorError), "connect failed");
    return false;
  }
  http.setTimeout(15000);
  http.setUserAgent("esp32-quota");
  http.addHeader("Cookie", cursorCookie());
  http.addHeader("Accept", "application/json");
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    float total = jsonNum(body, "totalPercentUsed");
    if (total >= 0) {
      cur.total = constrain(total, 0, 100);
      cur.autoPct = jsonNum(body, "autoPercentUsed");
      cur.apiPct = jsonNum(body, "apiPercentUsed");
      cur.starts = isoToEpoch(jsonStr(body, "billingCycleStart"));
      cur.resets = isoToEpoch(jsonStr(body, "billingCycleEnd"));
      String plan = jsonStr(body, "membershipType");
      plan.toUpperCase();
      snprintf(cur.plan, sizeof(cur.plan), "%s", plan.c_str());
      cursorError[0] = 0;
      ok = true;
    } else {
      snprintf(cursorError, sizeof(cursorError), "no usage in reply");
    }
  } else if (code == 401) {
    snprintf(cursorError, sizeof(cursorError), "session ended (401)");
    saveCursorToken("");  // back to QR login
  } else {
    snprintf(cursorError, sizeof(cursorError), "error %d", code);
  }
  Serial.printf("cursor: HTTP %d, total=%.1f%% auto=%.1f%% api=%.1f%% %s\n", code, cur.total, cur.autoPct, cur.apiPct, cursorError);
  http.end();
  return ok;
}

// ---------- LVGL glue ----------

void flushCb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t *)&px->full, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
  lv_disp_flush_ready(drv);
}

void touchRead(lv_indev_drv_t *, lv_indev_data_t *d) {
  int16_t x, y;
  if (touchOk && touch.getPoint(&x, &y, 1)) {
    // The panel reports portrait coordinates (320x480); map them to rotation 1.
    d->point.x = y;
    d->point.y = LCD_HOR_RES - 1 - x;
    d->state = LV_INDEV_STATE_PR;
  } else {
    d->state = LV_INDEV_STATE_REL;
  }
}

// ---------- UI helpers ----------

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_label_set_text(l, text);
  return l;
}

lv_obj_t *makePanel(lv_obj_t *parent, int x, int y, int w, int h, int radius) {
  lv_obj_t *p = lv_obj_create(parent);
  lv_obj_set_size(p, w, h);
  lv_obj_set_pos(p, x, y);
  lv_obj_set_style_bg_color(p, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_color(p, lv_color_hex(COL_BORDER), 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_radius(p, radius, 0);
  lv_obj_set_style_pad_all(p, 0, 0);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  return p;
}

lv_obj_t *makeDot(lv_obj_t *parent, int size, uint32_t color) {
  lv_obj_t *d = lv_obj_create(parent);
  lv_obj_set_size(d, size, size);
  lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(d, lv_color_hex(color), 0);
  lv_obj_set_style_border_width(d, 0, 0);
  return d;
}

// Status pill (dot + text); caller positions it.
Pill makePill(lv_obj_t *parent) {
  lv_obj_t *p = lv_obj_create(parent);
  lv_obj_set_size(p, LV_SIZE_CONTENT, 24);
  lv_obj_set_style_radius(p, 12, 0);
  lv_obj_set_style_bg_color(p, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_color(p, lv_color_hex(COL_BORDER), 0);
  lv_obj_set_style_border_width(p, 1, 0);
  lv_obj_set_style_pad_hor(p, 10, 0);
  lv_obj_set_style_pad_ver(p, 0, 0);
  lv_obj_set_style_pad_column(p, 6, 0);
  lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
  Pill r;
  r.dot = makeDot(p, 8, COL_DIM);
  r.text = makeLabel(p, &lv_font_montserrat_12, COL_MUTED, "");
  return r;
}

// Transparent flex row at the bottom of a page.
lv_obj_t *makeFooterRow(lv_obj_t *page, int y, int w) {
  lv_obj_t *row = lv_obj_create(page);
  lv_obj_set_size(row, w, 28);
  lv_obj_set_pos(row, 12, y);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_set_style_pad_column(row, 10, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
}

void setPill(Pill &p, uint32_t color, const char *text) {
  lv_obj_set_style_bg_color(p.dot, lv_color_hex(color), 0);
  lv_label_set_text(p.text, text);
}

uint32_t levelColor(float pct) {
  return pct >= 80 ? COL_BAD : (pct >= 50 ? COL_WARN : COL_OK);
}

// ---------- Page: Quota (Claude + Cursor) ----------

QuotaCard claudeCard, cursorCard;
Pill quotaPill;
lv_obj_t *quotaFootL, *quotaFootR;

lv_obj_t *makeBar(lv_obj_t *card, int y, int h) {
  lv_obj_t *b = lv_bar_create(card);
  lv_obj_set_size(b, 190, h);
  lv_obj_set_pos(b, 16, y);
  lv_bar_set_range(b, 0, 100);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_BORDER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
  lv_obj_set_style_radius(b, h / 2, LV_PART_INDICATOR);
  return b;
}

// Label pair on one row: muted name on the left, dim value on the right.
void makeRow(lv_obj_t *card, int y, lv_obj_t *&left, lv_obj_t *&right) {
  left = makeLabel(card, &lv_font_montserrat_14, COL_MUTED, "");
  lv_obj_set_pos(left, 16, y);
  right = makeLabel(card, &lv_font_montserrat_12, COL_DIM, "");
  lv_obj_align(right, LV_ALIGN_TOP_RIGHT, -16, y + 2);
}

// Card (design B "pace bars"): header, big % left, main bar with a white mark at the share of time
// left in the window, secondary bar, and a pace verdict line.
void makeQuotaCard(lv_obj_t *page, int x, const char *title, uint32_t brand, QuotaCard &c) {
  lv_obj_t *card = makePanel(page, x, 4, 222, 212, 20);

  lv_obj_t *mark = makeDot(card, 10, brand);
  lv_obj_set_pos(mark, 16, 17);
  lv_obj_t *t = makeLabel(card, &lv_font_montserrat_14, COL_MUTED, title);
  lv_obj_set_style_text_letter_space(t, 2, 0);
  lv_obj_set_pos(t, 32, 13);
  c.badge = makeLabel(card, &lv_font_montserrat_12, COL_DIM, "");
  lv_obj_align(c.badge, LV_ALIGN_TOP_RIGHT, -16, 15);

  c.pct = makeLabel(card, &lv_font_montserrat_36, COL_TEXT, "--");
  lv_obj_set_pos(c.pct, 16, 36);
  c.unit = makeLabel(card, &lv_font_montserrat_14, COL_MUTED, "left");

  makeRow(card, 86, c.l1, c.r1);
  c.bar1 = makeBar(card, 108, 10);
  c.pace = lv_obj_create(card);  // pace mark: where the bar end should be if usage tracked time evenly
  lv_obj_set_size(c.pace, 2, 18);
  lv_obj_set_style_bg_color(c.pace, lv_color_hex(COL_TEXT), 0);
  lv_obj_set_style_border_width(c.pace, 0, 0);
  lv_obj_set_style_radius(c.pace, 1, 0);
  lv_obj_set_style_pad_all(c.pace, 0, 0);

  makeRow(card, 130, c.l2, c.r2);
  c.bar2 = makeBar(card, 152, 6);

  c.statusDot = makeDot(card, 8, COL_DIM);
  lv_obj_set_pos(c.statusDot, 16, 180);
  c.status = makeLabel(card, &lv_font_montserrat_12, COL_DIM, "");
  lv_obj_set_pos(c.status, 30, 176);
}

void buildQuotaPage(lv_obj_t *page) {
  makeQuotaCard(page, 12, "CLAUDE", COL_CORAL, claudeCard);
  makeQuotaCard(page, 246, "CURSOR", COL_TEXT, cursorCard);

  // Cursor QR login: covers both cards until the board has a Cursor session (first boot, or session ended).
  loginPanel = makePanel(page, 12, 4, 456, 212, 20);
  lv_obj_t *mark = makeDot(loginPanel, 10, COL_TEXT);
  lv_obj_set_pos(mark, 16, 17);
  lv_obj_t *t = makeLabel(loginPanel, &lv_font_montserrat_14, COL_MUTED, "CURSOR");
  lv_obj_set_style_text_letter_space(t, 2, 0);
  lv_obj_set_pos(t, 32, 13);
  lv_obj_set_pos(makeLabel(loginPanel, &lv_font_montserrat_20, COL_TEXT, "Scan to connect"), 16, 50);
  lv_obj_t *steps = makeLabel(loginPanel, &lv_font_montserrat_14, COL_MUTED,
                              "1. Scan the code with your phone\n2. Log in to Cursor\n3. The board connects itself");
  lv_obj_set_style_text_line_space(steps, 6, 0);
  lv_obj_set_pos(steps, 16, 88);
  loginStatus = makeLabel(loginPanel, &lv_font_montserrat_12, COL_DIM, "");
  lv_obj_set_pos(loginStatus, 16, 180);
  lv_obj_t *qrBox = lv_obj_create(loginPanel);  // white quiet zone around the code
  lv_obj_set_size(qrBox, 196, 196);
  lv_obj_align(qrBox, LV_ALIGN_RIGHT_MID, -8, 0);
  lv_obj_set_style_bg_color(qrBox, lv_color_white(), 0);
  lv_obj_set_style_border_width(qrBox, 0, 0);
  lv_obj_set_style_radius(qrBox, 12, 0);
  lv_obj_set_style_pad_all(qrBox, 0, 0);
  lv_obj_clear_flag(qrBox, LV_OBJ_FLAG_SCROLLABLE);
  loginQr = lv_qrcode_create(qrBox, 180, lv_color_black(), lv_color_white());
  lv_obj_center(loginQr);

  // Footer row: status pill + update text on the left, IP on the right.
  lv_obj_t *row = makeFooterRow(page, 224, 330);
  quotaPill = makePill(row);
  quotaFootL = makeLabel(row, &lv_font_montserrat_14, COL_MUTED, "");
  lv_label_set_long_mode(quotaFootL, LV_LABEL_LONG_DOT);
  lv_obj_set_width(quotaFootL, 230);
  quotaFootR = makeLabel(page, &lv_font_montserrat_12, COL_DIM, "");
  lv_obj_align(quotaFootR, LV_ALIGN_TOP_RIGHT, -14, 230);
}

void formatRemaining(time_t resets, char *buf, size_t n) {
  time_t now = time(nullptr);
  if (resets == 0 || now < 1700000000) {  // no reset time, or clock not synced yet
    snprintf(buf, n, "--");
    return;
  }
  long s = (long)(resets - now);
  if (s <= 0) snprintf(buf, n, "now");
  else if (s >= 86400) snprintf(buf, n, "%ldd %ldh", s / 86400, (s % 86400) / 3600);
  else snprintf(buf, n, "%ldh %02ldm", s / 3600, (s % 3600) / 60);
}

// Percent remaining (0..100) from percent used; -1 = unknown.
int leftPct(float used) {
  return used < 0 ? -1 : constrain((int)(100.5f - used), 0, 100);
}

// Share of the window still to go (0..100); -1 if unknown.
int timeLeftPct(time_t resets, long windowSecs) {
  time_t now = time(nullptr);
  if (resets == 0 || windowSecs <= 0 || now < 1700000000) return -1;
  return constrain((int)((long long)(resets - now) * 100 / windowSecs), 0, 100);
}

uint32_t secondaryColor(float used, bool stale) {
  return stale ? COL_STALE : (used >= 80 ? COL_BAD : COL_VIOLET);
}

// Fills one card from percent used (main + secondary) and the share of time left in the main window.
void setCard(QuotaCard &c, float used, float used2, int timeLeft, bool stale) {
  int left = leftPct(used);
  if (left < 0) lv_label_set_text(c.pct, "--");
  else lv_label_set_text_fmt(c.pct, "%d%%", left);
  lv_obj_set_style_text_color(c.pct, lv_color_hex(stale ? COL_DIM : COL_TEXT), 0);
  lv_obj_align_to(c.unit, c.pct, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -7);

  lv_bar_set_value(c.bar1, max(left, 0), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(c.bar1, lv_color_hex(stale ? COL_STALE : levelColor(used)), LV_PART_INDICATOR);
  lv_bar_set_value(c.bar2, max(leftPct(used2), 0), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(c.bar2, lv_color_hex(secondaryColor(used2, stale)), LV_PART_INDICATOR);

  const char *txt = "Pace unknown";
  uint32_t col = COL_DIM;
  if (left >= 0 && timeLeft >= 0) {
    lv_obj_clear_flag(c.pace, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(c.pace, 16 + 190 * timeLeft / 100 - 1, 104);
    int d = left - timeLeft;  // quota left minus time left: > 0 = using slower than time passes
    if (d > 5) txt = "Ahead of pace", col = COL_OK;
    else if (d >= -5) txt = "On pace", col = COL_OK;
    else if (d >= -25) txt = "Faster than pace", col = COL_WARN;
    else txt = "Will run out early", col = COL_BAD;
  } else {
    lv_obj_add_flag(c.pace, LV_OBJ_FLAG_HIDDEN);
  }
  if (stale) col = COL_STALE;
  lv_label_set_text(c.status, txt);
  lv_obj_set_style_text_color(c.status, lv_color_hex(col), 0);
  lv_obj_set_style_bg_color(c.statusDot, lv_color_hex(col), 0);
}

void refreshQuotaPage() {
  char buf[24];
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  uint32_t claudeAge = millis() - lastUpdateMs, cursorAge = millis() - cursorUpdateMs;
  bool claudeStale = haveData && claudeAge >= STALE_MS;
  bool cursorStale = cursorHave && cursorAge >= CURSOR_STALE_MS;

  // Claude: main = 5-hour window, secondary = weekly.
  setCard(claudeCard, q5.pct, q7.pct, timeLeftPct(q5.resets, 5L * 3600L), claudeStale);
  lv_label_set_text(claudeCard.l1, "5-hour");
  formatRemaining(q5.resets, buf, sizeof(buf));
  lv_label_set_text_fmt(claudeCard.r1, "resets %s", buf);
  lv_label_set_text(claudeCard.l2, "Weekly");
  formatRemaining(q7.resets, buf, sizeof(buf));
  if (q7.pct < 0) lv_label_set_text_fmt(claudeCard.r2, "-- - %s", buf);
  else lv_label_set_text_fmt(claudeCard.r2, "%d%% - %s", leftPct(q7.pct), buf);

  // Cursor: main = billing month, secondary = Auto; API as text.
  setCard(cursorCard, cur.total, cur.autoPct, timeLeftPct(cur.resets, (long)(cur.resets - cur.starts)), cursorStale);
  lv_label_set_text(cursorCard.badge, cur.plan);
  lv_label_set_text(cursorCard.l1, "Month");
  formatRemaining(cur.resets, buf, sizeof(buf));
  lv_label_set_text_fmt(cursorCard.r1, "resets %s", buf);
  lv_label_set_text(cursorCard.l2, "Auto");
  if (cur.autoPct < 0) lv_label_set_text(cursorCard.r2, "--");
  else if (cur.apiPct < 0) lv_label_set_text_fmt(cursorCard.r2, "%d%%", leftPct(cur.autoPct));
  else lv_label_set_text_fmt(cursorCard.r2, "%d%% - API %d%%", leftPct(cur.autoPct), leftPct(cur.apiPct));

  bool needLogin = cursorToken.isEmpty();
  if (needLogin) lv_obj_clear_flag(loginPanel, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(loginPanel, LV_OBJ_FLAG_HIDDEN);
  if (!loginActive) lv_label_set_text(loginStatus, "Waiting for Wi-Fi...");
  else {
    uint32_t left = (LOGIN_TTL_MS - min(millis() - loginStartMs, LOGIN_TTL_MS)) / 1000;
    lv_label_set_text_fmt(loginStatus, "New code in %lu:%02lu", (unsigned long)(left / 60), (unsigned long)(left % 60));
  }

  bool anyError = apiError[0] || (cursorError[0] && !needLogin);
  if (!wifiUp) setPill(quotaPill, COL_BAD, "OFFLINE");
  else if (needLogin) setPill(quotaPill, COL_WARN, "LOGIN");
  else if (anyError) setPill(quotaPill, COL_BAD, "ERROR");
  else if (!haveData || !cursorHave) setPill(quotaPill, COL_DIM, "SYNCING");
  else if (claudeStale || cursorStale) setPill(quotaPill, COL_WARN, "STALE");
  else setPill(quotaPill, COL_OK, "LIVE");

  // Footer: problem first (Wi-Fi, Claude, Cursor), else age of the older source.
  uint32_t footCol = COL_MUTED;
  if (!wifiUp) {
    lv_label_set_text_fmt(quotaFootL, "Wi-Fi: %s (%u)", scanInfo, wifiReason);
    footCol = COL_BAD;
  } else if (apiError[0]) {
    lv_label_set_text_fmt(quotaFootL, "Claude: %s", apiError);
    footCol = COL_BAD;
  } else if (needLogin) {
    lv_label_set_text(quotaFootL, "Cursor: scan the QR code to log in");
  } else if (cursorError[0]) {
    lv_label_set_text_fmt(quotaFootL, "Cursor: %s", cursorError);
    footCol = COL_BAD;
  } else if (!haveData || !cursorHave) {
    lv_label_set_text(quotaFootL, "Fetching usage...");
  } else {
    uint32_t mins = max(claudeAge, cursorAge) / 60000;
    if (mins == 0) lv_label_set_text(quotaFootL, "Updated just now");
    else lv_label_set_text_fmt(quotaFootL, "Updated %lu min ago", (unsigned long)mins);
  }
  lv_obj_set_style_text_color(quotaFootL, lv_color_hex(footCol), 0);
  lv_label_set_text(quotaFootR, wifiUp ? WiFi.localIP().toString().c_str() : "");
}

// ---------- Page: ESP32 system ----------

Tile tCpu, tRam, tPsram, tWifi, tTemp, tBatt;
Pill sysPill;
int cpuLoad[2] = {0, 0};  // per-core load %, sampled once a second

// 146x104 metric tile: title, big value, detail line, thin bar.
Tile makeTile(lv_obj_t *page, int x, int y, const char *title) {
  lv_obj_t *p = makePanel(page, x, y, 146, 104, 16);
  lv_obj_t *t = makeLabel(p, &lv_font_montserrat_12, COL_MUTED, title);
  lv_obj_set_style_text_letter_space(t, 1, 0);
  lv_obj_set_pos(t, 14, 12);
  Tile r;
  r.value = makeLabel(p, &lv_font_montserrat_20, COL_TEXT, "--");
  lv_obj_set_pos(r.value, 14, 30);
  r.sub = makeLabel(p, &lv_font_montserrat_12, COL_DIM, "");
  lv_label_set_long_mode(r.sub, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(r.sub, 120);
  lv_obj_set_pos(r.sub, 14, 56);
  r.bar = lv_bar_create(p);
  lv_obj_set_size(r.bar, 118, 6);
  lv_obj_set_pos(r.bar, 14, 82);
  lv_bar_set_range(r.bar, 0, 100);
  lv_obj_set_style_bg_color(r.bar, lv_color_hex(COL_BORDER), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(r.bar, LV_OPA_COVER, LV_PART_MAIN);
  return r;
}

void setTile(Tile &t, int barPct, uint32_t color) {
  lv_bar_set_value(t.bar, constrain(barPct, 0, 100), LV_ANIM_OFF);
  lv_obj_set_style_bg_color(t.bar, lv_color_hex(color), LV_PART_INDICATOR);
}

void buildSystemPage(lv_obj_t *page) {
  tCpu = makeTile(page, 12, 4, "CPU");
  tRam = makeTile(page, 167, 4, "RAM");
  tPsram = makeTile(page, 322, 4, "PSRAM");
  tWifi = makeTile(page, 12, 116, "WI-FI");
  tTemp = makeTile(page, 167, 116, "CHIP TEMP");
  tBatt = makeTile(page, 322, 116, "BATTERY");

  // Footer row: uptime pill + chip info.
  lv_obj_t *row = makeFooterRow(page, 224, 456);
  sysPill = makePill(row);
  lv_obj_t *info = makeLabel(row, &lv_font_montserrat_12, COL_DIM, "");
  lv_label_set_text_fmt(info, "ESP32-S3  |  %lu MHz  |  %lu MB flash  |  IDF %s", (unsigned long)getCpuFrequencyMhz(),
                        (unsigned long)(ESP.getFlashChipSize() >> 20), esp_get_idf_version());
}

// Per-core load from FreeRTOS idle-task run time (counter is esp_timer microseconds).
void sampleCpu() {
  static uint32_t lastIdle[2] = {0, 0}, lastUs = 0;
  uint32_t now = (uint32_t)esp_timer_get_time();
  uint32_t dt = now - lastUs;
  for (int c = 0; c < 2; c++) {
    uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(c);
    if (lastUs && dt) cpuLoad[c] = constrain(100 - (int)((uint64_t)(idle - lastIdle[c]) * 100 / dt), 0, 100);
    lastIdle[c] = idle;
  }
  lastUs = now;
}

void refreshSystemPage() {
  int cpu = (cpuLoad[0] + cpuLoad[1]) / 2;
  lv_label_set_text_fmt(tCpu.value, "%d%%", cpu);
  lv_label_set_text_fmt(tCpu.sub, "C0 %d%%  C1 %d%%", cpuLoad[0], cpuLoad[1]);
  setTile(tCpu, cpu, levelColor(cpu));

  size_t ramTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  size_t ramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  int ramPct = ramTotal ? (int)((ramTotal - ramFree) * 100 / ramTotal) : 0;
  lv_label_set_text_fmt(tRam.value, "%d%%", ramPct);
  lv_label_set_text_fmt(tRam.sub, "%u / %u KB", (unsigned)((ramTotal - ramFree) >> 10), (unsigned)(ramTotal >> 10));
  setTile(tRam, ramPct, levelColor(ramPct));

  uint32_t psTotal = ESP.getPsramSize(), psFree = ESP.getFreePsram();
  int psPct = psTotal ? (int)((uint64_t)(psTotal - psFree) * 100 / psTotal) : 0;
  lv_label_set_text_fmt(tPsram.value, "%d%%", psPct);
  lv_label_set_text_fmt(tPsram.sub, "%u KB / %u MB", (unsigned)((psTotal - psFree) >> 10), (unsigned)(psTotal >> 20));
  setTile(tPsram, psPct, levelColor(psPct));

  if (WiFi.status() == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    int q = constrain((rssi + 90) * 100 / 60, 0, 100);  // -90 dBm = 0%, -30 dBm = 100%
    lv_label_set_text_fmt(tWifi.value, "%d dBm", rssi);
    lv_label_set_text_fmt(tWifi.sub, "%s  ch%d", q >= 60 ? "Good" : (q >= 30 ? "Fair" : "Weak"), WiFi.channel());
    setTile(tWifi, q, q >= 60 ? COL_OK : (q >= 30 ? COL_WARN : COL_BAD));
  } else {
    lv_label_set_text(tWifi.value, "Offline");
    lv_label_set_text(tWifi.sub, "reconnecting");
    setTile(tWifi, 0, COL_BAD);
  }

  float temp = temperatureRead();
  lv_label_set_text_fmt(tTemp.value, "%d.%d C", (int)temp, abs((int)(temp * 10)) % 10);
  lv_label_set_text(tTemp.sub, "on-die sensor");
  setTile(tTemp, (int)temp, temp >= 70 ? COL_BAD : (temp >= 55 ? COL_WARN : COL_OK));

  if (!pmuOk) {
    lv_label_set_text(tBatt.value, "n/a");
    lv_label_set_text(tBatt.sub, "PMU not found");
    setTile(tBatt, 0, COL_DIM);
  } else if (power.isBatteryConnect()) {
    int b = power.getBatteryPercent();
    uint16_t mv = power.getBattVoltage();
    lv_label_set_text_fmt(tBatt.value, "%d%%", b);
    lv_label_set_text_fmt(tBatt.sub, "%u.%02u V%s", mv / 1000, (mv % 1000) / 10, power.isCharging() ? "  charging" : "");
    setTile(tBatt, b, b <= 15 ? COL_BAD : (b <= 35 ? COL_WARN : COL_OK));
  } else {
    lv_label_set_text(tBatt.value, "USB");
    lv_label_set_text(tBatt.sub, "no battery");
    setTile(tBatt, 100, COL_DIM);
  }

  uint32_t s = millis() / 1000;
  char up[24];
  if (s >= 86400) snprintf(up, sizeof(up), "UP %lud %luh", s / 86400, (s % 86400) / 3600);
  else snprintf(up, sizeof(up), "UP %luh %02lum", s / 3600, (s % 3600) / 60);
  setPill(sysPill, COL_OK, up);
}

// ---------- Tabs ----------
// Segmented tab bar across the top. It scrolls sideways when the tabs don't fit;
// fades at the edges show that more tabs are hidden on that side.

struct Page {
  const char *icon;  // LV_SYMBOL_*
  const char *name;
  void (*build)(lv_obj_t *page);
  void (*refresh)();
};
const Page PAGES[] = {
  { LV_SYMBOL_CHARGE, "Quota", buildQuotaPage, refreshQuotaPage },
  { LV_SYMBOL_SETTINGS, "System", buildSystemPage, refreshSystemPage },
};
const int PAGE_COUNT = sizeof(PAGES) / sizeof(PAGES[0]);

lv_obj_t *tabBar, *fadeLeft, *fadeRight;
lv_obj_t *tabBtn[PAGE_COUNT], *tabIcon[PAGE_COUNT], *pageObj[PAGE_COUNT];
int activeTab = 0;

void selectTab(int i) {
  for (int k = 0; k < PAGE_COUNT; k++) {
    bool on = k == i;
    if (on) lv_obj_add_state(tabBtn[k], LV_STATE_CHECKED);
    else lv_obj_clear_state(tabBtn[k], LV_STATE_CHECKED);
    lv_obj_set_style_text_color(tabIcon[k], lv_color_hex(on ? COL_CORAL : COL_DIM), 0);
    if (on) lv_obj_clear_flag(pageObj[k], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pageObj[k], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_scroll_to_view(tabBtn[i], LV_ANIM_ON);  // half-hidden tab slides fully into view
  activeTab = i;
  dirty = true;
}

void updateFades() {
  if (lv_obj_get_scroll_left(tabBar) > 1) lv_obj_clear_flag(fadeLeft, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(fadeLeft, LV_OBJ_FLAG_HIDDEN);
  if (lv_obj_get_scroll_right(tabBar) > 1) lv_obj_clear_flag(fadeRight, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(fadeRight, LV_OBJ_FLAG_HIDDEN);
}

// LVGL 8 has no alpha gradient, so the fade is 6 strips of rising opacity.
lv_obj_t *makeFade(lv_obj_t *scr, int x, bool toRight) {
  lv_obj_t *f = lv_obj_create(scr);
  lv_obj_set_size(f, 36, 38);
  lv_obj_set_pos(f, x, 9);
  lv_obj_set_style_bg_opa(f, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(f, 0, 0);
  lv_obj_set_style_pad_all(f, 0, 0);
  lv_obj_set_style_radius(f, 11, 0);
  lv_obj_set_style_clip_corner(f, true, 0);
  lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(f, LV_OBJ_FLAG_SCROLLABLE);
  for (int s = 0; s < 6; s++) {
    lv_obj_t *strip = lv_obj_create(f);
    lv_obj_set_size(strip, 6, 38);
    lv_obj_set_pos(strip, s * 6, 0);
    lv_obj_set_style_radius(strip, 0, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_bg_color(strip, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(strip, (toRight ? s + 1 : 6 - s) * 40, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);
  }
  return f;
}

void buildUi() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  tabBar = makePanel(scr, 12, 8, 456, 40, 12);
  lv_obj_set_style_pad_all(tabBar, 3, 0);
  lv_obj_set_style_pad_column(tabBar, 2, 0);
  lv_obj_set_flex_flow(tabBar, LV_FLEX_FLOW_ROW);
  lv_obj_add_flag(tabBar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(tabBar, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(tabBar, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(tabBar, [](lv_event_t *) { updateFades(); }, LV_EVENT_SCROLL, NULL);

  for (int i = 0; i < PAGE_COUNT; i++) {
    lv_obj_t *b = lv_obj_create(tabBar);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 32);
    lv_obj_set_style_radius(b, 9, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 16, 0);
    lv_obj_set_style_pad_ver(b, 0, 0);
    lv_obj_set_style_pad_column(b, 8, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_BORDER), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(b, lv_color_hex(COL_BORDER), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_text_color(b, lv_color_hex(COL_TEXT), LV_STATE_CHECKED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(b, [](lv_event_t *e) { selectTab((int)(intptr_t)lv_event_get_user_data(e)); },
                        LV_EVENT_CLICKED, (void *)(intptr_t)i);
    tabIcon[i] = lv_label_create(b);
    lv_label_set_text(tabIcon[i], PAGES[i].icon);
    lv_obj_t *name = lv_label_create(b);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_label_set_text(name, PAGES[i].name);
    tabBtn[i] = b;

    lv_obj_t *page = lv_obj_create(scr);
    lv_obj_set_size(page, 480, 264);
    lv_obj_set_pos(page, 0, 56);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    PAGES[i].build(page);
    pageObj[i] = page;
  }

  fadeLeft = makeFade(scr, 13, false);
  fadeRight = makeFade(scr, 12 + 456 - 1 - 36, true);
  selectTab(0);
  lv_obj_update_layout(scr);
  updateFades();
}

void refreshActivePage() {
  PAGES[activeTab].refresh();
}

// Look for the configured SSID so the screen can say "not found" vs "found but rejected".
void scanForSsid() {
  int n = WiFi.scanNetworks();
  snprintf(scanInfo, sizeof(scanInfo), "SSID not found");
  for (int i = 0; i < n; i++) {
    Serial.printf("  scan: %-32s ch%-2d %4ddBm enc%d\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), WiFi.encryptionType(i));
    if (WiFi.SSID(i) == WIFI_SSID) {
      snprintf(scanInfo, sizeof(scanInfo), "ch%d %ddBm", WiFi.channel(i), WiFi.RSSI(i));
    }
  }
  WiFi.scanDelete();
  Serial.printf("target SSID: %s\n", scanInfo);
}

void setup() {
  Serial.begin(115200);
  Serial.println("esp32-dashboard start");

  Wire.begin(I2C_SDA, I2C_SCL);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  lcd_reset();

  touchOk = touch.begin(Wire, FT6X36_SLAVE_ADDRESS);
  Serial.println(touchOk ? "FT6X36 OK" : "FT6X36 NOT found");
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
  gfx->fillScreen(BLACK);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  lv_init();
  static lv_color_t buf[LCD_VER_RES * 40];
  static lv_disp_draw_buf_t drawBuf;
  static lv_disp_drv_t dispDrv;
  static lv_indev_drv_t indevDrv;
  lv_disp_draw_buf_init(&drawBuf, buf, NULL, LCD_VER_RES * 40);
  lv_disp_drv_init(&dispDrv);
  dispDrv.hor_res = gfx->width();  // 480 in landscape
  dispDrv.ver_res = gfx->height();
  dispDrv.flush_cb = flushCb;
  dispDrv.draw_buf = &drawBuf;
  lv_disp_drv_register(&dispDrv);
  lv_indev_drv_init(&indevDrv);
  indevDrv.type = LV_INDEV_TYPE_POINTER;
  indevDrv.read_cb = touchRead;
  lv_indev_drv_register(&indevDrv);

  Preferences prefs;
  prefs.begin("cursor", true);
  cursorToken = prefs.getString("tok", "");
  prefs.end();
  Serial.printf("cursor: %s\n", cursorToken.length() ? "session loaded from NVS" : "no session, QR login");

  buildUi();
  refreshActivePage();
  lv_timer_handler();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) { wifiReason = info.wifi_sta_disconnected.reason; },
               ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  scanForSsid();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  configTime(0, 0, "pool.ntp.org", "time.google.com");  // UTC; also needed for TLS cert dates
}

void loop() {
  static uint32_t lastRefresh = 0, lastPoll = 0, nextPollIn = 0;
  static wl_status_t lastWifi = WL_IDLE_STATUS;

  wl_status_t ws = WiFi.status();
  if (ws != lastWifi) {
    lastWifi = ws;
    dirty = true;
    if (ws == WL_CONNECTED) Serial.printf("Wi-Fi OK, IP %s\n", WiFi.localIP().toString().c_str());
  }

  // Poll once Wi-Fi is up and NTP has set the clock (TLS checks certificate dates).
  if (ws == WL_CONNECTED && time(nullptr) > 1700000000 && millis() - lastPoll >= nextPollIn) {
    lastPoll = millis();
    if (pollUsage()) {
      lastUpdateMs = millis();
      haveData = true;
      nextPollIn = POLL_MS;
    } else {
      nextPollIn = RETRY_MS;
    }
    dirty = true;
  }

  // Cursor: same pattern, slower interval (no token cost, but be polite to the dashboard API).
  static uint32_t lastCursorPoll = 0, nextCursorIn = 3000;  // 3 s after boot so Claude goes first
  static uint32_t lastLoginPoll = 0;
  bool netReady = ws == WL_CONNECTED && time(nullptr) > 1700000000;
  if (netReady && cursorToken.isEmpty()) {
    if (!loginActive || millis() - loginStartMs >= LOGIN_TTL_MS) {
      startCursorLogin();
      dirty = true;
    } else if (millis() - lastLoginPoll >= LOGIN_POLL_MS) {
      lastLoginPoll = millis();
      if (pollCursorLogin()) {
        nextCursorIn = 0;  // read usage right away
        dirty = true;
      }
    }
  } else if (netReady && millis() - lastCursorPoll >= nextCursorIn) {
    lastCursorPoll = millis();
    if (pollCursor()) {
      cursorUpdateMs = millis();
      cursorHave = true;
      nextCursorIn = CURSOR_POLL_MS;
    } else {
      nextCursorIn = RETRY_MS;
    }
    dirty = true;
  }

  static uint32_t lastDiag = 0;
  if (ws != WL_CONNECTED && millis() - lastDiag >= 5000) {
    lastDiag = millis();
    Serial.printf("Wi-Fi status %d, last reason %u, %s\n", ws, wifiReason, scanInfo);
    dirty = true;
  }

  if (dirty || millis() - lastRefresh >= 1000) {
    dirty = false;
    lastRefresh = millis();
    sampleCpu();
    refreshActivePage();
  }
  lv_timer_handler();
  delay(5);
}
