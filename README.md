# Coastal Weather Display

A remote TFT display (Freenove ESP32-S3 2.8", ILI9341 240x320) that pulls
NWS weather, NOAA tide, and marine advisory data directly over WiFi and
shows it on-screen — no source unit or paired sensor required.

ESP32 firmware. The version lives in one place — `version.h` — and should match the
git tag for each release.

> **Current version:** v1.0

## Hardware

- **Board:** Freenove ESP32-S3 2.8" display (ESP32-S3-DevKitC target)
- **Display:** ILI9341 240x320 SPI TFT
- **Backlight:** GPIO45 (confirmed working)
- **Battery sense:** GPIO9, board's built-in 0.5x resistive divider (JST battery
  connector) — see `fetchBattery()` in `src/main.cpp`
- **Graphics:** [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI), configured entirely
  via `build_flags` in `platformio.ini` (no `User_Setup.h` to edit)

> ⚠️ The TFT **SPI pins** (`TFT_MISO/MOSI/SCLK/CS/DC/RST`) in `platformio.ini` are
> reasonable defaults — **verify them against your board / the prior Freenove
> display project before first flash.** Only the backlight pin (GPIO45) is confirmed.

## What it shows

- **Weather** — NWS forecast (api.weather.gov, no API key) for your configured
  location: condition icon + scrolling condition text (forecast), with CURRENT
  temp + humidity pulled from the nearest live station observation
  (`WX_STATION`) rather than the forecast's predicted value.
- **Tides** — Next high/low tide predictions (NOAA CO-OPS) for your configured
  tide station, plus a moon-phase icon + name.
- **Hazards** — NWS small-craft advisory (your marine zone) and rip
  current/beach hazards statement (your land zone), combined into one
  scrolling headline; icon/text turn amber if either is active.
- **Wind / pressure** — live wind speed + direction and barometric pressure,
  from the same station observation as the current temp/humidity.
- **Battery** — on-board voltage + estimated charge percentage, shown as a
  compact meter just above the footer. Reads locally (GPIO9), no network call
  needed.

## Configuration

Personal settings live in `include/user_config.h`, which is **gitignored** —
copy the template and edit it before building:

```bash
cp include/user_config.example.h include/user_config.h
```

It holds your NWS contact email (User-Agent), OTA login, default location
(lat/lon, place, weather + tide stations), fixed hazard zones
(`MARINE_ZONE`, `RIP_ZONE`), and timezone. The template has notes on finding
each value for your area. Location and stations can also be changed later
from the device's web dashboard (saved to NVS, with built-in city/station
finders); the hazard zones are compile-time only.

## WiFi setup

No hardcoded credentials. On first boot (or failed join), the device starts
an AP named `Coastal Weather Display-XXXX` — connect and open `http://192.168.4.1` to
pick a network and password. Credentials are saved to NVS.

## Build & flash

```bash
# First flash over USB
pio run -e esp32s3 -t upload

# Subsequent updates over the air (device must be on WiFi)
pio run -e esp32s3-ota -t upload
```

OTA is also available in a browser at `http://coastal-weather-display.local/update`
(ElegantOTA).

## Versioning

`version.h` defines `FW_VERSION`. To cut a release: bump it, commit, then tag to match:

```bash
git tag v1.0
git push --tags
```

## License

_TODO: pick one._
