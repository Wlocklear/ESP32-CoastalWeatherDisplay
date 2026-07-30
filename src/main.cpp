/*
 ******************************************************************************
 *  Weather Display
 ******************************************************************************
 *
 *  TARGET BOARD:
 *    Freenove ESP32-S3 Display FNK0104AB — ILI9341 2.8" IPS 240x320.
 *    Display SPI pins + backlight (GPIO45) are set via build_flags in
 *    platformio.ini. USE_HSPI_PORT=1 is required (see platformio.ini).
 *
 *  WHAT IT SHOWS (portrait):
 *    TOP    — Weather icon for Hampstead, NC (US National Weather Service,
 *             api.weather.gov, no API key) NEXT TO a large CURRENT
 *             temperature + humidity (live station observation, WX_STATION).
 *             The forecast condition scrolls beneath, followed directly by
 *             a NOAA Coastal Waters Forecast (CWF text bulletin, office MHX)
 *             scrolling line for the current period.
 *    MIDDLE — Next high/low TIDES for Hampstead, NC (NOAA CO-OPS, station
 *             8657813). Needs NTP time to know which events are upcoming.
 *    BOTTOM — NWS hazard alerts: small-craft advisory (marine zone) + rip
 *             current/beach hazards statement (land zone), one combined
 *             scrolling headline. Below that, live wind + barometric
 *             pressure (same station observation as the hero readings).
 *
 *  WiFi SETUP (no hardcoded credentials):
 *    First boot / failed join -> AP "WeatherSetup-XXXX", open http://192.168.4.1,
 *    pick network + password. Saved to NVS; creds from previous firmware reused.
 *
 *  OTA:
 *    ElegantOTA  -> http://<device>.local/update  (user: admin / pass: pw)
 *    ArduinoOTA  -> PlatformIO env:esp32s3-ota     (port 3232)
 ******************************************************************************/

#include "version.h"

// ── AP portal / OTA credentials ─────────────────────────────────────────────
#define AP_SSID_PREFIX   "WeatherSetup-"
#define OTA_USER         "admin"
#define OTA_PASS         "pw"
#define DEFAULT_DEVICE_NAME  "Weather Display"

// ── Weather — loaded from NVS on startup ───────────────────────────────────
// Default to Hampstead, NC if no config is saved
#define DEFAULT_WX_LAT   "34.3671"
#define DEFAULT_WX_LON   "-77.7108"
#define DEFAULT_WX_PLACE "Hampstead, NC"
#define DEFAULT_WX_STATION "KILM"
#define DEFAULT_TIDE_STATION "8657813"
#define DEFAULT_MARINE_ZONE "AMZ158"
#define DEFAULT_RIP_ZONE "NCZ105"
#define DEFAULT_COASTAL_ZONE "AMZ158"
#define DEFAULT_COASTAL_OFFICE "MHX"

String wxLat, wxLon, wxPlace, wxStation, tideStation, marineZone, ripZone, coastalZone, coastalOffice;

#define NWS_USER_AGENT  "WeatherDisplay/" FW_VERSION " (wlock75@gmail.com)"
#define WEATHER_INTERVAL_MS   (10UL * 60UL * 1000UL)   // refresh every 10 min
#define WEATHER_RETRY_MS      (60UL * 1000UL)
#define TIDE_INTERVAL_MS      (30UL * 60UL * 1000UL)   // refresh every 30 min
#define TIDE_RETRY_MS         (20UL * 1000UL)
#define MARINE_INTERVAL_MS    (15UL * 60UL * 1000UL)   // refresh every 15 min
#define MARINE_RETRY_MS       (60UL * 1000UL)
#define COASTAL_INTERVAL_MS   (60UL * 60UL * 1000UL)   // bulletin issued a few times/day
#define COASTAL_RETRY_MS      (5UL * 60UL * 1000UL)

// ── Time (NTP) ──────────────────────────────────────────────────────────────
#define TZ_STRING  "EST5EDT,M3.2.0,M11.1.0"            // US Eastern

// ── Timing ──────────────────────────────────────────────────────────────────
#define WIFI_RETRY_INTERVAL   30000UL
#define FOOTER_REFRESH_MS     15000UL
#define SCROLL_STEP_MS        70UL
#define SCROLL_STEP_PX        2

// ── Includes ────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <ArduinoOTA.h>
#include <esp_wifi.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <time.h>

// ── Objects ─────────────────────────────────────────────────────────────────
TFT_eSPI    tft;
TFT_eSprite condSpr = TFT_eSprite(&tft);      // scrolling condition marquee
WebServer   server(80);
Preferences prefs;

// ── Theme colors (via color565 for the BGR panel) ───────────────────────────
uint16_t COL_BG, COL_CARD, COL_BORDER, COL_ACCENT, COL_GREEN, COL_RED,
         COL_WARN, COL_DIM, COL_TEXT, COL_CYAN;
uint16_t WXC_SUN, WXC_MOON, WXC_CRATER, WXC_CLOUD, WXC_CLOUD_DK,
         WXC_RAIN, WXC_BOLT, WXC_SNOW;

// ── Weather state ───────────────────────────────────────────────────────────
enum WxIcon { WX_UNKNOWN = 0, WX_CLEAR, WX_PARTLY, WX_CLOUDY,
              WX_RAIN, WX_TSTORM, WX_SNOW, WX_FOG };
struct Weather {
  bool   valid    = false;
  float  tempF    = NAN;
  float  humidity = NAN;
  float  windMph  = NAN;
  float  windDeg  = NAN;
  float  pressureInHg = NAN;
  bool   isDay    = true;
  WxIcon icon     = WX_UNKNOWN;
  String condition = "";
};
Weather wx;
String  forecastUrl = "";

// ── Tides state ─────────────────────────────────────────────────────────────
struct TideEv { String clock; float ht = NAN; bool high = false; };
struct Tides  { bool valid = false; int n = 0; TideEv ev[2]; };
Tides tides;

// ── Marine (small craft advisory) state ─────────────────────────────────────
struct Marine { bool valid = false; bool active = false; String event; String headline; };
Marine marine;

// ── Rip current / beach hazards state (same shape as Marine) ───────────────
struct RipCurrent { bool valid = false; bool active = false; String event; String headline; };
RipCurrent rip;

// ── Marine headline marquee (second scrolling sprite) ───────────────────────
TFT_eSprite marSpr = TFT_eSprite(&tft);
#define MAR_Y 261
#define MAR_H 20
#define MAR_GAP 48
bool   marReady = false, marScroll = false;
int    marOff = 0, marTextW = 0;
String marText = "";
unsigned long lastMarScrollMs = 0;

// ── Coastal Waters Forecast state ───────────────────────────────────────────
struct Coastal { bool valid = false; String period; String text; };
Coastal coastal;

// ── Coastal forecast marquee (third scrolling sprite) ───────────────────────
TFT_eSprite coastSpr = TFT_eSprite(&tft);
#define COAST_Y 115
#define COAST_H 20
#define COAST_GAP 48
bool   coastReady = false, coastScroll = false;
int    coastOff = 0, coastTextW = 0;
String coastText = "";
unsigned long lastCoastScrollMs = 0;
unsigned long nextCoastAt = 0;

// ── Condition marquee state ─────────────────────────────────────────────────
#define COND_Y 76
#define COND_H 18
#define COND_GAP 40
bool   condReady = false, condScroll = false;
int    condOff = 0, condTextW = 0;
String condText = "";
unsigned long lastScrollMs = 0;

// ── Runtime state ───────────────────────────────────────────────────────────
String deviceName;
bool          mdnsRunning     = false;
bool          inAPMode        = false;
bool          otaRunning      = false;
bool          needsFullRedraw = true;
unsigned long bootMillis      = 0;
unsigned long lastWiFiRetry   = 0;
unsigned long lastFooterMs    = 0;
unsigned long lastWeatherOkMs = 0;
unsigned long nextWeatherAt   = 0;
unsigned long lastTideOkMs    = 0;
unsigned long nextTideAt      = 0;
unsigned long nextMarineAt    = 0;

// ── Forward declarations ────────────────────────────────────────────────────
bool   connectWiFi();
void   startAPPortal();
void   startMDNS();
void   setupRoutes();
void   setupArduinoOTA();
void   initColors();
void   drawBootScreen();
void   drawAPScreen();
void   drawMainScreen();
void   drawWeatherPanel();
void   drawHeroTemp();
void   drawTidesPanel();
void   drawMarinePanel();
void   drawCoastalPanel();
void   drawFooter();
bool   fetchWeather();
bool   fetchTides();
bool   fetchMarine();
bool   fetchRipCurrent();
bool   fetchCoastalForecast();
void   drawWindPressure();
String sanitizeMDNS(const String& s);
String wifiQuality(int rssi);
String apSsid();

// ============================================================
//  NVS HELPERS
// ============================================================
void loadPrefs() {
  prefs.begin("system", true);
  deviceName = prefs.getString("name", DEFAULT_DEVICE_NAME);
  prefs.end();
}
void saveSettings() {
  prefs.begin("system", false);
  prefs.putString("name", deviceName);
  prefs.end();
}
void saveWiFiCreds(const String& ssid, const String& pass) {
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}
void loadWiFiCreds(String& ssid, String& pass) {
  prefs.begin("wifi", true);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
}
void clearWiFiCreds() {
  prefs.begin("wifi", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.end();
}

void saveLocationConfig(const String& lat, const String& lon, const String& place) {
  prefs.begin("location", false);
  prefs.putString("lat", lat);
  prefs.putString("lon", lon);
  prefs.putString("place", place);
  prefs.end();
}

void loadLocationConfig(String& lat, String& lon, String& place) {
  prefs.begin("location", true);
  lat = prefs.getString("lat", DEFAULT_WX_LAT);
  lon = prefs.getString("lon", DEFAULT_WX_LON);
  place = prefs.getString("place", DEFAULT_WX_PLACE);
  prefs.end();
}

// ============================================================
//  mDNS / MISC HELPERS
// ============================================================
String sanitizeMDNS(const String& input) {
  String out = "";
  for (int i = 0; i < (int)input.length(); i++) {
    char c = tolower((unsigned char)input[i]);
    if (isalnum((unsigned char)c))  out += c;
    else if (c == '-' || c == ' ')  out += '-';
  }
  while (out.startsWith("-")) out = out.substring(1);
  while (out.endsWith("-"))   out = out.substring(0, out.length() - 1);
  if (out.isEmpty())          out = "weatherdisplay";
  return out;
}
void startMDNS() {
  if (mdnsRunning) MDNS.end();
  String host = sanitizeMDNS(deviceName);
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
    mdnsRunning = true;
    Serial.printf("[mDNS] http://%s.local\n", host.c_str());
  } else {
    mdnsRunning = false;
    Serial.println("[mDNS] Failed to start");
  }
}
String apSsid() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
  return String(AP_SSID_PREFIX) + buf;
}
String wifiQuality(int rssi) {
  if (!WiFi.isConnected()) return "Disconnected";
  if (rssi > -50) return "Excellent";
  if (rssi > -60) return "Good";
  if (rssi > -70) return "Fair";
  return "Weak";
}
// "2026-07-19 13:16" -> "1:16p"
static String formatClock(const String& t) {
  if (t.length() < 16) return t;
  int hh = t.substring(11, 13).toInt();
  String mm = t.substring(14, 16);
  const char* ap = (hh < 12) ? "a" : "p";
  int h12 = hh % 12; if (h12 == 0) h12 = 12;
  return String(h12) + ":" + mm + ap;
}

