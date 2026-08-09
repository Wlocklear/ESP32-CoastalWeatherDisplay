# Changelog

All notable changes to this firmware are recorded here.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/);
versions use [Semantic Versioning](https://semver.org/) (MAJOR.MINOR.PATCH).

## [1.0] — 2026-08-08

Consolidates two divergent codebases (the private dev repo and this public
repo's `main`/`release`/`Battery` branches) into one program, renamed
**CoastalWeather**. `Battery` becomes the sole branch going forward.

### Added
- On-board battery monitoring: reads the Freenove FNK0104AB's built-in
  battery-sense circuit (GPIO9, 0.5x resistive divider) locally — no network
  call. Voltage is mapped to an estimated charge percentage via a single-cell
  Li-ion discharge curve (`batteryPercent()`), oversampled 32x per reading to
  cut ADC/WiFi noise. Shown as a compact meter (`drawBatteryPanel()`) just
  above the IP/version footer, color-coded green/amber/red by charge level.
  `/status` adds `battValid`/`battVoltage`/`battPercent`/`battFull`; web
  dashboard gets a matching Battery card. No "Charging" state: this board has
  no charge-status pin, and a voltage-trend guess proved unreliable under
  load (flip-flopped between "Charging"/"On battery") — only "Fully charged"
  is shown, since that's a real, stable threshold (≥4.15V).
- `include/user_config.example.h` drops `COASTAL_OFFICE`/`COASTAL_ZONE` (no
  longer needed) now that the Coastal Waters Forecast panel is gone.

### Removed
- NOAA Coastal Waters Forecast panel entirely (`fetchCoastalForecast()` and
  its stream-scanning helpers, the scrolling marquee, `/status` `coastal*`
  fields, the dashboard's "Coastal Waters Forecast" card). Freed the 52px it
  occupied (border 95-146), which now hosts the BATTERY panel at the bottom
  of the screen; TIDES, HAZARDS, and WIND/PRESSURE all shifted up 52px to
  fill the gap — internal proportions of each panel are unchanged.

### Changed
- Renamed "Weather Display" → "CoastalWeather" throughout: default device
  name, AP setup SSID prefix (`CoastalWeather-`), mDNS default hostname
  (`coastalweather`), NWS user-agent, README/CHANGELOG titles, and web
  dashboard title/heading.
- HAZARDS marquee text no longer hardcodes a place name ("North Topsail
  Beach") — now uses the configured `wxPlace`, so a relocated device doesn't
  show a stale location in the scrolling headline.
- Fixed a latent build bug (present since the original public v0.14 fork):
  the `String` globals (`wxLat`, etc.) were declared before `#include
  <Arduino.h>`, so the file never actually compiled as committed. Includes
  now come first.

## [0.16] — 2026-08-04

### Changed
- Tides section rework (user feedback on 0.15): "TIDES" back down to Font 2,
  High/Low rows shrunk from Font 4 to Font 2 (rows at y172/y190), and the
  tide station place name promoted to FreeSansBold9pt at the header's right.
- Moon moved out of the tides header into its own "MOON" strip below the
  tide rows (y210–240): bigger phase icon (r11) on the right, phase name +
  "% lit" (same illumination math as `/status`) on the left.

## [0.15] — 2026-08-04

### Changed
- TIDES section title bumped from Font 2 to FreeSansBold12pt (~29 px), so it
  now reads larger than the Font-4 High/Low rows beneath it.
- Right side of the TIDES header now shows the tide station place name
  (`tidePlace`, e.g. "New River Inlet") in Font 2 next to the moon-phase
  icon, replacing the moon phase name text (still shown on the web
  dashboard's Tides card).

## [0.14] — 2026-08-02

### Added
- Location and station config moved out of compile-time `#define`s into
  runtime NVS settings (`loadLocationConfig()`/`saveLocationConfig()`),
  editable live from the web dashboard's new "Change Location / Stations"
  card: latitude/longitude/place name (drives the NWS forecast + title bar),
  weather observation station code, tide station number, and tide station
  place name. Saving reboots the device to pick up the new forecast URL and
  station IDs, same pattern as the existing WiFi-change flow. Defaults
  remain Hampstead, NC. `/status` now exposes
  `lat`/`lon`/`wxStation`/`tideStation`/`tidePlace`. Marine/rip/coastal
  hazard zones are unaffected and stay fixed to the Hampstead area.
- "Find nearest tide stations" button on the Location card: queries NOAA's
  full CO-OPS station list directly from the browser (client-side, not
  through the device — the list is ~2MB, too large for the ESP32's RAM),
  ranks stations by great-circle distance to the entered lat/lon, and lists
  the 10 closest. Clicking a result fills in the tide station # and name.
  The Tides dashboard card and web title now show the tide station's own
  place name instead of reusing the weather location.
- Location card now has a "Find by City" search (queries Open-Meteo's free
  geocoding API from the browser) that fills in latitude/longitude/place
  from a picked result, and a "Find nearest weather stations" search
  (queries `api.weather.gov/points` then its `observationStations` link)
  that lists nearby ASOS stations to pick from. Both the weather-station and
  tide-station pickers are now real `<select>` dropdowns instead of a
  clickable list, populated the same client-side way as the existing tide
  station search.

### Changed
- Web dashboard restyled to match the physical display's look: uppercase
  dim section labels (mirroring the TFT's on-screen headers), a large hero
  temperature/humidity readout, tide rows color-coded cyan/amber for
  High/Low the same as the device, hazard status text colored green/amber
  by active state, and a slim IP/firmware footer bar matching the device's
  own footer line. Panel cards given more contrast against the page
  background (`--card` lightened, visible border, subtle drop shadow) so
  sections read as distinct boxes instead of blending into one long list.
  Same base color palette as before (it already matched the firmware's
  `initColors()` values exactly).
- Added real inline-SVG icons generated client-side, procedurally built to
  match the device's own drawing code and color constants instead of just
  describing things in text: a weather icon (sun/moon/cloud/rain/
  thunderstorm/snow/fog, picked from the condition text the same way
  `mapCondition()` does on-device) next to the hero temperature, a moon
  phase icon (crescent/gibbous arc computed from the illumination
  percentage, same cosine-based terminator math as `drawMoonPhase()`) by
  the Tides moon-phase row, a sailboat icon in the Hazards header that
  changes color with advisory status like `marineColor()`, and a wave-chevron
  icon in the Coastal header.

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
