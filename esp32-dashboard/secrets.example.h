// Copy this file to secrets.h (gitignored) and fill it in. Keep secrets.h private.
// 2.4 GHz Wi-Fi only.
#define WIFI_SSID "your-ssid"
#define WIFI_PASS "your-password"

// Output of `claude setup-token` (starts with sk-ant-oat01-).
// Stored in plain text on the board's flash; revoke it if the board is lost.
#define CLAUDE_TOKEN "sk-ant-oat01-..."

// Cursor needs nothing here: the board shows a QR code, you log in on your phone,
// and the session token is kept in NVS and renewed automatically.
