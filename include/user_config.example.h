#pragma once

// Contact for the api.weather.gov User-Agent header. NWS asks every client to
// identify itself with a way to reach you (an email address is fine).
#define NWS_CONTACT  "you@example.com"

// OTA / firmware-update login (ElegantOTA + espota). Pick your own.
#define OTA_USER  "admin"
#define OTA_PASS  "changeme"

#define DEFAULT_WX_LAT     "40.7128"
#define DEFAULT_WX_LON     "-74.0060"
#define DEFAULT_WX_PLACE   "New York, NY"
#define DEFAULT_WX_STATION "KNYC"

// Tides (NOAA CO-OPS). Find your station number at
// https://tidesandcurrents.noaa.gov/ (example: 8518750 = The Battery, NY).
#define DEFAULT_TIDE_STATION  "8518750"
#define DEFAULT_TIDE_PLACE    "The Battery"

#define MARINE_ZONE      "ANZ338"    // New York Harbor
#define RIP_ZONE         "NYZ072"    // New York (Manhattan)

// Timezone (POSIX TZ string — see
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)
#define TZ_STRING  "EST5EDT,M3.2.0,M11.1.0"            // US Eastern
