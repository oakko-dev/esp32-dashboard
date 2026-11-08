# esp32-dashboard

A standalone desk dashboard for the **Waveshare ESP32-S3-Touch-LCD-3.5-C** (480x320 landscape, LVGL 8.4).
No PC needed once it is flashed.

- **Quota tab**: Claude plan usage (5-hour and weekly) and Cursor monthly usage (total, Auto, API),
  shown as percent remaining with a pace mark that tells you whether you will run out before the reset.
- **System tab**: ESP32 CPU load, RAM, PSRAM, Wi-Fi signal, chip temperature, battery.
- Scrolling tab bar on top; adding a tab is one line in `PAGES[]`.

## How the data is read

- **Claude**: every 2 minutes the board sends a 1-token request to the Claude Messages API with a
  `claude setup-token` token and reads the `anthropic-ratelimit-unified-{5h,7d}-*` response headers.
- **Cursor**: every 5 minutes the board reads `cursor.com/api/usage-summary`. On first boot it shows a
  QR code; scan it with your phone and log in. The session token is kept in NVS and renewed about
  every 10 days, so you only scan once. The login/refresh flow follows
  [OpenUsage](https://github.com/robinebers/openusage) (MIT).

Both are undocumented endpoints and may change without notice.

## Layout

| Path | What |
|---|---|
| `esp32-dashboard/` | Main sketch |
| `examples/` | Smaller sketches for the same board: `hello_touch` (display + touch test), `clock_sensors` (RTC, IMU, battery), `pomodoro` (timer with speaker), `totp` (authenticator, QR scan with the camera) |
| `config/lv_conf.h` | LVGL config used by the LVGL sketches |
| `tools/` | PC probes used while developing (Claude headers, Cursor usage, Cursor QR login). `tools/legacy/` is the retired Claude Code status-line bridge |

## Build

Toolchain (verified): esp32 Arduino core **3.2.0**, GFX Library for Arduino 1.5.5, lvgl **8.4.0**,
SensorLib 0.3.1, XPowersLib, TCA9554 (the libraries bundled with Waveshare's board repo).

1. Copy `config/lv_conf.h` to your Arduino `libraries/` folder (next to the `lvgl` folder).
   It enables Montserrat 12/14/20/36 and `LV_USE_QRCODE`.
2. Copy `esp32-dashboard/secrets.example.h` to `esp32-dashboard/secrets.h` and fill in Wi-Fi and the
   output of `claude setup-token`.
3. Build and upload (Arduino IDE or `arduino-cli`), board settings:

```
FQBN: esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc
```

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc" esp32-dashboard
arduino-cli upload -p COM3 --fqbn "esp32:esp32:esp32s3:FlashSize=16M,PSRAM=opi,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc" esp32-dashboard
```

The first LVGL build takes 10+ minutes.

## Security notes

- `secrets.h` is gitignored. Never commit it.
- The Claude token and the Cursor session token are stored unencrypted on the board's flash.
  If the board is lost, revoke the Claude token and sign out the Cursor session on cursor.com.
