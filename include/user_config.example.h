#pragma once
// ── Personal configuration TEMPLATE ─────────────────────────────────────────
// Copy this file to include/user_config.h and fill in your own values.
// user_config.h is gitignored so your email, credentials, and location never
// end up in the repo.
//
//   cp include/user_config.example.h include/user_config.h

// Contact for the api.weather.gov User-Agent header. NWS asks every client to
// identify itself with a way to reach you (an email address is fine).
#define NWS_CONTACT  "you@example.com"

// OTA / firmware-update login (ElegantOTA + espota). Pick your own.
#define OTA_USER  "admin"
#define OTA_PASS  "changeme"

// ── Default location (first flash only — changeable from the web dashboard,
//    saved to NVS) ──────────────────────────────────────────────────────────
// Example values below are New York City. Find yours:
//   * lat/lon + place: any map service
//   * WX_STATION: nearest ASOS/AWOS station with live observations — open
//     https://api.weather.gov/points/{lat},{lon} then the observationStations
//     URL it returns, and pick the first station (e.g. an airport code).
#define DEFAULT_WX_LAT     "40.7128"
#define DEFAULT_WX_LON     "-74.0060"
#define DEFAULT_WX_PLACE   "New York, NY"
#define DEFAULT_WX_STATION "KNYC"

// Tides (NOAA CO-OPS). Find your station number at
// https://tidesandcurrents.noaa.gov/ (example: 8518750 = The Battery, NY).
#define DEFAULT_TIDE_STATION  "8518750"
#define DEFAULT_TIDE_PLACE    "The Battery"

// ── Fixed hazard zones (not runtime-configurable — set for your area) ───────
// Find your zones via https://api.weather.gov/points/{lat},{lon}:
//   * forecastZone gives the land zone (RIP_ZONE, e.g. "NYZ072")
//   * marine zones (AMZ/ANZ/...) are listed at
//     https://www.weather.gov/gis/MarineZones — pick the coastal-waters zone
//     off your shore (MARINE_ZONE)
#define MARINE_ZONE      "ANZ338"    // New York Harbor
#define RIP_ZONE         "NYZ072"    // New York (Manhattan)

// Timezone (POSIX TZ string — see
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)
#define TZ_STRING  "EST5EDT,M3.2.0,M11.1.0"            // US Eastern
