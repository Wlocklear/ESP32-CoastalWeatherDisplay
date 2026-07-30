# Changelog

All notable changes to this firmware are recorded here.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/);
versions use [Semantic Versioning](https://semver.org/) (MAJOR.MINOR.PATCH).

## [0.14] — 2026-07-29

Forked from `WeatherDisplay` (private) into its own public repo, with the
hardcoded home location pulled out so the firmware can be shared without
exposing a real address.

### Added
- Weather location (`wxLat`/`wxLon`/`wxPlace`) is now read from NVS at boot
  (`loadLocationConfig()`), defaulting to Hampstead, NC only on first flash.
- WiFi setup portal gained a `/save-location` step: enter latitude/longitude
  and an optional place name alongside WiFi credentials; saved via
  `saveLocationConfig()`.
- `fetchWeather()`/`fetchObservation()` build their request URLs from the
  runtime `wxLat`/`wxLon`/`wxStation` instead of compile-time macros.

### Known limitation
- Tide station, marine zone, rip zone, and coastal-forecast office/zone are
  still fixed at their Hampstead-area defaults (`DEFAULT_TIDE_STATION`,
  `DEFAULT_MARINE_ZONE`, `DEFAULT_RIP_ZONE`, `DEFAULT_COASTAL_ZONE/OFFICE`) —
  only the weather location is configurable so far. A user elsewhere gets
  correct local weather but Hampstead's tides/marine/coastal data.

## [0.13] — 2026-07-25

### Added
- NOAA Coastal Waters Forecast section: fetches the latest CWF text bulletin
  for office MHX and slices out zone AMZ158's current forecast period (e.g.
  "TONIGHT: NW winds 10 to 15 kt. Seas 2 to 4 ft..."), shown as a scrolling
  line. Refreshes hourly. Web dashboard gets a matching "Coastal Waters
  Forecast" card; `/status` adds `coastalValid`/`coastalPeriod`/`coastalText`.
- The CWF bulletin covers EVERY marine zone for the office (~25KB of escaped
  JSON text) — too large to safely hold in RAM alongside WiFi/TLS/webserver
  on this device (this is what made the section report "unavailable" on
  first flash). Rewrote the fetch to stream-scan the raw HTTPS response
  byte-by-byte, JSON-unescaping on the fly, and only ever keep the ~350
  bytes around our zone's header + first forecast period
  (`streamCoastalZone()`/`COASTAL_CAPTURE_MAX`), discarding the rest of the
  bulletin as it's read. Validated the extraction against two real NOAA
  bulletins before flashing.

### Added
- Rip current / beach hazards: fetches NWS active alerts for the land coastal
  zone (`NCZ105`, resolved from the same `/points` lookup already used for
  the forecast), preferring a "Rip Current Statement" or "Beach Hazards
  Statement" event. Combined into the existing MARINE panel (now labeled
  "HAZARDS (marine/rip)") — shows whichever of small-craft/rip-current are
  active, both if both are, on one scrolling line; icon/text turn amber if
  either is active. `/status` adds `ripValid`/`ripActive`/`ripEvent`/
  `ripHeadline`; web dashboard's Marine card becomes a combined Hazards card.
- Wind + barometric pressure: parsed from the same KILM station observation
  call already used for current temp/humidity (`fetchObservation()`), so no
  extra API request. Shown as a compact line ("NW 7 mph | 29.98 inHg") in
  the strip between the HAZARDS panel and the footer that was previously
  unused padding (283-307) — no other panel's layout changed. `/status`
  adds `wxWindMph`/`wxWindDir`/`wxPressureInHg`.

### Changed
- Reordered panels: COASTAL now sits directly under WEATHER (border 95-146),
  pushing TIDES down to 147-240 and MARINE to 241-282. All internal y-offsets
  were shifted uniformly (Coastal -137, Tides +52, Marine +51) so each
  panel's internal proportions are unchanged — just moved as a block.
- Hero temp/humidity now comes from the nearest live station observation
  (`api.weather.gov/stations/KILM/observations/latest`, `WX_STATION`) instead
  of the forecast's predicted periods[0] values — an actual current reading
  rather than a forecast estimate. The forecast call (`fetchWeather()`) still
  supplies the condition icon + scrolling condition text; a second call
  (`fetchObservation()`) supplies temp (converted C→F) + humidity. Both must
  succeed for the panel to show as valid. Web dashboard labels updated from
  "Forecast temp/humidity" to "Current temp/humidity" to match.
- Shrunk the MARINE panel (border 190) to make room for the new COASTAL panel
  right below it (border 232): both now use a Font-2 marquee (was Font-4) in
  a 20px-tall sprite (was 26px). Overall bottom-of-screen footprint (190-284)
  is unchanged — it's now split into two sections instead of one.

## [0.12] — 2026-07-25

### Removed
- All planter-monitor code: local XIAO climate reading, soil moisture, pump
  status/animation, `setLocalClimate()`/`setLocalSoil()`, and the "Planter
  (local sensor)" web dashboard card. This device is now strictly a Weather
  Display (weather/tides/marine only) — no local sensor input.

### Changed
- Hero temp/humidity now shows the NWS forecast values (`wx.tempF`/
  `wx.humidity`) instead of a local sensor reading.
- Rebranded from "Planter Info Display" to "Weather Display": default
  device name, AP setup SSID prefix (`WeatherSetup-`), mDNS default
  hostname (`weatherdisplay`), NWS user-agent, and web dashboard titles.
- `FW_VERSION` 0.11 → 0.12.

## [0.11] — 2026-07-23

### Added
- NWS weather (api.weather.gov): procedurally-drawn condition icon (sun/moon/
  cloud/rain/storm/snow/fog), forecast temp + humidity, scrolling condition text.
- NOAA CO-OPS tide predictions (station 8657813, Hampstead NC): next high/low,
  alongside a moon-phase icon + name.
- NWS marine alerts for small-craft advisories (zone AMZ158) with a scrolling
  headline marquee.
- Local planter panel: on-device XIAO climate (temp/humidity) and soil/pump
  readings via `setLocalClimate()`/`setLocalSoil()`, with an animated spinning
  pump icon.
- WiFi setup portal (AP mode, network scan, credentials saved to NVS) — no
  hardcoded WiFi.
- Web dashboard (`/`) mirroring on-device readings, plus device rename, WiFi
  network change/reset, and network scan endpoints.
- `/status` JSON API backing the dashboard.
- ElegantOTA (`/update`) and ArduinoOTA (env `esp32s3-ota`).

### Changed
- `FW_VERSION` 0.1 → 0.11.

## [0.1] — 2026-07-19

### Added
- Initial scaffold of Planter Info Display.
- Freenove ESP32-S3 2.8" ILI9341 (240x320) display via TFT_eSPI (configured in
  `platformio.ini` build_flags; backlight GPIO45).
- WiFi + mDNS (`planterinfodisplay.local`), ElegantOTA and `espota` OTA.
- Placeholder `/status` poll + JSON parse + on-screen render of temp/humidity/soil.

<!--
## [1.1.0] — YYYY-MM-DD

### Added
- ...

### Changed
- ...

### Fixed
- ...
-->
