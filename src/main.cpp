#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>

#include "secrets.h"
#include "web_page.h"

// ============================================================
// CONFIGURATION
// ============================================================

// ---------- GPS ----------
// Le module u-blox NEO-M8N est configuré par tools/gps_config.ps1 :
// UART1 à 38400 bauds, trames NMEA RMC + GGA + ZDA, modèle Portable.
#define GPS_USE_UART1 1

static const uint32_t GPS_BAUD = 38400;
static const int GPS_UART1_RX = 1;
static const int GPS_UART1_TX = 2;
static const int GPS_UART2_RX = 1;
static const int GPS_UART2_TX = 2;

// ---------- TFT ----------
static const int TFT_CS   = 14;
static const int TFT_DC   = 15;
static const int TFT_RST  = 21;
static const int TFT_SCLK = 7;
static const int TFT_MOSI = 6;
static const int TFT_BL   = 22;

// ---------- OLED ----------
#define OLED_SDA    23
#define OLED_SCL    20
#define OLED_ADDR   0x3C
#define OLED_WIDTH  128
#define OLED_HEIGHT 32

// ---------- WiFi ----------
const char* WIFI_SSID = WIFI_SSID_VALUE;
const char* WIFI_PASS = WIFI_PASS_VALUE;

IPAddress LOCAL_IP(192, 168, 0, 32);
IPAddress GATEWAY(192, 168, 0, 1);
IPAddress SUBNET(255, 255, 255, 0);
IPAddress PRIMARY_DNS(8, 8, 8, 8);
IPAddress SECONDARY_DNS(8, 8, 4, 4);

// ---------- Timing ----------
const unsigned long GPS_TIMEOUT_MS      = 3000;
const unsigned long GPS_TIME_MAX_AGE_MS = 2000;
const unsigned long BLINK_INTERVAL_MS   = 500;
const unsigned long REFRESH_INTERVAL_MS = 500;
const unsigned long OLED_SWITCH_MS      = 5000;
const unsigned long WIFI_RETRY_MS       = 10000;

// ============================================================
// COULEURS TFT
// ============================================================

#define C_BG      0x0841
#define C_PANEL   0x10A2
#define C_GRID    0x2124
#define C_TEXT    0xFFFF
#define C_DIM     0xBDF7
#define C_ACCENT  0x07FF
#define C_GREEN   0x07E0
#define C_ORANGE  0xFD20
#define C_RED     0xF800

// ============================================================
// OBJETS
// ============================================================

SPIClass spiTFT(FSPI);
Adafruit_ST7789 tft(&spiTFT, TFT_CS, TFT_DC, TFT_RST);
Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

TinyGPSPlus gps;
HardwareSerial* gpsPort = nullptr;

WebServer server(80);

// ============================================================
// ETAT GLOBAL
// ============================================================

bool gpsError = true;

bool locValid  = false;
bool altValid  = false;
bool spdValid  = false;
bool satsValid = false;

double lat = 0.0;
double lon = 0.0;
double alt = 0.0;
double spd = 0.0;
uint32_t sats = 0;

String fixText = "NO FIX";
uint16_t fixColor = C_RED;

unsigned long lastDataRefresh = 0;
unsigned long lastBlink = 0;
unsigned long lastGpsActivity = 0;
unsigned long lastOledSwitch = 0;
unsigned long lastWifiAttempt = 0;

bool blinkState = false;
bool showUTC = true;

String oldOledText = "";

String oldLat = "";
String oldLon = "";
String oldAlt = "";
String oldSpd = "";
String oldFix = "";
String oldStatus = "";
String oldGpsError = "";
uint32_t oldSats = 999999;
bool oldBlinkState = false;

// ============================================================
// OUTILS
// ============================================================

String fmtCoord(double v, uint8_t decimals = 6) {
  char buf[24];
  dtostrf(v, 0, decimals, buf);
  return String(buf);
}

String fmtAlt(double v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.1f m", v);
  return String(buf);
}