// ============================================================
//  THEME COLORS
// ============================================================
void initColors() {
  COL_BG     = tft.color565(0x0D, 0x0F, 0x1A);
  COL_CARD   = tft.color565(0x15, 0x17, 0x24);
  COL_BORDER = tft.color565(0x25, 0x28, 0x40);
  COL_ACCENT = tft.color565(0x4F, 0x8E, 0xF7);
  COL_GREEN  = tft.color565(0x3E, 0xCF, 0x8E);
  COL_RED    = tft.color565(0xF7, 0x6E, 0x6E);
  COL_WARN   = tft.color565(0xF7, 0xB8, 0x4F);
  COL_DIM    = tft.color565(0x88, 0x92, 0xA4);
  COL_TEXT   = tft.color565(0xDD, 0xE3, 0xF0);
  COL_CYAN   = tft.color565(0x4F, 0xCF, 0xEF);

  WXC_SUN      = tft.color565(0xFF, 0xD1, 0x40);
  WXC_MOON     = tft.color565(0xEA, 0xEE, 0xF7);
  WXC_CRATER   = tft.color565(0xBC, 0xC4, 0xD4);
  WXC_CLOUD    = tft.color565(0xC6, 0xCE, 0xDE);
  WXC_CLOUD_DK = tft.color565(0x6B, 0x74, 0x88);
  WXC_RAIN     = tft.color565(0x5A, 0x9E, 0xF7);
  WXC_BOLT     = tft.color565(0xFF, 0xD1, 0x40);
  WXC_SNOW     = tft.color565(0xFF, 0xFF, 0xFF);
}

// ============================================================
//  WEATHER ICON DRAWING (procedural)
// ============================================================
static void drawSun(int cx, int cy, int r) {
  for (int a = 0; a < 360; a += 45) {
    float rad = a * DEG_TO_RAD;
    int x0 = cx + cosf(rad) * (r + 4), y0 = cy + sinf(rad) * (r + 4);
    int x1 = cx + cosf(rad) * (r + 12), y1 = cy + sinf(rad) * (r + 12);
    tft.drawLine(x0, y0, x1, y1, WXC_SUN);
    tft.drawLine(x0, y0 + 1, x1, y1 + 1, WXC_SUN);
  }
  tft.fillCircle(cx, cy, r, WXC_SUN);
}
static void drawMoon(int cx, int cy, int r) {
  tft.fillCircle(cx, cy, r, WXC_MOON);
  tft.fillCircle(cx - r / 3, cy - r / 4, r / 5, WXC_CRATER);
  tft.fillCircle(cx + r / 4, cy + r / 5, r / 4, WXC_CRATER);
  tft.fillCircle(cx + r / 5, cy - r / 3, r / 7, WXC_CRATER);
}
static void drawCloud(int cx, int cy, int w, uint16_t color) {
  int r = w / 4;
  tft.fillCircle(cx - r, cy, r, color);
  tft.fillCircle(cx + r, cy, r, color);
  tft.fillCircle(cx, cy - (r * 3) / 4, (r * 6) / 5, color);
  tft.fillRect(cx - r, cy, 2 * r, r + 1, color);
}
static void drawRainDrops(int cx, int cy) {
  for (int i = -1; i <= 1; i++) {
    int x = cx + i * 14;
    tft.drawLine(x + 2, cy, x - 2, cy + 12, WXC_RAIN);
    tft.drawLine(x + 3, cy, x - 1, cy + 12, WXC_RAIN);
  }
}
static void drawBolt(int cx, int cy) {
  tft.fillTriangle(cx + 4, cy - 12, cx - 8, cy + 4, cx + 1, cy + 4, WXC_BOLT);
  tft.fillTriangle(cx - 3, cy + 2, cx + 8, cy - 2, cx - 4, cy + 16, WXC_BOLT);
}
static void drawSnowFlakes(int cx, int cy) {
  for (int i = -1; i <= 1; i++)
    tft.fillCircle(cx + i * 14, cy + (i == 0 ? 6 : 0), 2, WXC_SNOW);
}
static void drawFogLines(int cx, int cy) {
  for (int i = 0; i < 3; i++)
    tft.drawFastHLine(cx - 22 + (i & 1) * 6, cy + i * 7, 40, WXC_CLOUD);
}
// r = sun/moon radius; cloud width scales from it.
static void drawWxIcon(int cx, int cy, WxIcon icon, bool isDay, int r) {
  int cw = r * 2;                       // base cloud width
  switch (icon) {
    case WX_CLEAR:
      isDay ? drawSun(cx, cy, r) : drawMoon(cx, cy, r); break;
    case WX_PARTLY:
      isDay ? drawSun(cx - r * 0.6f, cy - r * 0.5f, r * 0.62f)
            : drawMoon(cx - r * 0.6f, cy - r * 0.5f, r * 0.62f);
      drawCloud(cx + r * 0.25f, cy + r * 0.35f, cw * 0.9f, WXC_CLOUD); break;
    case WX_CLOUDY:
      drawCloud(cx, cy, cw * 1.1f, WXC_CLOUD); break;
    case WX_RAIN:
      drawCloud(cx, cy - r * 0.35f, cw, WXC_CLOUD_DK); drawRainDrops(cx, cy + r * 0.8f); break;
    case WX_TSTORM:
      drawCloud(cx, cy - r * 0.35f, cw, WXC_CLOUD_DK); drawBolt(cx, cy + r * 0.6f); break;
    case WX_SNOW:
      drawCloud(cx, cy - r * 0.35f, cw, WXC_CLOUD); drawSnowFlakes(cx, cy + r * 0.8f); break;
    case WX_FOG:
      drawCloud(cx, cy - r * 0.4f, cw * 0.9f, WXC_CLOUD); drawFogLines(cx, cy + r * 0.45f); break;
    default:
      drawCloud(cx, cy, cw, WXC_CLOUD); break;
  }
}
static WxIcon mapCondition(const String& s) {
  String t = s; t.toLowerCase();
  if (t.indexOf("thunder") >= 0)                                   return WX_TSTORM;
  if (t.indexOf("snow") >= 0 || t.indexOf("sleet") >= 0 ||
      t.indexOf("flurr") >= 0 || t.indexOf("ice") >= 0 ||
      t.indexOf("blizzard") >= 0)                                  return WX_SNOW;
  if (t.indexOf("rain") >= 0 || t.indexOf("shower") >= 0 ||
      t.indexOf("drizzle") >= 0)                                   return WX_RAIN;
  if (t.indexOf("fog") >= 0 || t.indexOf("haze") >= 0 ||
      t.indexOf("smoke") >= 0 || t.indexOf("mist") >= 0)           return WX_FOG;
  if (t.indexOf("cloud") >= 0)
    return (t.indexOf("partly") >= 0) ? WX_PARTLY : WX_CLOUDY;
  if (t.indexOf("partly") >= 0 || t.indexOf("mostly sunny") >= 0 ||
      t.indexOf("mostly clear") >= 0)                              return WX_PARTLY;
  if (t.indexOf("sunny") >= 0 || t.indexOf("clear") >= 0 ||
      t.indexOf("fair") >= 0)                                      return WX_CLEAR;
  return WX_UNKNOWN;
}

// ============================================================
//  DISPLAY — BOOT / AP SCREENS
// ============================================================
void drawBootScreen() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.setTextFont(4);
  tft.drawString("Weather Display", 120, 120);
  tft.setTextFont(2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("v" FW_VERSION, 120, 158);
  tft.drawString("Connecting...", 120, 182);
}
void drawAPScreen() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_WARN, COL_BG);
  tft.setTextFont(4);
  tft.drawString("WiFi Setup", 120, 12);
  tft.setTextFont(2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("Connect your phone to:", 120, 70);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextFont(4);
  tft.drawString(apSsid(), 120, 96);
  tft.setTextFont(2);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("Then open:", 120, 150);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.drawString("http://192.168.4.1", 120, 172);
  tft.drawFastHLine(10, 210, 220, COL_BORDER);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("Pick a network + password,", 120, 224);
  tft.drawString("then Save to connect.", 120, 244);
}

