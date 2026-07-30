# Weather Display

A remote TFT display (Freenove ESP32-S3 2.8", ILI9341 240x320) that pulls
NWS weather, NOAA tide, and marine advisory data directly over WiFi and
shows it on-screen — no source unit or paired sensor required.

ESP32 firmware. The version lives in one place — `version.h` — and should match the
git tag for each release.

> **Current version:** v0.14

## Hardware

- **Board:** Freenove ESP32-S3 2.8" display (ESP32-S3-DevKitC target)
- **Display:** ILI9341 240x320 SPI TFT
- **Backlight:** GPIO45 (confirmed working)
- **Graphics:** [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI), configured entirely
  via `build_flags` in `platformio.ini` (no `User_Setup.h` to edit)

> ⚠️ The TFT **SPI pins** (`TFT_MISO/MOSI/SCLK/CS/DC/RST`) in `platformio.ini` are
> reasonable defaults — **verify them against your board / the prior Freenove
> display project before first flash.** Only the backlight pin (GPIO45) is confirmed.

## What it shows

- **Weather** — NWS forecast (api.weather.gov, no API key) for configured location:
  condition icon + scrolling condition text (forecast), with CURRENT temp +
  humidity pulled from the nearest live weather station rather than the forecast's
  predicted value.
- **Tides** — Next high/low tide predictions (NOAA CO-OPS) for the nearest tide
  station to your location, plus a moon-phase icon + name.
- **Hazards** — NWS small-craft advisory (marine zone) and rip current/beach
  hazards statement (coastal zone), combined into one scrolling headline; icon/text
  turn amber if either is active.
- **Wind / pressure** — live wind speed + direction and barometric pressure,
  from the same weather station observation as the current temp/humidity.
- **Coastal Waters Forecast** — NOAA/NWS text bulletin (product CWF) for your
  region; shows the current period's synopsis (winds/seas), scrolling. Refreshes
  hourly.

**Location configuration:** On first boot or if WiFi fails to connect, the device
starts an AP named `WeatherSetup-XXXX`. Connect and open `http://192.168.4.1` to:
1. Enter your WiFi SSID and password
2. Enter latitude/longitude for the weather location + an optional place name
Both are saved to NVS. Change location anytime by wiping NVS or resetting WiFi credentials.

## WiFi setup

No hardcoded credentials. On first boot (or failed join), the device starts
an AP named `WeatherSetup-XXXX` — connect and open `http://192.168.4.1` to
pick a network and password. Credentials are saved to NVS.

## Build & flash

```bash
# First flash over USB
pio run -e esp32s3 -t upload

# Subsequent updates over the air (device must be on WiFi)
pio run -e esp32s3-ota -t upload
```

OTA is also available in a browser at `http://weatherdisplay.local/update`
(ElegantOTA).

## Versioning

`version.h` defines `FW_VERSION`. To cut a release: bump it, commit, then tag to match:

```bash
git tag v1.0
git push --tags
```

## License

_TODO: pick one._