String fmtSpeed(double v) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%.1f km/h", v);
  return String(buf);
}

String safeValue(bool valid, const String& value, const String& fallback = "--") {
  return valid ? value : fallback;
}

// ============================================================
// LOGIQUE GPS
// ============================================================

String computeFixText() {
  if (gpsError) return "NO FIX";
  if (!gps.location.isValid()) return "NO FIX";
  if (gps.altitude.isValid() && gps.satellites.isValid() && gps.satellites.value() >= 4) return "FIX 3D";
  return "FIX 2D";
}

uint16_t computeFixColor(const String& fix) {
  if (fix == "FIX 3D") return C_GREEN;
  if (fix == "FIX 2D") return C_ORANGE;
  return C_RED;
}

String computeStatusText() {
  if (gpsError) return "erreur gps";
  if (gps.charsProcessed() < 10) return "attente...";
  if (!gps.location.isValid()) return "recherche sat...";
  return "OK";
}

// Avant le fix, le u-blox peut sortir l'heure de son RTC interne (souvent
// fausse sans pile de sauvegarde) : on n'affiche l'heure qu'avec un fix.
bool gpsTimeTrusted() {
  return !gpsError &&
         gps.time.isValid() &&
         gps.location.isValid() &&
         gps.time.age() < GPS_TIME_MAX_AGE_MS;
}

// Jour du dernier dimanche d'un mois de 31 jours (mars ou octobre).
int lastSundayOfMonth(int year, int month) {
  int y = year, m = month;
  if (m < 3) { m += 12; y--; }
  // Congruence de Zeller pour le 31 : 0 = samedi, 1 = dimanche, ...
  int h = (31 + 13 * (m + 1) / 5 + y + y / 4 - y / 100 + y / 400) % 7;
  int dow = (h + 6) % 7;  // 0 = dimanche
  return 31 - dow;
}

// Décalage France : UTC+2 du dernier dimanche de mars 01:00 UTC
// au dernier dimanche d'octobre 01:00 UTC, UTC+1 sinon.
int franceUtcOffset() {
  if (!gps.date.isValid()) return 1;

  int y = gps.date.year();
  int mo = gps.date.month();
  int d = gps.date.day();
  int h = gps.time.hour();

  if (mo < 3 || mo > 10) return 1;
  if (mo > 3 && mo < 10) return 2;

  int ls = lastSundayOfMonth(y, mo);
  if (mo == 3) return (d > ls || (d == ls && h >= 1)) ? 2 : 1;
  return (d < ls || (d == ls && h < 1)) ? 2 : 1;
}

String getUtcTimeString() {
  if (!gpsTimeTrusted()) return "--:--:--";

  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
           gps.time.hour(),
           gps.time.minute(),
           gps.time.second());
  return String(buf);
}

String getFranceTimeString() {
  if (!gpsTimeTrusted()) return "--:--:--";

  int hour = (gps.time.hour() + franceUtcOffset()) % 24;

  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
           hour,
           gps.time.minute(),
           gps.time.second());
  return String(buf);
}

void initGpsPort() {
#if GPS_USE_UART1
  gpsPort = &Serial1;
  gpsPort->begin(GPS_BAUD, SERIAL_8N1, GPS_UART1_RX, GPS_UART1_TX);
#else
  gpsPort = &Serial2;
  gpsPort->begin(GPS_BAUD, SERIAL_8N1, GPS_UART2_RX, GPS_UART2_TX);
#endif
}

void readGpsStream() {
  while (gpsPort && gpsPort->available()) {
    char c = gpsPort->read();
    lastGpsActivity = millis();
    gps.encode(c);
  }
}