// ── shared title bar helper ──────────────────────────────────────────────────
static void drawTitleBar(const String& title) {
  const int W = tft.width();
  tft.fillRect(0, 0, W, 24, COL_BG);
  tft.setTextFont(4);
  tft.setTextColor(COL_ACCENT, COL_BG);
  tft.setTextDatum(TC_DATUM);                        // centered
  tft.drawString(title.c_str(), W / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

// ============================================================
//  DISPLAY — CONDITION MARQUEE (scrolling forecast text)
// ============================================================
static void renderCondFrame() {
  if (!condReady) return;
  condSpr.fillSprite(COL_BG);
  condSpr.setTextDatum(ML_DATUM);
  condSpr.setTextColor(WXC_SUN, COL_BG);
  int y = COND_H / 2;
  condSpr.drawString(condText, condOff, y, 2);
  if (condScroll)
    condSpr.drawString(condText, condOff + condTextW + COND_GAP, y, 2);
  condSpr.pushSprite(0, COND_Y);
}
static void setupCondition() {
  const int W = tft.width();
  String raw = !wx.valid ? "Fetching weather..."
                         : (wx.condition.isEmpty() ? "--" : wx.condition);
  // No scrolling: drop any " then ..." continuation so the primary phrase fits.
  int t = raw.indexOf(" then ");
  if (t > 0) raw = raw.substring(0, t);
  condText = raw;
  // Ellipsis-truncate if it still doesn't fit.
  if (condReady && condSpr.textWidth(condText, 2) > W - 8) {
    while (condText.length() > 1 && condSpr.textWidth(condText + "...", 2) > W - 8)
      condText.remove(condText.length() - 1);
    condText += "...";
  }
  condScroll = false;                                // weather condition never scrolls
  condTextW = condReady ? condSpr.textWidth(condText, 2) : tft.textWidth(condText, 2);
  condOff = (W - condTextW) / 2;                     // centered
  renderCondFrame();
}

// ============================================================
//  DISPLAY — HERO (live temp + humidity beside the weather icon)
// ============================================================
void drawHeroTemp() {
  const int W = tft.width();
  tft.fillRect(44, 26, W - 44, 50, COL_BG);          // clear numbers, keep icon (x<44)
  const int y = 50;

  // Temperature (Font 4).
  const int hx = 52;
  String tv = (wx.valid && !isnan(wx.tempF))
                ? String((int)lroundf(wx.tempF)) : "--";
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(tv, hx, y, 4);
  int tw = tft.textWidth(tv, 4);
  tft.drawCircle(hx + tw + 6, y - 10, 3, COL_TEXT);  // degree ring
  tft.drawCircle(hx + tw + 6, y - 10, 2, COL_TEXT);
  tft.drawString("F", hx + tw + 11, y, 4);
  int tempEnd = hx + tw + 11 + tft.textWidth("F", 4);

  // Humidity (same size, BESIDE the temperature).
  String hv = (wx.valid && !isnan(wx.humidity))
                ? String((int)lroundf(wx.humidity)) : "--";
  int hx2 = tempEnd + 18;
  tft.setTextColor(COL_CYAN, COL_BG);
  tft.drawString(hv, hx2, y, 4);
  int hw = tft.textWidth(hv, 4);
  tft.drawString("%", hx2 + hw + 5, y, 4);
}

// ============================================================
//  DISPLAY — WEATHER PANEL  (title 0..23, body 24..75, cond 76..94)
// ============================================================
void drawWeatherPanel() {
  const int W = tft.width();
  drawTitleBar(wxPlace);
  tft.fillRect(0, 24, W, 52, COL_BG);                // icon area (numbers cleared in hero)
  if (wx.valid) drawWxIcon(24, 50, wx.icon, wx.isDay, 14);
  drawHeroTemp();
  setupCondition();
}

// ============================================================
//  MOON PHASE  (needs NTP time)
//  phase 0..1: 0/1 = new, 0.25 = first quarter, 0.5 = full, 0.75 = last quarter
// ============================================================
static float moonPhase() {
  time_t now = time(nullptr);
  if (now < 1600000000) return -1.0f;             // time not synced yet
  const double SYNODIC = 2551442.8;               // synodic month, seconds
  const double REF     = 947182440.0;             // 2000-01-06 18:14 UTC new moon
  double p = fmod(((double)now - REF), SYNODIC) / SYNODIC;
  if (p < 0) p += 1.0;
  return (float)p;
}
static String moonName(float p) {
  if (p < 0)                   return "";
  if (p < 0.02 || p > 0.98)    return "New Moon";
  if (p < 0.23)                return "Waxing Crescent";
  if (p < 0.27)                return "First Quarter";
  if (p < 0.48)                return "Waxing Gibbous";
  if (p < 0.52)                return "Full Moon";
  if (p < 0.73)                return "Waning Gibbous";
  if (p < 0.77)                return "Last Quarter";
  return "Waning Crescent";
}
// Small moon icon with the illuminated fraction drawn (scanline method).
static void drawMoonPhase(int cx, int cy, int r, float phase) {
  tft.fillCircle(cx, cy, r, COL_BORDER);          // dark (shadowed) disc
  bool isNew = (phase < 0.0f) || (phase < 0.02f) || (phase > 0.98f);
  if (!isNew) {
    bool waxing = phase < 0.5f;
    double c = cos((double)phase * TWO_PI);       // terminator factor
    for (int y = -r; y <= r; y++) {
      double xr = sqrt((double)r * r - (double)y * y);
      int litL, litR;
      if (waxing) { litL = (int)lround(c * xr); litR = (int)lround(xr); }
      else        { litL = (int)lround(-xr);    litR = (int)lround(-c * xr); }
      if (litR >= litL)
        tft.drawFastHLine(cx + litL, cy + y, litR - litL + 1, WXC_MOON);
    }
  }
  tft.drawCircle(cx, cy, r, COL_DIM);             // outline
}

// ============================================================
//  DISPLAY — TIDES PANEL  (border 147, body 148..240)
// ============================================================
static void drawTideRow(int y, bool high, const String& clock, const String& ht) {
  const int W = tft.width();
  const int m = y + 13;
  tft.fillRect(0, y, W, 26, COL_BG);
  tft.setTextFont(4);
  tft.setTextColor(high ? COL_CYAN : COL_WARN, COL_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString(high ? "High" : "Low", 12, m);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(clock.c_str(), W / 2 + 6, m);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(ht.c_str(), W - 12, m);
}
void drawTidesPanel() {
  const int W = tft.width();
  tft.fillRect(0, 147, W, 241 - 147, COL_BG);
  tft.drawFastHLine(0, 147, W, COL_BORDER);
  tft.setTextFont(2);
  tft.setTextColor(COL_CYAN, COL_BG);
  tft.setTextDatum(ML_DATUM);
  tft.drawString("TIDES", 10, 160);

  // Moon phase (drives the tides) — icon + name on the right.
  float mp = moonPhase();
  drawMoonPhase(W - 15, 160, 9, mp);
  tft.setTextFont(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextDatum(MR_DATUM);
  tft.drawString(moonName(mp), W - 30, 160);

  if (!tides.valid) {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(2);
    tft.drawString("Tides unavailable", W / 2, 207);
    return;
  }
  drawTideRow(178, tides.ev[0].high, tides.ev[0].clock,
              String(tides.ev[0].ht, 1) + " ft");
  if (tides.n > 1)
    drawTideRow(210, tides.ev[1].high, tides.ev[1].clock,
                String(tides.ev[1].ht, 1) + " ft");
  else
    tft.fillRect(0, 210, W, 26, COL_BG);
}

// ============================================================
//  DISPLAY — MARINE / SMALL CRAFT ADVISORY  (border 241, body 242..282)
//  Advisory headline scrolls (marquee sprite).
// ============================================================
static void drawBoat(int cx, int cy, uint16_t col) {
  tft.fillTriangle(cx, cy - 9, cx, cy + 2, cx + 9, cy + 2, col);  // sail
  tft.drawLine(cx, cy - 9, cx, cy + 2, col);                      // mast
  tft.fillRect(cx - 9, cy + 3, 20, 2, col);                       // deck
  tft.drawLine(cx - 9, cy + 5, cx - 5, cy + 9, col);              // hull
  tft.drawLine(cx + 11, cy + 5, cx + 7, cy + 9, col);
}
static uint16_t marineColor() {
  if (!marine.valid || !rip.valid) return COL_DIM;
  return (marine.active || rip.active) ? COL_WARN : COL_GREEN;
}
static void renderMarFrame() {
  if (!marReady) return;
  marSpr.fillSprite(COL_BG);
  marSpr.setTextDatum(ML_DATUM);
  marSpr.setTextColor(marineColor(), COL_BG);
  int y = MAR_H / 2;
  marSpr.drawString(marText, marOff, y, 2);
  if (marScroll)
    marSpr.drawString(marText, marOff + marTextW + MAR_GAP, y, 2);
  marSpr.pushSprite(0, MAR_Y);
}
static void setupMarineMarquee() {
  const int W = tft.width();
  if (!marine.valid || !rip.valid) {
    marText = "North Topsail Beach  -  checking marine/rip conditions...";
  } else if (marine.active && rip.active) {
    marText = "North Topsail Beach  -  " + marine.event + ": " + marine.headline
             + "   |   " + rip.event + ": " + rip.headline;
  } else if (marine.active) {
    marText = "North Topsail Beach  -  " + marine.headline;
  } else if (rip.active) {
    marText = "North Topsail Beach  -  " + rip.headline;
  } else {
    marText = "North Topsail Beach  -  No small craft or rip current advisories in effect";
  }

  marTextW = marReady ? marSpr.textWidth(marText, 2) : tft.textWidth(marText, 2);
  if (marTextW <= W - 8) { marScroll = false; marOff = (W - marTextW) / 2; }
  else                   { marScroll = true;  marOff = 0; lastMarScrollMs = millis(); }
  renderMarFrame();
}
void drawMarinePanel() {
  const int W = tft.width();
  tft.fillRect(0, 241, W, 283 - 241, COL_BG);
  tft.drawFastHLine(0, 241, W, COL_BORDER);
  tft.setTextFont(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("HAZARDS (marine/rip)", 10, 250);
  drawBoat(W - 16, 253, marineColor());
  setupMarineMarquee();          // Font-2 scrolling line at MAR_Y
}

// ============================================================
//  DISPLAY — COASTAL WATERS FORECAST  (border 95, body 96..146)
//  NOAA/NWS text-bulletin synopsis for the current period; scrolls (marquee).
// ============================================================
static void drawWaveIcon(int cx, int cy, uint16_t col) {
  for (int row = -1; row <= 1; row++) {
    int y0 = cy + row * 5;
    for (int i = 0; i < 3; i++) {
      int x0 = cx - 9 + i * 6;
      tft.drawLine(x0, y0 + 2, x0 + 3, y0 - 2, col);
      tft.drawLine(x0 + 3, y0 - 2, x0 + 6, y0 + 2, col);
    }
  }
}
static void renderCoastFrame() {
  if (!coastReady) return;
  coastSpr.fillSprite(COL_BG);
  coastSpr.setTextDatum(ML_DATUM);
  coastSpr.setTextColor(coastal.valid ? COL_TEXT : COL_DIM, COL_BG);
  int y = COAST_H / 2;
  coastSpr.drawString(coastText, coastOff, y, 2);
  if (coastScroll)
    coastSpr.drawString(coastText, coastOff + coastTextW + COAST_GAP, y, 2);
  coastSpr.pushSprite(0, COAST_Y);
}
static void setupCoastalMarquee() {
  const int W = tft.width();
  if (!coastal.valid) coastText = "Coastal waters forecast unavailable";
  else                 coastText = coastal.period + ": " + coastal.text;

  coastTextW = coastReady ? coastSpr.textWidth(coastText, 2) : tft.textWidth(coastText, 2);
  if (coastTextW <= W - 8) { coastScroll = false; coastOff = (W - coastTextW) / 2; }
  else                     { coastScroll = true;  coastOff = 0; lastCoastScrollMs = millis(); }
  renderCoastFrame();
}
void drawCoastalPanel() {
  const int W = tft.width();
  tft.fillRect(0, 95, W, 147 - 95, COL_BG);
  tft.drawFastHLine(0, 95, W, COL_BORDER);
  tft.setTextFont(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("COASTAL (NOAA)", 10, 104);
  drawWaveIcon(W - 16, 107, COL_CYAN);
  setupCoastalMarquee();          // Font-2 scrolling line at COAST_Y
}

// ============================================================
//  DISPLAY — WIND / PRESSURE  (283..307, the strip between MARINE and the
//  footer that was previously unused padding). Comes from the same live
//  station observation as the hero temp/humidity -- no extra API cost.
// ============================================================
static const char* compassDir(float deg) {
  if (isnan(deg)) return "--";
  static const char* DIRS[16] = { "N","NNE","NE","ENE","E","ESE","SE","SSE",
                                   "S","SSW","SW","WSW","W","WNW","NW","NNW" };
  int idx = ((int)((deg + 11.25f) / 22.5f)) % 16;
  if (idx < 0) idx += 16;
  return DIRS[idx];
}
void drawWindPressure() {
  const int W = tft.width();
  tft.fillRect(0, 283, W, 308 - 283, COL_BG);
  tft.setTextFont(2);
  tft.setTextDatum(MC_DATUM);
  if (!wx.valid || isnan(wx.windMph) || isnan(wx.pressureInHg)) {
    tft.setTextColor(COL_DIM, COL_BG);
    tft.drawString("Wind/pressure unavailable", W / 2, 295);
  } else {
    tft.setTextColor(COL_TEXT, COL_BG);
    String line = String(compassDir(wx.windDeg)) + " " + String((int)lroundf(wx.windMph))
                + " mph   |   " + String(wx.pressureInHg, 2) + " inHg";
    tft.drawString(line, W / 2, 295);
  }
  tft.setTextDatum(TL_DATUM);
}

void drawFooter() {
  const int W = tft.width();
  tft.fillRect(0, 308, W, 12, COL_BG);
  tft.setTextFont(1);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.setTextDatum(TL_DATUM);
  String left = WiFi.isConnected()
      ? WiFi.localIP().toString() + "  " + sanitizeMDNS(deviceName) + ".local"
      : "--";
  tft.drawString(left, 4, 309);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("v" FW_VERSION, W - 4, 309);
}

void drawMainScreen() {
  tft.fillScreen(COL_BG);
  drawWeatherPanel();
  drawCoastalPanel();
  drawTidesPanel();
  drawMarinePanel();
  drawWindPressure();
  drawFooter();
  needsFullRedraw = false;
}

// ============================================================
//  HTTPS JSON GET (shared by NWS + NOAA)
// ============================================================
static bool httpsGetJson(const String& url, JsonDocument& doc,
                         JsonDocument* filter, const char* accept) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(9000);
  if (!http.begin(client, url)) return false;
  http.addHeader("User-Agent", NWS_USER_AGENT);
  http.addHeader("Accept", accept);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[HTTPS] %d  %s\n", code, url.c_str());
    http.end();
    return false;
  }
  DeserializationError err = filter
      ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter))
      : deserializeJson(doc, http.getStream());
  http.end();
  if (err) { Serial.printf("[HTTPS] JSON error: %s\n", err.c_str()); return false; }
  return true;
}

// ============================================================
//  WEATHER — NWS FETCH
// ============================================================
static bool resolveForecastUrl() {
  JsonDocument filter;
  filter["properties"]["forecast"] = true;
  JsonDocument doc;
  String pointsUrl = "https://api.weather.gov/points/" + wxLat + "," + wxLon;
  if (!httpsGetJson(pointsUrl.c_str(),
                    doc, &filter, "application/geo+json"))
    return false;
  forecastUrl = doc["properties"]["forecast"].as<String>();
  Serial.printf("[NWS] forecast URL: %s\n", forecastUrl.c_str());
  return forecastUrl.length() > 0;
}
// Live current temp/humidity/wind/pressure from the nearest ASOS station
// (WX_STATION) -- one HTTPS call covers all four, no extra API cost for
// wind + pressure beyond what temp/humidity already needed.
static bool fetchObservation() {
  JsonDocument filter;
  filter["properties"]["temperature"]["value"]        = true;
  filter["properties"]["relativeHumidity"]["value"]   = true;
  filter["properties"]["windSpeed"]["value"]          = true;
  filter["properties"]["windDirection"]["value"]      = true;
  filter["properties"]["barometricPressure"]["value"] = true;

  JsonDocument doc;
  String stationUrl = "https://api.weather.gov/stations/" + wxStation + "/observations/latest";
  if (!httpsGetJson(stationUrl.c_str(),
                    doc, &filter, "application/geo+json"))
    return false;

  JsonObject p = doc["properties"];
  if (p.isNull()) return false;

  float tempC = p["temperature"]["value"].isNull() ? NAN : p["temperature"]["value"].as<float>();
  wx.tempF    = isnan(tempC) ? NAN : (tempC * 9.0f / 5.0f + 32.0f);
  wx.humidity = p["relativeHumidity"]["value"].isNull()
                  ? NAN : p["relativeHumidity"]["value"].as<float>();

  float windKmh = p["windSpeed"]["value"].isNull() ? NAN : p["windSpeed"]["value"].as<float>();
  wx.windMph    = isnan(windKmh) ? NAN : (windKmh * 0.621371f);
  wx.windDeg    = p["windDirection"]["value"].isNull()
                    ? NAN : p["windDirection"]["value"].as<float>();

  float pressPa    = p["barometricPressure"]["value"].isNull()
                        ? NAN : p["barometricPressure"]["value"].as<float>();
  wx.pressureInHg  = isnan(pressPa) ? NAN : (pressPa / 3386.389f);

  return !isnan(wx.tempF);
}

bool fetchWeather() {
  if (!WiFi.isConnected()) return false;
  if (forecastUrl.isEmpty() && !resolveForecastUrl()) return false;

  JsonDocument filter;
  JsonObject fp = filter["properties"]["periods"].add<JsonObject>();
  fp["isDaytime"] = true;
  fp["shortForecast"] = true;

  JsonDocument doc;
  if (!httpsGetJson(forecastUrl, doc, &filter, "application/geo+json")) {
    forecastUrl = "";
    return false;
  }
  JsonObject p = doc["properties"]["periods"][0];
  if (p.isNull()) return false;

  wx.isDay     = p["isDaytime"] | true;
  wx.condition = p["shortForecast"].as<String>();
  wx.icon      = mapCondition(wx.condition);

  wx.valid = fetchObservation();
  Serial.printf("[NWS] %s  %.0fF  hum=%.0f  wind=%.0fmph@%.0f  press=%.2finHg  day=%d\n",
                wx.condition.c_str(), wx.tempF, wx.humidity,
                wx.windMph, wx.windDeg, wx.pressureInHg, wx.isDay);
  return wx.valid;
}

// ============================================================
//  TIDES — NOAA CO-OPS FETCH (next high/low)
// ============================================================
bool fetchTides() {
  if (!WiFi.isConnected()) return false;
  struct tm ti;
  if (!getLocalTime(&ti, 200) || ti.tm_year + 1900 < 2021) {
    Serial.println("[Tide] time not synced yet");
    return false;
  }
  char beginDate[9];  strftime(beginDate, sizeof(beginDate), "%Y%m%d", &ti);
  char nowStr[20];    strftime(nowStr, sizeof(nowStr), "%Y-%m-%d %H:%M", &ti);
  String nowS = nowStr;

  String url = String(
      "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?"
      "product=predictions&application=weatherdisplay&datum=MLLW&station=")
    + tideStation
    + "&time_zone=lst_ldt&units=english&interval=hilo&format=json&begin_date="
    + beginDate + "&range=48";

  JsonDocument filter;
  JsonObject pe = filter["predictions"].add<JsonObject>();
  pe["t"] = true; pe["v"] = true; pe["type"] = true;

  JsonDocument doc;
  if (!httpsGetJson(url, doc, &filter, "application/json")) return false;
  JsonArray arr = doc["predictions"].as<JsonArray>();
  if (arr.isNull()) return false;

  int cnt = 0;
  for (JsonObject p : arr) {
    String t = p["t"].as<String>();
    if (t <= nowS) continue;                 // already passed (string==chrono order)
    tides.ev[cnt].clock = formatClock(t);
    tides.ev[cnt].ht    = p["v"].as<String>().toFloat();
    tides.ev[cnt].high  = (p["type"].as<String>() == "H");
    if (++cnt >= 2) break;
  }
  tides.n = cnt;
  tides.valid = cnt > 0;
  if (tides.valid)
    Serial.printf("[Tide] next: %s %s %.1fft\n",
                  tides.ev[0].high ? "HIGH" : "LOW",
                  tides.ev[0].clock.c_str(), tides.ev[0].ht);
  return tides.valid;
}

// ============================================================
//  MARINE — NWS active alerts for the coastal-waters zone (small craft, etc.)
// ============================================================
bool fetchMarine() {
  if (!WiFi.isConnected()) return false;

  JsonDocument filter;
  JsonObject f = filter["features"].add<JsonObject>();
  f["properties"]["event"]    = true;
  f["properties"]["headline"] = true;

  JsonDocument doc;
  String marineUrl = "https://api.weather.gov/alerts/active/zone/" + marineZone;
  if (!httpsGetJson(marineUrl.c_str(),
                    doc, &filter, "application/geo+json"))
    return false;

  marine.active = false; marine.event = ""; marine.headline = "";
  JsonArray fs = doc["features"].as<JsonArray>();
  if (!fs.isNull()) {
    for (JsonObject ft : fs) {
      String ev = ft["properties"]["event"].as<String>();
      if (ev.isEmpty()) continue;
      // Prefer a Small Craft Advisory; otherwise keep the first alert seen.
      if (!marine.active || ev.indexOf("Small Craft") >= 0) {
        marine.active   = true;
        marine.event    = ev;
        marine.headline = ft["properties"]["headline"].as<String>();
        if (ev.indexOf("Small Craft") >= 0) break;
      }
    }
  }
  marine.valid = true;
  Serial.printf("[Marine] %s\n", marine.active ? marine.event.c_str() : "none");
  return true;
}

// ============================================================
//  RIP CURRENT / BEACH HAZARDS — NWS active alerts for the LAND coastal
//  zone (RIP_ZONE). Same alerts feed/shape as fetchMarine(), just a
//  different zone and preferred event names.
// ============================================================
bool fetchRipCurrent() {
  if (!WiFi.isConnected()) return false;

  JsonDocument filter;
  JsonObject f = filter["features"].add<JsonObject>();
  f["properties"]["event"]    = true;
  f["properties"]["headline"] = true;

  JsonDocument doc;
  String ripUrl = "https://api.weather.gov/alerts/active/zone/" + ripZone;
  if (!httpsGetJson(ripUrl.c_str(),
                    doc, &filter, "application/geo+json"))
    return false;

  rip.active = false; rip.event = ""; rip.headline = "";
  JsonArray fs = doc["features"].as<JsonArray>();
  if (!fs.isNull()) {
    for (JsonObject ft : fs) {
      String ev = ft["properties"]["event"].as<String>();
      if (ev.isEmpty()) continue;
      // Prefer a Rip Current / Beach Hazards statement; otherwise keep the
      // first alert seen for this zone.
      bool isRip = ev.indexOf("Rip Current") >= 0 || ev.indexOf("Beach Hazards") >= 0;
      if (!rip.active || isRip) {
        rip.active   = true;
        rip.event    = ev;
        rip.headline = ft["properties"]["headline"].as<String>();
        if (isRip) break;
      }
    }
  }
  rip.valid = true;
  Serial.printf("[Rip] %s\n", rip.active ? rip.event.c_str() : "none");
  return true;
}

// ============================================================
//  COASTAL WATERS FORECAST — NOAA/NWS text bulletin (product CWF, office MHX)
//  The bulletin covers EVERY marine zone for the office (~25KB of escaped
//  JSON text) -- too large to hold in RAM alongside WiFi/TLS/webserver on
//  this device, so we scan the raw HTTPS response byte-by-byte and only
//  ever keep a few hundred bytes: our zone's header + first forecast
//  period (e.g. ".TONIGHT...NW winds 10 to 15 kt. Seas 2 to 4 ft...").
// ============================================================
#define COASTAL_CAPTURE_MAX 700

static int readByteBlocking(Stream& s, unsigned long deadline) {
  while (millis() < deadline) {
    if (s.available()) return s.read();
    delay(1);
  }
  return -1;
}

// Slices the (small, already-zone-scoped) capture buffer down to just the
// first forecast period: name (e.g. "TONIGHT") + body text.
static bool extractCoastalPeriod(const String& text, String& periodOut, String& bodyOut) {
  int p1 = text.indexOf("\n.");                   // start of the first period marker
  if (p1 < 0) return false;
  int p2 = text.indexOf("\n.", p1 + 1);           // start of the second period marker
  if (p2 < 0) p2 = text.indexOf("\n\n", p1 + 1);  // fallback: blank line after last period
  if (p2 < 0) p2 = min((int)text.length(), p1 + 400);

  String seg = text.substring(p1 + 1, p2);        // e.g. ".TONIGHT...NW winds..."
  int dots = seg.indexOf("...", 1);
  if (dots < 0) return false;
  periodOut = seg.substring(1, dots);
  bodyOut   = seg.substring(dots + 3);
  bodyOut.replace("\n", " ");
  while (bodyOut.indexOf("  ") >= 0) bodyOut.replace("  ", " ");
  bodyOut.trim();
  periodOut.trim();
  return true;
}

// Scans the still-open HTTP response stream for `"productText":"`, then
// JSON-unescapes bytes one at a time looking for our zone's UGC header
// (e.g. "AMZ158-"); once found, captures that zone's segment (bounded by
// COASTAL_CAPTURE_MAX) instead of the whole multi-zone bulletin.
static bool streamCoastalZone(Stream& s, String& capture) {
  unsigned long deadline = millis() + 9000;

  const char* KEY = "\"productText\"";
  size_t klen = strlen(KEY), kmatch = 0;
  int c;
  while ((c = readByteBlocking(s, deadline)) >= 0) {
    kmatch = ((char)c == KEY[kmatch]) ? kmatch + 1 : ((char)c == KEY[0] ? 1 : 0);
    if (kmatch == klen) break;
  }
  if (kmatch != klen) return false;
  // Skip ':' and any whitespace up to the value's opening quote (JSON
  // formatting between key and value is not guaranteed byte-for-byte —
  // this API pretty-prints with a space: `"productText": "`).
  do {
    c = readByteBlocking(s, deadline);
    if (c < 0) return false;
  } while (c != '"');

  String zonePrefix = coastalZone + "-";
  size_t zlen = zonePrefix.length(), zmatch = 0;
  bool capturing = false;
  char prev = 0;
  int boundaries = 0;

  while ((c = readByteBlocking(s, deadline)) >= 0) {
    char ch;
    if (c == '\\') {
      int esc = readByteBlocking(s, deadline);
      if (esc < 0) break;
      switch (esc) {
        case 'n':  ch = '\n'; break;
        case 't':  ch = '\t'; break;
        case 'r':  ch = '\r'; break;
        case '"':  ch = '"';  break;
        case '\\': ch = '\\'; break;
        default:   ch = (char)esc; break;
      }
    } else if (c == '"') {
      break;                       // unescaped quote: end of the JSON string
    } else {
      ch = (char)c;
    }

    if (!capturing) {
      zmatch = (ch == zonePrefix[zmatch]) ? zmatch + 1 : (ch == zonePrefix[0] ? 1 : 0);
      if (zmatch == zlen) { capturing = true; capture = zonePrefix; }
      continue;
    }
    capture += ch;
    if (prev == '\n' && ch == '.' && ++boundaries >= 2) break;
    prev = ch;
    if (capture.length() >= COASTAL_CAPTURE_MAX) break;
  }
  return capturing;
}

bool fetchCoastalForecast() {
  if (!WiFi.isConnected()) return false;

  // Step 1: look up the most recently issued CWF bulletin for this office.
  JsonDocument listFilter;
  JsonObject g = listFilter["@graph"].add<JsonObject>();
  g["@id"] = true;
  JsonDocument listDoc;
  String coastalUrl = "https://api.weather.gov/products/types/CWF/locations/" + coastalOffice;
  if (!httpsGetJson(coastalUrl.c_str(),
                    listDoc, &listFilter, "application/ld+json"))
    return false;
  JsonArray graph = listDoc["@graph"].as<JsonArray>();
  if (graph.isNull() || graph.size() == 0) return false;
  String productUrl = graph[0]["@id"].as<String>();
  if (productUrl.isEmpty()) return false;
  listDoc.clear();

  // Step 2: stream-scan the bulletin for our zone; never buffer the whole
  // ~25KB document (every marine zone for the office is in that one file).
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(9000);
  if (!http.begin(client, productUrl)) return false;
  http.addHeader("User-Agent", NWS_USER_AGENT);
  http.addHeader("Accept", "application/ld+json");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Coastal] HTTP %d\n", code);
    http.end();
    return false;
  }

  String capture;
  bool found = streamCoastalZone(http.getStream(), capture);
  http.end();
  if (!found) { Serial.println("[Coastal] zone not found in bulletin"); return false; }

  String period, body;
  if (!extractCoastalPeriod(capture, period, body)) return false;
  coastal.period = period;
  coastal.text   = body;
  coastal.valid  = true;
  Serial.printf("[Coastal] %s: %s\n", period.c_str(), body.c_str());
  return true;
}