void refreshGpsData() {
  bool noNmeaEver = (gps.charsProcessed() < 10);
  bool timeoutNmea = (millis() - lastGpsActivity > GPS_TIMEOUT_MS);

  if (noNmeaEver || timeoutNmea) {
    gpsError = true;

    locValid  = false;
    altValid  = false;
    spdValid  = false;
    satsValid = false;

    lat = 0.0;
    lon = 0.0;
    alt = 0.0;
    spd = 0.0;
    sats = 0;

    fixText = "NO FIX";
    fixColor = C_RED;
    return;
  }

  gpsError = false;

  locValid  = gps.location.isValid();
  altValid  = gps.altitude.isValid();
  spdValid  = gps.speed.isValid();
  satsValid = gps.satellites.isValid();

  if (locValid) {
    lat = gps.location.lat();
    lon = gps.location.lng();
  }

  if (altValid) alt = gps.altitude.meters();
  if (spdValid) spd = gps.speed.kmph();
  if (satsValid) sats = gps.satellites.value();

  fixText = computeFixText();
  fixColor = computeFixColor(fixText);
}

// ============================================================
// TFT HELPERS
// ============================================================

void clearValueBox(int x, int y, int w, int h) {
  tft.fillRect(x, y, w, h, C_PANEL);
}

// ============================================================
// OLED
// ============================================================

void updateOledTime() {
  if (millis() - lastOledSwitch >= OLED_SWITCH_MS) {
    lastOledSwitch = millis();
    showUTC = !showUTC;
  }

  String line1 = showUTC ? "UTC" : "FR";
  String line2 = showUTC ? getUtcTimeString() : getFranceTimeString();

  String currentText = line1 + "|" + line2;
  if (currentText == oldOledText) return;
  oldOledText = currentText;

  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  oled.setTextSize(1);
  int16_t x1, y1;
  uint16_t w1, h1;
  oled.getTextBounds(line1.c_str(), 0, 0, &x1, &y1, &w1, &h1);
  int xLine1 = (OLED_WIDTH - w1) / 2;
  if (xLine1 < 0) xLine1 = 0;
  oled.setCursor(xLine1, 2);
  oled.print(line1);

  oled.setTextSize(2);
  int16_t x2, y2;
  uint16_t w2, h2;
  oled.getTextBounds(line2.c_str(), 0, 0, &x2, &y2, &w2, &h2);
  int xLine2 = (OLED_WIDTH - w2) / 2;
  if (xLine2 < 0) xLine2 = 0;
  oled.setCursor(xLine2, 16);
  oled.print(line2);

  oled.display();
}

// ============================================================
// TFT UI
// ============================================================

void drawStaticUI() {
  tft.fillScreen(C_BG);

  for (int y = 0; y < 240; y += 20) tft.drawFastHLine(0, y, 240, C_GRID);
  for (int x = 0; x < 240; x += 20) tft.drawFastVLine(x, 0, 240, C_GRID);

  tft.fillCircle(210, 28, 26, C_PANEL);
  tft.fillCircle(228, 12, 12, C_GRID);

  tft.setTextWrap(false);

  tft.setTextColor(C_ACCENT);
  tft.setTextSize(2);
  tft.setCursor(12, 10);
  tft.print("-={GPS}=-");

  tft.fillRoundRect(10, 44, 220, 182, 12, C_PANEL);
  tft.drawRoundRect(10, 44, 220, 182, 12, C_GRID);

  tft.drawRoundRect(132, 8, 96, 28, 8, C_GRID);
  tft.fillRoundRect(132, 8, 96, 28, 8, C_PANEL);

  tft.setTextColor(C_DIM);
  tft.setTextSize(1);
  tft.setCursor(22, 58);  tft.print("LAT");
  tft.setCursor(22, 104); tft.print("LONG");
  tft.setCursor(22, 150); tft.print("ALTITUDE");
  tft.setCursor(22, 194); tft.print("VITESSE");
  tft.setCursor(182, 58); tft.print("SAT");
}

void updateFix() {
  if (fixText != oldFix || blinkState != oldBlinkState) {
    tft.fillRoundRect(133, 9, 94, 26, 8, C_PANEL);

    if (blinkState && fixText != "NO FIX") {
      tft.fillCircle(143, 22, 4, fixColor);
    } else {
      tft.drawCircle(143, 22, 4, fixColor);
    }

    tft.setTextColor(fixColor);
    tft.setTextSize(1);
    tft.setCursor(154, 18);
    tft.print(fixText);

    oldFix = fixText;
    oldBlinkState = blinkState;
  }
}

void updateLat() {
  String s = gpsError ? "error" : safeValue(locValid, fmtCoord(lat));
  if (s != oldLat) {
    clearValueBox(22, 74, 120, 16);
    tft.setTextColor(gpsError ? C_RED : C_TEXT);
    tft.setTextSize(2);
    tft.setCursor(22, 74);
    tft.print(s);
    oldLat = s;
  }
}

void updateLon() {
  String s = gpsError ? "error" : safeValue(locValid, fmtCoord(lon));
  if (s != oldLon) {
    clearValueBox(22, 120, 120, 16);
    tft.setTextColor(gpsError ? C_RED : C_TEXT);
    tft.setTextSize(2);
    tft.setCursor(22, 120);
    tft.print(s);
    oldLon = s;
  }
}

void updateAlt() {
  String s = gpsError ? "error" : safeValue(altValid, fmtAlt(alt), "-- m");
  if (s != oldAlt) {
    clearValueBox(22, 166, 120, 16);
    tft.setTextColor(gpsError ? C_RED : C_TEXT);
    tft.setTextSize(2);
    tft.setCursor(22, 166);
    tft.print(s);
    oldAlt = s;
  }
}

void updateSpd() {
  String s = gpsError ? "error" : safeValue(spdValid, fmtSpeed(spd), "-- km/h");
  if (s != oldSpd) {
    clearValueBox(22, 210, 120, 16);
    tft.setTextColor(gpsError ? C_RED : C_ACCENT);
    tft.setTextSize(2);
    tft.setCursor(22, 210);
    tft.print(s);
    oldSpd = s;
  }
}

void updateSats() {
  uint32_t displaySats = (gpsError || !satsValid) ? 999999 : sats;

  if (displaySats != oldSats) {
    clearValueBox(160, 82, 60, 28);

    tft.setTextColor((!gpsError && satsValid) ? C_GREEN : C_DIM);
    tft.setTextSize(3);
    tft.setCursor(176, 82);

    if (!gpsError && satsValid) tft.print((uint32_t)sats);
    else tft.print("--");

    tft.fillRect(148, 140, 70, 45, C_PANEL);

    int bars = 0;
    if (!gpsError && satsValid) {
      bars = (int)(sats / 2);
      if (bars > 5) bars = 5;
    }

    for (int i = 0; i < 5; i++) {
      int bh = 8 + i * 6;
      uint16_t color = (i < bars) ? C_GREEN : C_GRID;
      tft.fillRoundRect(150 + i * 12, 180 - bh, 8, bh, 2, color);
    }

    oldSats = displaySats;
  }
}

void updateGpsError() {
  String s = gpsError ? "error" : "";
  if (s != oldGpsError) {
    clearValueBox(22, 184, 120, 10);

    if (gpsError) {
      tft.setTextColor(C_RED);
      tft.setTextSize(1);
      tft.setCursor(22, 184);
      tft.print("error");
    }

    oldGpsError = s;
  }
}

void updateFooter() {
#if GPS_USE_UART1
  String s = "GPS: UART1 - " + computeStatusText();
#else
  String s = "GPS: UART2 - " + computeStatusText();
#endif

  if (WiFi.status() == WL_CONNECTED) s += " - WIFI OK";
  else s += " - WIFI OFF";

  if (s != oldStatus) {
    tft.fillRect(0, 228, 240, 12, C_BG);
    tft.setTextColor(C_DIM);
    tft.setTextSize(1);
    tft.setCursor(4, 231);
    tft.print(s);
    oldStatus = s;
  }
}

void refreshDisplay() {
  updateFix();
  updateLat();
  updateLon();
  updateAlt();
  updateGpsError();
  updateSpd();
  updateSats();
  updateFooter();
}

// ============================================================
// WIFI
// ============================================================