// ============================================================
//  STATUS JSON  (for the web dashboard)
// ============================================================
String buildStatusJson() {
  unsigned long nowMs = millis();
  JsonDocument doc;
  doc["firmware"] = FW_VERSION;
  doc["device"]   = deviceName;
  doc["mdns"]     = sanitizeMDNS(deviceName);
  doc["uptime"]   = (nowMs - bootMillis) / 1000UL;
  doc["place"]    = wxPlace;

  doc["wxValid"] = wx.valid;
  if (!isnan(wx.tempF))    doc["wxTempF"]    = wx.tempF;
  if (!isnan(wx.humidity)) doc["wxHumidity"] = wx.humidity;
  if (!isnan(wx.windMph)) {
    doc["wxWindMph"] = wx.windMph;
    doc["wxWindDir"] = compassDir(wx.windDeg);
  }
  if (!isnan(wx.pressureInHg)) doc["wxPressureInHg"] = wx.pressureInHg;
  doc["wxCondition"] = wx.condition;
  doc["wxIsDay"]     = wx.isDay;
  doc["wxAgoSec"]    = lastWeatherOkMs == 0
                         ? -1 : (long)((nowMs - lastWeatherOkMs) / 1000UL);

  doc["tideValid"] = tides.valid;
  if (tides.valid) {
    JsonArray ta = doc["tides"].to<JsonArray>();
    for (int i = 0; i < tides.n; i++) {
      JsonObject o = ta.add<JsonObject>();
      o["type"] = tides.ev[i].high ? "H" : "L";
      o["time"] = tides.ev[i].clock;
      o["ht"]   = tides.ev[i].ht;
    }
  }
  doc["tideAgoSec"] = lastTideOkMs == 0
                        ? -1 : (long)((nowMs - lastTideOkMs) / 1000UL);

  doc["marineValid"]    = marine.valid;
  doc["marineAdvisory"] = marine.active;
  if (marine.active) {
    doc["marineEvent"]    = marine.event;
    doc["marineHeadline"] = marine.headline;
  }

  doc["ripValid"]  = rip.valid;
  doc["ripActive"] = rip.active;
  if (rip.active) {
    doc["ripEvent"]    = rip.event;
    doc["ripHeadline"] = rip.headline;
  }

  doc["coastalValid"] = coastal.valid;
  if (coastal.valid) {
    doc["coastalPeriod"] = coastal.period;
    doc["coastalText"]   = coastal.text;
  }

  float mp = moonPhase();
  if (mp >= 0) {
    doc["moonPhase"] = mp;
    doc["moonName"]  = moonName(mp);
    doc["moonIllum"] = (int)lroundf((1.0f - cosf(mp * TWO_PI)) / 2.0f * 100.0f);
  }

  doc["ssid"]    = WiFi.isConnected() ? WiFi.SSID()               : "Not connected";
  doc["ip"]      = WiFi.isConnected() ? WiFi.localIP().toString() : "--";
  doc["mac"]     = WiFi.macAddress();
  doc["rssi"]    = WiFi.isConnected() ? WiFi.RSSI() : 0;
  doc["quality"] = wifiQuality(WiFi.RSSI());

  String out; serializeJson(doc, out);
  return out;
}

// ============================================================
//  WEB ROUTE HANDLERS
// ============================================================
void handleStatus() { server.send(200, "application/json", buildStatusJson()); }
void handleHealth() { server.send(200, "text/plain", "OK"); }

void handleRename() {
  if (!server.hasArg("name") || server.arg("name").isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"Missing name\"}"); return;
  }
  String raw = server.arg("name"); raw.trim();
  deviceName = raw;
  saveSettings();
  startMDNS();
  server.send(200, "application/json",
              "{\"ok\":true,\"name\":\"" + sanitizeMDNS(deviceName) + "\"}");
}
void handleRefresh() {
  nextWeatherAt = 0;
  nextTideAt    = 0;
  server.send(200, "application/json", "{\"ok\":true}");
}
void handleWiFiReset() {
  server.send(200, "application/json",
              "{\"ok\":true,\"msg\":\"Rebooting into setup mode.\"}");
  clearWiFiCreds();
  delay(1000);
  ESP.restart();
}
void handleWiFiChange() {
  if (!server.hasArg("ssid") || server.arg("ssid").isEmpty()) {
    server.send(400, "application/json", "{\"error\":\"Missing SSID\"}"); return;
  }
  String ssid = server.arg("ssid");
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  saveWiFiCreds(ssid, pass);
  server.send(200, "application/json", "{\"ok\":true}");
  delay(800);
  ESP.restart();
}
void handleWiFiScanStart() {
  WiFi.scanNetworks(true);
  server.send(200, "application/json", "{\"ok\":true,\"status\":\"scanning\"}");
}
void handleWiFiScanResult() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    server.send(200, "application/json", "{\"status\":\"scanning\"}"); return;
  }
  JsonDocument doc;
  if (n < 0) doc["status"] = "error";
  else {
    doc["status"] = "done";
    JsonArray arr = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      JsonObject net = arr.add<JsonObject>();
      net["ssid"]   = WiFi.SSID(i);
      net["rssi"]   = WiFi.RSSI(i);
      net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
  }
  WiFi.scanDelete();
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ============================================================
//  WEB DASHBOARD
// ============================================================
void handleRoot() {
  server.send(200, "text/html", R"DASH(
<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Weather Display</title>
<style>
:root{--bg:#0d0f1a;--card:#151724;--bdr:#252840;--acc:#4f8ef7;
 --grn:#3ecf8e;--red:#f76e6e;--warn:#f7b84f;--dim:#8892a4;--txt:#dde3f0;--cya:#4fcfef;}
*{box-sizing:border-box;margin:0;padding:0}
body{background:var(--bg);color:var(--txt);font-family:'Segoe UI',Arial,sans-serif;
 padding:1rem;max-width:560px;margin:auto}
h1{text-align:center;color:var(--acc);font-size:1.3rem;margin:.8rem 0 .3rem}
.card{background:var(--card);border:1px solid var(--bdr);border-radius:14px;
 padding:1.2rem 1.4rem;margin-bottom:1rem}
.card h2{font-size:.78rem;text-transform:uppercase;letter-spacing:.1em;
 color:var(--dim);margin-bottom:.9rem}
.row{display:flex;justify-content:space-between;align-items:center;
 padding:.35rem 0;border-bottom:1px solid var(--bdr);font-size:.9rem}
.row:last-child{border-bottom:none}
.lbl{color:var(--dim);font-size:.88rem}.val{font-weight:600;font-size:.92rem}
.btn-row{display:flex;gap:.6rem;flex-wrap:wrap;margin-top:.85rem}
button{padding:.5rem .95rem;border:none;border-radius:8px;font-size:.86rem;
 font-weight:600;cursor:pointer;transition:opacity .15s}
button:hover{opacity:.83}button:active{opacity:.65}
.b-blue{background:var(--acc);color:#fff}.b-red{background:var(--red);color:#fff}
.b-dim{background:var(--bdr);color:var(--txt)}
.irow{display:flex;gap:.5rem;margin-top:.7rem}
input[type=text],input[type=password]{flex:1;padding:.5rem .8rem;
 background:#1e2535;border:1px solid var(--bdr);border-radius:8px;
 color:var(--txt);font-size:.9rem;outline:none}
input:focus{border-color:var(--acc)}
.ota-link{display:block;text-align:center;color:var(--acc);text-decoration:none;
 font-size:.92rem;font-weight:600;padding:.4rem 0}
.net-list{margin-top:.7rem;max-height:180px;overflow-y:auto}
.net-item{display:flex;justify-content:space-between;align-items:center;
 padding:.4rem .6rem;border-radius:.4rem;cursor:pointer;margin-bottom:.3rem;background:#1e2535}
.net-item:hover{background:var(--bdr)}
.lock{font-size:.72rem;margin-left:.3rem;color:var(--warn)}
#scanStatus{font-size:.82rem;color:var(--dim);margin:.4rem 0;text-align:center}
#upd{text-align:center;font-size:.73rem;color:var(--dim);margin-top:.4rem}
</style></head><body>
<h1 id="hTitle">Weather Display</h1>

<div class="card">
  <h2>Weather <span id="wxPlace" style="color:var(--txt);text-transform:none;letter-spacing:0"></span></h2>
  <div class="row"><span class="lbl">Condition</span><span class="val" id="wxCond">--</span></div>
  <div class="row"><span class="lbl">Current temp</span><span class="val" id="wxTemp">--</span></div>
  <div class="row"><span class="lbl">Current humidity</span><span class="val" id="wxHum">--</span></div>
  <div class="row"><span class="lbl">Wind</span><span class="val" id="wxWind">--</span></div>
  <div class="row"><span class="lbl">Pressure</span><span class="val" id="wxPress">--</span></div>
  <div class="row"><span class="lbl">Updated</span><span class="val" id="wxAge">--</span></div>
  <div class="btn-row"><button class="b-dim" onclick="refreshAll()">Refresh now</button></div>
</div>

<div class="card">
  <h2>Tides — Hampstead</h2>
  <div id="tideRows"><div class="row"><span class="lbl">--</span><span class="val"></span></div></div>
  <div class="row"><span class="lbl">Moon phase</span><span class="val" id="moon">--</span></div>
</div>

<div class="card">
  <h2>Hazards — Marine / Rip Current</h2>
  <div class="row"><span class="lbl">Small craft</span><span class="val" id="marStatus">--</span></div>
  <div class="row"><span class="lbl">Details</span><span class="val" id="marHead" style="text-align:right;max-width:62%">--</span></div>
  <div class="row"><span class="lbl">Rip current</span><span class="val" id="ripStatus">--</span></div>
  <div class="row"><span class="lbl">Details</span><span class="val" id="ripHead" style="text-align:right;max-width:62%">--</span></div>
</div>

<div class="card">
  <h2>Coastal Waters Forecast — NOAA</h2>
  <div class="row"><span class="lbl">Period</span><span class="val" id="coastPeriod">--</span></div>
  <div class="row"><span class="lbl">Forecast</span><span class="val" id="coastText" style="text-align:right;max-width:62%">--</span></div>
</div>

<div class="card">
  <h2>Network</h2>
  <div class="row"><span class="lbl">SSID</span><span class="val" id="ssid">--</span></div>
  <div class="row"><span class="lbl">IP address</span><span class="val" id="ip">--</span></div>
  <div class="row"><span class="lbl">MAC address</span><span class="val" id="mac">--</span></div>
  <div class="row"><span class="lbl">Signal</span><span class="val" id="quality">--</span></div>
  <div class="row"><span class="lbl">mDNS hostname</span><span class="val" id="mdnsLabel">--</span></div>

  <h2 style="margin-top:1.1rem">Change WiFi Network</h2>
  <button class="b-dim" style="width:100%;margin-top:.5rem" onclick="doScan()">Scan for networks</button>
  <div id="scanStatus"></div>
  <div class="net-list" id="netList"></div>
  <div class="irow">
    <input type="text" id="wifiSSID" placeholder="Selected or type SSID" autocomplete="off">
    <input type="password" id="wifiPass" placeholder="Password" autocomplete="new-password">
  </div>
  <div class="btn-row" style="margin-top:.7rem">
    <button class="b-blue" onclick="changeWiFi()">Connect to network</button>
    <button class="b-red"  onclick="resetWiFi()">Reset WiFi credentials</button>
  </div>

  <h2 style="margin-top:1.1rem">Change Device Name / mDNS</h2>
  <div class="irow">
    <input type="text" id="renameInput" placeholder="new-hostname">
    <button class="b-blue" onclick="renameDevice()">Save</button>
  </div>
</div>

<div class="card">
  <h2>Firmware <span id="fwVer" style="font-weight:600;color:var(--txt)"></span></h2>
  <a class="ota-link" href="/update">Open OTA update page &#x2197;</a>
</div>

<div id="upd">Connecting...</div>
<script>
var $=function(id){return document.getElementById(id);};
function toast(m,ms){ms=ms||2500;var d=document.createElement('div');d.textContent=m;
 d.style.cssText='position:fixed;bottom:1.4rem;left:50%;transform:translateX(-50%);'
 +'background:#252840;color:#dde3f0;padding:.55rem 1.1rem;border-radius:8px;'
 +'font-size:.86rem;z-index:9999;box-shadow:0 2px 12px rgba(0,0,0,.4);transition:opacity .4s';
 document.body.appendChild(d);setTimeout(function(){d.style.opacity='0';
 setTimeout(function(){d.remove();},500);},ms);}
function update(){
 fetch('/status').then(function(r){return r.json();}).then(function(d){
  $('hTitle').textContent=d.device||'Weather Display';
  document.title=d.device||'Weather Display';
  $('fwVer').textContent='v'+(d.firmware||'');
  $('wxPlace').textContent=d.place?('— '+d.place):'';
  $('wxCond').textContent=d.wxValid?(d.wxCondition||'--'):'Offline';
  $('wxTemp').textContent=d.wxTempF!=null?Math.round(d.wxTempF)+'°F':'--';
  $('wxHum').textContent=d.wxHumidity!=null?Math.round(d.wxHumidity)+'%':'--';
  $('wxWind').textContent=d.wxWindMph!=null?(d.wxWindDir+' '+Math.round(d.wxWindMph)+' mph'):'--';
  $('wxPress').textContent=d.wxPressureInHg!=null?(d.wxPressureInHg.toFixed(2)+' inHg'):'--';
  $('wxAge').textContent=d.wxAgoSec<0?'never':d.wxAgoSec+'s ago';
  var tr=$('tideRows');
  if(d.tideValid&&d.tides&&d.tides.length){
    tr.innerHTML=d.tides.map(function(x){return '<div class="row"><span class="lbl">'
     +(x.type==='H'?'High':'Low')+'</span><span class="val">'+x.time+' — '
     +x.ht.toFixed(1)+' ft</span></div>';}).join('');
  } else tr.innerHTML='<div class="row"><span class="lbl">Tides</span><span class="val">unavailable</span></div>';
  $('moon').textContent=d.moonName?(d.moonName+' ('+(d.moonIllum!=null?d.moonIllum+'%':'')+')'):'--';
  $('marStatus').textContent=d.marineValid?(d.marineAdvisory?(d.marineEvent||'Advisory'):'None in effect'):'--';
  $('marHead').textContent=d.marineHeadline||'';
  $('ripStatus').textContent=d.ripValid?(d.ripActive?(d.ripEvent||'Advisory'):'None in effect'):'--';
  $('ripHead').textContent=d.ripHeadline||'';
  $('coastPeriod').textContent=d.coastalValid?(d.coastalPeriod||'--'):'--';
  $('coastText').textContent=d.coastalValid?(d.coastalText||''):'unavailable';
  $('ssid').textContent=d.ssid;$('ip').textContent=d.ip;$('mac').textContent=d.mac;
  $('quality').textContent=d.quality+' ('+d.rssi+' dBm)';
  $('mdnsLabel').textContent=(d.mdns||'')+'.local';
  $('upd').textContent='Updated: '+new Date().toLocaleTimeString();
 }).catch(function(){$('upd').textContent='Connection lost — retrying...';});
}
function refreshAll(){fetch('/refresh').then(function(){toast('Refreshing...');
 setTimeout(update,3500);});}
function renameDevice(){var n=$('renameInput').value.trim();if(!n){toast('Enter a name first');return;}
 fetch('/device/rename?name='+encodeURIComponent(n)).then(function(r){return r.json();})
 .then(function(d){if(d.ok){toast('Renamed to: '+d.name);$('renameInput').value='';update();}
  else toast('Error: '+(d.error||'unknown'));});}
function changeWiFi(){var s=$('wifiSSID').value.trim(),p=$('wifiPass').value;
 if(!s){toast('Enter or select a network first');return;}
 var fd=new FormData();fd.append('ssid',s);fd.append('pass',p);
 fetch('/wifi/change',{method:'POST',body:fd}).then(function(){toast('Saved. Rebooting...',4000);});}
function resetWiFi(){fetch('/wifi/reset').then(function(){toast('Rebooting into setup mode...',4000);});}
var scanTimer=null;
function signalBars(r){return r>-50?'||||':r>-60?'||| ':r>-70?'||  ':'|   ';}
function doScan(){$('scanStatus').textContent='Starting scan...';$('netList').innerHTML='';
 fetch('/wifi/scan/start').then(function(){$('scanStatus').textContent='Scanning... (5-10 s)';
 clearInterval(scanTimer);scanTimer=setInterval(pollScan,2000);})
 .catch(function(){$('scanStatus').textContent='Scan request failed';});}
function pollScan(){fetch('/wifi/scan/result').then(function(r){return r.json();}).then(function(d){
 if(d.status==='scanning')return;clearInterval(scanTimer);
 if(d.status==='error'||!d.networks){$('scanStatus').textContent='Scan failed';return;}
 $('scanStatus').textContent=d.networks.length+' network'+(d.networks.length!==1?'s':'')+' found';
 d.networks.forEach(function(n){var div=document.createElement('div');div.className='net-item';
  div.innerHTML='<span>'+n.ssid+(n.secure?'<span class="lock">&#x1F512;</span>':'')+'</span>'
  +'<span style="font-size:.75rem;color:var(--dim)">'+signalBars(n.rssi)+' '+n.rssi+' dBm</span>';
  div.onclick=function(){$('wifiSSID').value=n.ssid;$('wifiPass').focus();};
  $('netList').appendChild(div);});
 }).catch(function(){clearInterval(scanTimer);$('scanStatus').textContent='Poll failed';});}
setInterval(update,15000);update();
</script></body></html>
)DASH");
}

void setupRoutes() {
  server.on("/",                 HTTP_GET,  handleRoot);
  server.on("/status",           HTTP_GET,  handleStatus);
  server.on("/health",           HTTP_GET,  handleHealth);
  server.on("/device/rename",    HTTP_GET,  handleRename);
  server.on("/refresh",          HTTP_GET,  handleRefresh);
  server.on("/wifi/scan/start",  HTTP_GET,  handleWiFiScanStart);
  server.on("/wifi/scan/result", HTTP_GET,  handleWiFiScanResult);
  server.on("/wifi/change",      HTTP_POST, handleWiFiChange);
  server.on("/wifi/reset",       HTTP_GET,  handleWiFiReset);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
}

// ============================================================
//  AP SETUP PORTAL
// ============================================================
void startAPPortal() {
  inAPMode = true;
  String ssid = apSsid();
  Serial.printf("[WiFi] Starting AP: %s\n", ssid.c_str());
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid.c_str());
  Serial.printf("[WiFi] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  drawAPScreen();

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", R"HTML(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Weather Display Setup</title>
<style>
body{background:#111;color:#eee;font-family:Arial,sans-serif;display:flex;
 justify-content:center;align-items:center;min-height:100vh;margin:0;padding:1rem;box-sizing:border-box}
.box{background:#1e1e2e;padding:2rem;border-radius:1rem;width:100%;max-width:340px;
 text-align:center;box-shadow:0 4px 24px rgba(0,0,0,.5)}
h2{color:#4f8ef7;margin-bottom:1.5rem}
input{width:100%;padding:.6rem;margin:.4rem 0 .8rem;border-radius:.5rem;
 border:1px solid #444;background:#2a2a3e;color:#fff;font-size:1rem;box-sizing:border-box}
label{display:block;text-align:left;color:#8892a4;font-size:.82rem;margin-bottom:.1rem}
button{width:100%;padding:.7rem;background:#4f8ef7;color:#fff;border:none;
 border-radius:.5rem;font-size:1rem;cursor:pointer;margin-bottom:.6rem}
button:hover{background:#3a7de0}.scan-btn{background:#252840}.scan-btn:hover{background:#333658}
.note{font-size:.8rem;color:#888;margin-top:.8rem}
.net-item{display:flex;justify-content:space-between;align-items:center;
 padding:.4rem .6rem;border-radius:.4rem;cursor:pointer;margin-bottom:.3rem;background:#252840;text-align:left}
.net-item:hover{background:#333658}.lock{font-size:.75rem;margin-left:.4rem;color:#f7b84f}
#netList{margin-bottom:.8rem;max-height:180px;overflow-y:auto}
#scanning{color:#8892a4;font-size:.85rem;margin:.5rem 0}
</style></head><body>
<div class="box">
  <h2>Weather Display WiFi Setup</h2>
  <button class="scan-btn" onclick="doScan()">Scan for Networks</button>
  <div id="scanning" style="display:none">Scanning...</div>
  <div id="netList"></div>
  <label>WiFi Network</label>
  <input id="ssidInput" placeholder="Select above or type SSID" autocomplete="off">
  <label>Password</label>
  <input id="passInput" placeholder="Password" type="password" autocomplete="new-password">
  <button onclick="doConnect()">Save &amp; Connect</button>
  <p class="note">Device will restart and connect to your network.</p>
</div>
<script>
function doScan(){var list=document.getElementById('netList'),sc=document.getElementById('scanning');
 list.innerHTML='';sc.style.display='block';
 fetch('/wifi/scan').then(function(r){return r.json();}).then(function(nets){sc.style.display='none';
  if(!nets.length){list.innerHTML='<p style="color:#8892a4;font-size:.85rem">No networks found</p>';return;}
  nets.forEach(function(n){var d=document.createElement('div');d.className='net-item';
   d.innerHTML='<span>'+n.ssid+(n.secure?'<span class="lock">&#x1F512;</span>':'')+'</span>'
   +'<span style="font-size:.75rem;color:#8892a4">'+n.rssi+' dBm</span>';
   d.onclick=function(){document.getElementById('ssidInput').value=n.ssid;};list.appendChild(d);});
 }).catch(function(){sc.textContent='Scan failed — try again';});}
function doConnect(){var s=document.getElementById('ssidInput').value.trim();
 var p=document.getElementById('passInput').value;
 if(!s){alert('Enter or select an SSID first.');return;}
 var fd=new FormData();fd.append('ssid',s);fd.append('password',p);
 fetch('/save',{method:'POST',body:fd}).then(function(r){return r.text();})
 .then(function(t){document.body.innerHTML=t;});}
doScan();
</script></body></html>
)HTML");
  });

  server.on("/save", HTTP_POST, []() {
    if (!server.hasArg("ssid") || server.arg("ssid").isEmpty()) {
      server.send(400, "text/plain", "SSID is required"); return;
    }
    String ssid = server.arg("ssid"); ssid.trim();
    String pass = server.hasArg("password") ? server.arg("password") : "";
    saveWiFiCreds(ssid, pass);
    server.send(200, "text/html", R"HTML(
<!DOCTYPE html><html><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Weather Location Setup</title>
<style>
body{background:#111;color:#eee;font-family:Arial,sans-serif;display:flex;
 justify-content:center;align-items:center;min-height:100vh;margin:0;padding:1rem;box-sizing:border-box}
.box{background:#1e1e2e;padding:2rem;border-radius:1rem;width:100%;max-width:340px;
 text-align:center;box-shadow:0 4px 24px rgba(0,0,0,.5)}
h2{color:#4f8ef7;margin-bottom:1.5rem}
input{width:100%;padding:.6rem;margin:.4rem 0 .8rem;border-radius:.5rem;
 border:1px solid #444;background:#2a2a3e;color:#fff;font-size:1rem;box-sizing:border-box}
label{display:block;text-align:left;color:#8892a4;font-size:.82rem;margin-bottom:.1rem}
button{width:100%;padding:.7rem;background:#4f8ef7;color:#fff;border:none;
 border-radius:.5rem;font-size:1rem;cursor:pointer;margin-bottom:.6rem}
button:hover{background:#3a7de0}
.note{font-size:.8rem;color:#888;margin-top:.8rem}
</style></head><body>
<div class="box">
  <h2>Location Setup</h2>
  <label>Latitude</label>
  <input id="latInput" type="number" step="0.0001" placeholder="e.g. 34.3671" autocomplete="off">
  <label>Longitude</label>
  <input id="lonInput" type="number" step="0.0001" placeholder="e.g. -77.7108" autocomplete="off">
  <label>Location Name</label>
  <input id="placeInput" placeholder="e.g. Hampstead, NC" autocomplete="off">
  <button onclick="doSave()">Save &amp; Finish</button>
  <p class="note">Device will restart and connect to your WiFi.</p>
</div>
<script>
function doSave(){var lat=document.getElementById('latInput').value.trim();
 var lon=document.getElementById('lonInput').value.trim();
 var place=document.getElementById('placeInput').value.trim();
 if(!lat||!lon){alert('Latitude and longitude are required.');return;}
 var fd=new FormData();fd.append('lat',lat);fd.append('lon',lon);fd.append('place',place);
 fetch('/save-location',{method:'POST',body:fd}).then(function(r){return r.text();})
 .then(function(t){document.body.innerHTML=t;});}
</script></body></html>
)HTML");
  });

  server.on("/save-location", HTTP_POST, []() {
    String lat = server.hasArg("lat") ? server.arg("lat") : "";
    String lon = server.hasArg("lon") ? server.arg("lon") : "";
    String place = server.hasArg("place") ? server.arg("place") : "";
    if (lat.isEmpty() || lon.isEmpty()) {
      server.send(400, "text/plain", "Latitude and longitude are required");
      return;
    }
    saveLocationConfig(lat, lon, place.isEmpty() ? (lat + ", " + lon) : place);
    String mdnsName = sanitizeMDNS(deviceName);
    server.send(200, "text/html",
      String("<!DOCTYPE html><html><head>")
      + "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      + "<style>body{background:#111;color:#eee;font-family:Arial;text-align:center;padding-top:3rem}</style></head><body>"
      + "<h2 style='color:#3ecf8e'>Setup Complete!</h2><p>Restarting...</p>"
      + "<p style='color:#888;font-size:.9rem'>Reconnect to your WiFi, then visit<br>"
      + "<strong>http://" + mdnsName + ".local</strong></p></body></html>");
    delay(1500);
    ESP.restart();
  });

  server.on("/wifi/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int i = 0; i < n; i++) {
      JsonObject net = arr.add<JsonObject>();
      net["ssid"]   = WiFi.SSID(i);
      net["rssi"]   = WiFi.RSSI(i);
      net["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    String out; serializeJson(doc, out);
    server.send(200, "application/json", out);
  });

  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();
}

// ============================================================
//  WiFi — STATION CONNECT
// ============================================================
bool connectWiFi() {
  String ssid, pass;
  loadWiFiCreds(ssid, pass);
  if (ssid.isEmpty()) { Serial.println("[WiFi] No saved credentials"); return false; }
  Serial.printf("[WiFi] Connecting to: %s\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] Connected  IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
  }
  Serial.println("[WiFi] Connection failed");
  return false;
}

// ============================================================
//  ArduinoOTA
// ============================================================
void setupArduinoOTA() {
  ArduinoOTA.setHostname(sanitizeMDNS(deviceName).c_str());
  ArduinoOTA.onStart([]() {
    tft.fillScreen(COL_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COL_ACCENT, COL_BG);
    tft.setTextFont(4);
    tft.drawString("OTA Update...", 120, 120);
  });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    if (t > 0) {
      int pct = p * 100 / t;
      tft.fillRect(20, 155, 200 * pct / 100, 12, COL_ACCENT);
      tft.drawRect(20, 155, 200, 12, COL_BORDER);
    }
  });
  ArduinoOTA.onEnd([]() {
    tft.setTextColor(COL_GREEN, COL_BG);
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Done! Rebooting...", 120, 180);
  });
  ArduinoOTA.begin();
  otaRunning = true;
  Serial.println("[ArduinoOTA] Ready on port 3232");
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[BOOT] Weather Display v" FW_VERSION);
  bootMillis = millis();

  // Force backlight on before init — GPIO floats low after OTA reboot.
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.init();
  tft.setRotation(0);          // portrait; change to 2 if upside-down
  initColors();

  // Scrolling marquee sprites (full width).
  condSpr.setColorDepth(16);
  condReady = (condSpr.createSprite(tft.width(), COND_H) != nullptr);
  if (condReady) condSpr.setTextFont(2);
  marSpr.setColorDepth(16);
  marReady = (marSpr.createSprite(tft.width(), MAR_H) != nullptr);
  if (marReady) marSpr.setTextFont(2);
  coastSpr.setColorDepth(16);
  coastReady = (coastSpr.createSprite(tft.width(), COAST_H) != nullptr);
  if (coastReady) coastSpr.setTextFont(2);

  drawBootScreen();
  loadPrefs();
  loadLocationConfig(wxLat, wxLon, wxPlace);

  // Initialize other location variables to defaults (can be extended later for configurability)
  wxStation = DEFAULT_WX_STATION;
  tideStation = DEFAULT_TIDE_STATION;
  marineZone = DEFAULT_MARINE_ZONE;
  ripZone = DEFAULT_RIP_ZONE;
  coastalZone = DEFAULT_COASTAL_ZONE;
  coastalOffice = DEFAULT_COASTAL_OFFICE;

  Serial.printf("[Config] Location: %s (%s, %s)\n", wxPlace.c_str(), wxLat.c_str(), wxLon.c_str());

  String ssid, pass;
  loadWiFiCreds(ssid, pass);
  bool connected = !ssid.isEmpty() && connectWiFi();
  if (!connected) {
    Serial.println(ssid.isEmpty() ? "[WiFi] No creds — AP" : "[WiFi] Connect failed — AP");
    startAPPortal();
    return;
  }

  esp_wifi_set_ps(WIFI_PS_NONE);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", TZ_STRING, 1);
  tzset();

  startMDNS();
  setupArduinoOTA();
  setupRoutes();
  ElegantOTA.begin(&server, OTA_USER, OTA_PASS);
  server.begin();
  Serial.println("[HTTP] Server started");

  needsFullRedraw = true;
  nextWeatherAt   = 0;         // weather immediately
  nextTideAt      = 3000;      // tides a few seconds later (needs NTP)
  nextMarineAt    = 6000;      // marine advisory shortly after
  nextCoastAt     = 9000;      // coastal forecast bulletin shortly after that
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  server.handleClient();
  ElegantOTA.loop();
  if (otaRunning) ArduinoOTA.handle();

  // WiFi reconnect watchdog.
  if (!inAPMode && WiFi.getMode() == WIFI_STA
      && WiFi.status() != WL_CONNECTED
      && millis() - lastWiFiRetry >= WIFI_RETRY_INTERVAL) {
    lastWiFiRetry = millis();
    Serial.println("[WiFi] Reconnecting...");
    String ssid, pass; loadWiFiCreds(ssid, pass);
    WiFi.begin(ssid.c_str(), pass.c_str());
  }

  if (inAPMode) return;

  if (needsFullRedraw) drawMainScreen();

  // Scrolling condition marquee.
  if (condScroll && millis() - lastScrollMs >= SCROLL_STEP_MS) {
    lastScrollMs = millis();
    condOff -= SCROLL_STEP_PX;
    if (condOff <= -(condTextW + COND_GAP)) condOff += (condTextW + COND_GAP);
    renderCondFrame();
  }

  // Scrolling marine-advisory marquee.
  if (marScroll && millis() - lastMarScrollMs >= SCROLL_STEP_MS) {
    lastMarScrollMs = millis();
    marOff -= SCROLL_STEP_PX;
    if (marOff <= -(marTextW + MAR_GAP)) marOff += (marTextW + MAR_GAP);
    renderMarFrame();
  }

  // Scrolling coastal-forecast marquee.
  if (coastScroll && millis() - lastCoastScrollMs >= SCROLL_STEP_MS) {
    lastCoastScrollMs = millis();
    coastOff -= SCROLL_STEP_PX;
    if (coastOff <= -(coastTextW + COAST_GAP)) coastOff += (coastTextW + COAST_GAP);
    renderCoastFrame();
  }

  // Weather refresh.
  if (WiFi.isConnected() && millis() >= nextWeatherAt) {
    if (fetchWeather()) {
      lastWeatherOkMs = millis();
      nextWeatherAt   = millis() + WEATHER_INTERVAL_MS;
      drawWeatherPanel();
      drawWindPressure();
    } else {
      nextWeatherAt = millis() + WEATHER_RETRY_MS;
    }
  }

  // Tide refresh.
  if (WiFi.isConnected() && millis() >= nextTideAt) {
    if (fetchTides()) {
      lastTideOkMs = millis();
      nextTideAt   = millis() + TIDE_INTERVAL_MS;
      drawTidesPanel();
    } else {
      nextTideAt = millis() + TIDE_RETRY_MS;
    }
  }

  // Marine + rip current hazards refresh (share one schedule/panel).
  if (WiFi.isConnected() && millis() >= nextMarineAt) {
    bool okMarine = fetchMarine();
    bool okRip    = fetchRipCurrent();
    nextMarineAt = millis() + ((okMarine && okRip) ? MARINE_INTERVAL_MS : MARINE_RETRY_MS);
    drawMarinePanel();
  }

  // Coastal waters forecast refresh.
  if (WiFi.isConnected() && millis() >= nextCoastAt) {
    if (fetchCoastalForecast()) {
      nextCoastAt = millis() + COASTAL_INTERVAL_MS;
      drawCoastalPanel();
    } else {
      nextCoastAt = millis() + COASTAL_RETRY_MS;
    }
  }

  // Footer refresh (IP / hostname).
  if (millis() - lastFooterMs >= FOOTER_REFRESH_MS) {
    lastFooterMs = millis();
    drawFooter();
  }

  delay(15);
}