void connectWifi() {
  Serial.println();
  Serial.println("Configuration WiFi...");

  WiFi.mode(WIFI_STA);

  if (!WiFi.config(LOCAL_IP, GATEWAY, SUBNET, PRIMARY_DNS, SECONDARY_DNS)) {
    Serial.println("Echec WiFi.config()");
  }

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connexion a ");
  Serial.println(WIFI_SSID);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_RETRY_MS) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi non connecte");
  }
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  if (millis() - lastWifiAttempt >= WIFI_RETRY_MS) {
    lastWifiAttempt = millis();
    Serial.println("Reconnexion WiFi...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ============================================================
// WEB / JSON
// ============================================================

String buildJson() {
  String json;
  json.reserve(1200);

  json += "{";
  json += "\"wifi\":{";
  json += "\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ssid\":\"" + String(WIFI_SSID) + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += "},";

  json += "\"gps\":{";
  json += "\"error\":" + String(gpsError ? "true" : "false") + ",";
  json += "\"status\":\"" + computeStatusText() + "\",";
  json += "\"fix\":\"" + fixText + "\",";
  json += "\"locationValid\":" + String(locValid ? "true" : "false") + ",";
  json += "\"altitudeValid\":" + String(altValid ? "true" : "false") + ",";
  json += "\"speedValid\":" + String(spdValid ? "true" : "false") + ",";
  json += "\"satellitesValid\":" + String(satsValid ? "true" : "false") + ",";
  json += "\"latitude\":\"" + safeValue(locValid, fmtCoord(lat)) + "\",";
  json += "\"longitude\":\"" + safeValue(locValid, fmtCoord(lon)) + "\",";
  json += "\"altitude\":\"" + safeValue(altValid, fmtAlt(alt), "-- m") + "\",";
  json += "\"speed\":\"" + safeValue(spdValid, fmtSpeed(spd), "-- km/h") + "\",";
  json += "\"satellites\":" + String(satsValid ? sats : 0) + ",";
  json += "\"utc\":\"" + getUtcTimeString() + "\",";
  json += "\"france\":\"" + getFranceTimeString() + "\",";
  json += "\"charsProcessed\":" + String(gps.charsProcessed());
  json += "}";

#if GPS_USE_UART1
  json += ",\"port\":\"UART1\"";
#else
  json += ",\"port\":\"UART2\"";
#endif

  json += "}";

  return json;
}

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

void handleJson() {
  server.send(200, "application/json; charset=utf-8", buildJson());
}

void handleNotFound() {
  server.send(404, "text/plain; charset=utf-8", "404 Not Found");
}

void initWebServer() {
  server.on("/", handleRoot);
  server.on("/json", handleJson);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.println("Serveur web demarre");
  Serial.print("HTML : http://");
  Serial.print(LOCAL_IP);
  Serial.println("/");
  Serial.print("JSON : http://");
  Serial.print(LOCAL_IP);
  Serial.println("/json");
}

// ============================================================
// INIT MATERIEL
// ============================================================

void initOled() {
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("Echec OLED !");
    return;
  }

  oled.clearDisplay();
  oled.display();
}

void initTft() {
  if (TFT_BL >= 0) {
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
  }

  spiTFT.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
  tft.init(240, 240);
  tft.setRotation(0);
}

// ============================================================
// SETUP / LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  initOled();
  initTft();
  initGpsPort();

  lastGpsActivity = millis();
  lastWifiAttempt = millis();

  connectWifi();
  initWebServer();

  drawStaticUI();
  refreshGpsData();
  refreshDisplay();
  updateOledTime();
}

void loop() {
  readGpsStream();
  server.handleClient();
  maintainWifi();

  if (millis() - lastBlink >= BLINK_INTERVAL_MS) {
    lastBlink = millis();
    blinkState = !blinkState;
    updateFix();
  }

  if (millis() - lastDataRefresh >= REFRESH_INTERVAL_MS) {
    lastDataRefresh = millis();
    refreshGpsData();
    refreshDisplay();
  }

  updateOledTime();
}
