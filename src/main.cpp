#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPSPlus.h>
#include <Preferences.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#include "gps_clock.h"
#include "metar.h"
#include "ntp_server.h"
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
// Broche TIMEPULSE (PPS) du GPS pour un NTP précis à quelques µs ; -1 si non câblée
static const int GPS_PPS_PIN = -1;

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
// Valeurs par défaut, remplacées par celles enregistrées en NVS depuis /config.
struct NetConfig {
  String ssid = WIFI_SSID_VALUE;
  String pass = WIFI_PASS_VALUE;
  bool dhcp = false;
  IPAddress ip      {192, 168, 0, 32};
  IPAddress gateway {192, 168, 0, 1};
  IPAddress subnet  {255, 255, 255, 0};
  IPAddress dns1    {8, 8, 8, 8};
  IPAddress dns2    {8, 8, 4, 4};
};

NetConfig net;
Preferences prefs;

// Point d'accès de secours si le WiFi configuré est injoignable
const char* SETUP_AP_SSID = "ESP32-GPS-Setup";
const char* SETUP_AP_PASS = "gpsconfig";
bool setupApActive = false;

// ---------- Timing ----------
const unsigned long GPS_TIMEOUT_MS      = 3000;
const unsigned long GPS_TIME_MAX_AGE_MS = 2000;
const unsigned long BLINK_INTERVAL_MS   = 500;
const unsigned long REFRESH_INTERVAL_MS = 500;
const unsigned long OLED_SWITCH_MS      = 5000;
const unsigned long WIFI_RETRY_MS       = 10000;
const unsigned long TFT_PAGE_MS         = 10000;
const int TFT_PAGE_COUNT                = 3;

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

// Page 2 dessinée hors écran puis envoyée d'un bloc : pas de scintillement.
GFXcanvas16* tftCanvas = nullptr;

TinyGPSPlus gps;
TinyGPSCustom gpsQuality(gps, "GNGGA", 6);
HardwareSerial* gpsPort = nullptr;

// Dernières trames brutes, pour l'affichage "paquet GPS" de la page 2
char nmeaLine[100];
size_t nmeaLen = 0;
char lastRmc[100] = "";
char lastGga[100] = "";

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
unsigned long lastGpsActivity = 0;
unsigned long lastOledSwitch = 0;
unsigned long lastWifiAttempt = 0;

unsigned long lastPageSwitch = 0;

bool blinkState = false;
bool showUTC = true;
uint8_t tftPage = 0;

String oldOledText = "";

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

String windDirText(int deg) {
  if (deg < 0) return "VRB";
  char buf[4];
  snprintf(buf, sizeof(buf), "%03d", deg);
  return String(buf);
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

// Garde la dernière RMC et la dernière GGA, sans le checksum
void captureNmea(char c) {
  if (c == '$') nmeaLen = 0;
  if (c == '\r' || c == '\n') {
    if (nmeaLen > 6 && nmeaLine[0] == '$') {
      nmeaLine[nmeaLen] = '\0';
      char* star = strchr(nmeaLine, '*');
      if (star) *star = '\0';
      if (strncmp(nmeaLine + 3, "RMC", 3) == 0) strlcpy(lastRmc, nmeaLine, sizeof(lastRmc));
      else if (strncmp(nmeaLine + 3, "GGA", 3) == 0) strlcpy(lastGga, nmeaLine, sizeof(lastGga));
    }
    nmeaLen = 0;
    return;
  }
  if (nmeaLen < sizeof(nmeaLine) - 1) nmeaLine[nmeaLen++] = c;
}

void readGpsStream() {
  while (gpsPort && gpsPort->available()) {
    char c = gpsPort->read();
    lastGpsActivity = millis();
    gps.encode(c);
    gpsClockOnChar(c);
    captureNmea(c);
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
// TFT PAGE 2 : RESEAU + PAQUET GPS
// ============================================================

#define C2_BG      0x0862
#define C2_CARD    0x18E5
#define C2_EDGE    0x2967
#define C2_LABEL   0x8C51
#define C2_VALUE   0xF7BE
#define C2_RAW     0x6E7F

// Texte en police lissée, positionné par le haut (les FreeFonts partent de la ligne de base)
void drawSmooth(GFXcanvas16& g, const GFXfont* font, int x, int top, uint16_t color, const String& s) {
  g.setFont(font);
  g.setTextColor(color);
  int16_t bx, by;
  uint16_t bw, bh;
  g.getTextBounds("Ay", 0, 0, &bx, &by, &bw, &bh);
  g.setCursor(x, top - by);
  g.print(s);
}

void drawLabel(GFXcanvas16& g, int x, int y, const char* s) {
  g.setFont(nullptr);
  g.setTextSize(1);
  g.setTextColor(C2_LABEL);
  g.setCursor(x, y);
  g.print(s);
}

void drawCard(GFXcanvas16& g, int x, int y, int w, int h, const char* title, uint16_t accent) {
  g.fillRoundRect(x, y, w, h, 10, C2_CARD);
  g.drawRoundRect(x, y, w, h, 10, C2_EDGE);
  g.fillRoundRect(x + 12, y + 9, 3, 9, 1, accent);
  drawLabel(g, x + 20, y + 10, title);
}

void drawPageDots(GFXcanvas16& g) {
  for (int i = 0; i < TFT_PAGE_COUNT; i++) {
    g.fillCircle(120 + (i * 2 - (TFT_PAGE_COUNT - 1)) * 6, 236, 2, i == tftPage ? C_ACCENT : C2_EDGE);
  }
}

void drawSignalBars(GFXcanvas16& g, int x, int bottom, int rssi, bool connected) {
  int bars = 0;
  if (connected) {
    if (rssi > -55) bars = 4;
    else if (rssi > -65) bars = 3;
    else if (rssi > -75) bars = 2;
    else bars = 1;
  }
  for (int i = 0; i < 4; i++) {
    int h = 5 + i * 4;
    g.fillRoundRect(x + i * 7, bottom - h, 5, h, 1, i < bars ? C_GREEN : C2_EDGE);
  }
}

void drawPage2() {
  if (!tftCanvas) return;
  GFXcanvas16& g = *tftCanvas;
  g.setTextWrap(false);
  g.fillScreen(C2_BG);

  bool wifiOk = WiFi.status() == WL_CONNECTED;

  // En-tête
  drawSmooth(g, &FreeSansBold12pt7b, 14, 12, C2_VALUE, "Reseau & GPS");
  g.fillRoundRect(186, 12, 42, 18, 9, wifiOk ? C_GREEN : (setupApActive ? C_ORANGE : C_RED));
  g.setFont(nullptr);
  g.setTextColor(C2_BG);
  g.setCursor(wifiOk ? 196 : (setupApActive ? 201 : 196), 17);
  g.print(wifiOk ? "WIFI" : (setupApActive ? "AP" : "HORS"));

  // Carte WiFi
  drawCard(g, 10, 42, 220, 84, "WIFI", C_ACCENT);
  drawSignalBars(g, 194, 60, WiFi.RSSI(), wifiOk);
  drawSmooth(g, &FreeSansBold9pt7b, 22, 28 + 42 - 4, C2_VALUE,
             setupApActive && !wifiOk ? String(SETUP_AP_SSID) : net.ssid);
  drawLabel(g, 22, 90, "IP");
  String ipTxt = wifiOk ? WiFi.localIP().toString()
                        : (setupApActive ? WiFi.softAPIP().toString() : String("--"));
  drawSmooth(g, &FreeSansBold12pt7b, 40, 84, C_ACCENT, ipTxt);
  drawLabel(g, 22, 112, setupApActive && !wifiOk ? "AP" : "GW");
  g.setTextColor(C2_VALUE);
  g.setCursor(40, 112);
  g.print(setupApActive && !wifiOk ? String("mdp ") + SETUP_AP_PASS
                                   : (wifiOk ? WiFi.gatewayIP() : net.gateway).toString());
  if (wifiOk) {
    g.setCursor(150, 112);
    g.print(String(WiFi.RSSI()) + " dBm");
  }

  // Carte paquet GPS
  uint16_t gpsAccent = gpsError ? C_RED : (locValid ? C_GREEN : C_ORANGE);
  drawCard(g, 10, 134, 220, 90, "PAQUET GPS", gpsAccent);

  g.setFont(nullptr);
  g.setTextColor(C2_RAW);
  g.setCursor(22, 150);
  g.print(String(lastRmc[0] ? lastRmc : "$--RMC (rien)").substring(0, 33));
  g.setCursor(22, 161);
  g.print(String(lastGga[0] ? lastGga : "$--GGA (rien)").substring(0, 33));

  // Qualité GGA : 0 aucun fix, 1 GPS, 2 différentiel (EGNOS)
  String qualTxt = "--";
  if (!gpsError && gpsQuality.isValid()) {
    int q = atoi(gpsQuality.value());
    qualTxt = q == 0 ? "AUCUN" : (q == 2 ? "DGPS" : (q == 1 ? "GPS" : String(q)));
  }
  String hdopTxt = (!gpsError && gps.hdop.isValid() && locValid) ? String(gps.hdop.hdop(), 1) : String("--");
  String ageTxt = (!gpsError && gps.location.isValid())
                      ? String(gps.location.age() / 1000.0, 1) + "s"
                      : String("--");

  drawLabel(g, 22, 176, "QUALITE");
  drawLabel(g, 104, 176, "HDOP");
  drawLabel(g, 166, 176, "AGE");
  drawSmooth(g, &FreeSansBold9pt7b, 22, 187, C2_VALUE, qualTxt);
  drawSmooth(g, &FreeSansBold9pt7b, 104, 187, C2_VALUE, hdopTxt);
  drawSmooth(g, &FreeSansBold9pt7b, 166, 187, C2_VALUE, ageTxt);

  g.setFont(nullptr);
  g.setTextColor(C2_LABEL);
  g.setCursor(22, 210);
  g.print("OK " + String(gps.passedChecksum()) + "  ERR " + String(gps.failedChecksum()));
  g.setTextColor(gpsAccent);
  g.setCursor(148, 210);
  g.print(gpsError ? "PAS DE DONNEES" : (locValid ? "FIX" : "RECHERCHE"));

  drawPageDots(g);

  tft.drawRGBBitmap(0, 0, g.getBuffer(), 240, 240);
}

// ============================================================
// TFT PAGE 1 : POSITION
// ============================================================

void drawTile(GFXcanvas16& g, int x, int y, int w, int h, const char* label, const String& value,
              const char* unit, uint16_t valueColor) {
  g.fillRoundRect(x, y, w, h, 8, C2_CARD);
  g.drawRoundRect(x, y, w, h, 8, C2_EDGE);
  drawLabel(g, x + 8, y + 8, label);
  drawSmooth(g, &FreeSansBold12pt7b, x + 8, y + 22, valueColor, value);
  if (unit) drawLabel(g, x + 8, y + h - 12, unit);
}

void drawCoordRow(GFXcanvas16& g, int top, double v, char pos, char neg) {
  String txt = gpsError ? String("--") : (locValid ? fmtCoord(fabs(v)) : String("--.------"));
  drawSmooth(g, &FreeSansBold18pt7b, 22, top, locValid && !gpsError ? C2_VALUE : C2_LABEL, txt);
  if (locValid && !gpsError) {
    drawSmooth(g, &FreeSansBold12pt7b, 206, top + 4, C_ACCENT, String(v >= 0 ? pos : neg));
  }
}

void drawPage1() {
  if (!tftCanvas) return;
  GFXcanvas16& g = *tftCanvas;
  g.setTextWrap(false);
  g.fillScreen(C2_BG);

  // En-tête + pastille de fix
  drawSmooth(g, &FreeSansBold12pt7b, 14, 12, C2_VALUE, "Position");
  g.fillRoundRect(138, 9, 90, 24, 12, C2_CARD);
  g.drawRoundRect(138, 9, 90, 24, 12, fixColor);
  if (blinkState && fixText != "NO FIX") g.fillCircle(152, 21, 4, fixColor);
  else g.drawCircle(152, 21, 4, fixColor);
  drawSmooth(g, &FreeSansBold9pt7b, 162, 14, fixColor, fixText);

  // Coordonnées en grand
  drawCard(g, 10, 42, 220, 100, "COORDONNEES", C_ACCENT);
  drawCoordRow(g, 66, lat, 'N', 'S');
  drawCoordRow(g, 104, lon, 'E', 'O');

  // Altitude, vitesse, satellites
  bool ok = !gpsError;
  String altTxt = ok && altValid ? String((long)lround(alt)) : String("--");
  String spdTxt = ok && spdValid ? String(spd, spd < 100 ? 1 : 0) : String("--");
  String satTxt = ok && satsValid ? String(sats) : String("--");
  drawTile(g, 10, 150, 70, 58, "ALTITUDE", altTxt, "m", C2_VALUE);
  drawTile(g, 85, 150, 70, 58, "VITESSE", spdTxt, "km/h", C_ACCENT);
  drawTile(g, 160, 150, 70, 58, "SAT", satTxt, nullptr,
           ok && satsValid && sats >= 4 ? C_GREEN : C2_VALUE);
  int bars = ok && satsValid ? min<int>(sats / 2, 5) : 0;
  for (int i = 0; i < 5; i++) {
    int bh = 3 + i * 2;
    g.fillRect(168 + i * 6, 202 - bh, 4, bh, i < bars ? C_GREEN : C2_EDGE);
  }

  // Pied : serveur NTP et réseau
  bool synced = gpsClockSynced();
  g.fillRoundRect(10, 214, 220, 18, 9, C2_CARD);
  g.fillCircle(21, 223, 3, synced ? C_GREEN : C_ORANGE);
  g.setFont(nullptr);
  g.setTextColor(C2_VALUE);
  g.setCursor(30, 219);
  g.print(String("NTP ") + (synced ? (gpsClockUsesPps() ? "PPS" : "GPS") : "ATTENTE") +
          "  " + String(ntpRequestCount()) + " req");
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  g.setTextColor(wifiOk ? C2_LABEL : (setupApActive ? C_ORANGE : C_RED));
  String netTxt = wifiOk ? WiFi.localIP().toString() : String(setupApActive ? "AP" : "WIFI HORS");
  g.setCursor(222 - netTxt.length() * 6, 219);
  g.print(netTxt);

  drawPageDots(g);
  tft.drawRGBBitmap(0, 0, g.getBuffer(), 240, 240);
}

// ============================================================
// TFT PAGE 3 : METAR
// ============================================================

uint16_t flightRulesColor(const String& r) {
  if (r == "VFR") return C_GREEN;
  if (r == "MVFR") return 0x5D9F;   // bleu
  if (r == "IFR") return C_RED;
  if (r == "LIFR") return 0xF81F;   // magenta
  return C2_LABEL;
}

// Rose : la flèche indique où va le vent (il vient de la direction opposée)
void drawWindRose(GFXcanvas16& g, int cx, int cy, int r, int dirDeg) {
  g.drawCircle(cx, cy, r, C2_EDGE);
  for (int i = 0; i < 4; i++) {
    float a = i * HALF_PI;
    g.drawLine(cx + (r - 4) * sin(a), cy - (r - 4) * cos(a), cx + r * sin(a), cy - r * cos(a), C2_LABEL);
  }
  g.setFont(nullptr);
  g.setTextColor(C2_LABEL);
  g.setCursor(cx - 2, cy - r - 9);
  g.print("N");
  if (dirDeg < 0) {
    g.fillCircle(cx, cy, 3, C_ACCENT);
    return;
  }
  float a = radians(dirDeg + 180);
  int tx = cx + (r - 3) * sin(a), ty = cy - (r - 3) * cos(a);       // pointe
  int bx = cx - (r - 6) * sin(a), by = cy + (r - 6) * cos(a);       // queue
  float pa = a + PI;
  int lx = tx + 8 * sin(pa - 0.45), ly = ty - 8 * cos(pa - 0.45);
  int rx = tx + 8 * sin(pa + 0.45), ry = ty - 8 * cos(pa + 0.45);
  g.drawLine(bx, by, tx, ty, C_ACCENT);
  g.drawLine(bx + 1, by, tx + 1, ty, C_ACCENT);
  g.fillTriangle(tx, ty, lx, ly, rx, ry, C_ACCENT);
}

// Texte 6x8 sur plusieurs lignes, coupé aux espaces
int drawWrapped(GFXcanvas16& g, int x, int y, int maxChars, int maxLines, uint16_t color, const String& s) {
  g.setFont(nullptr);
  g.setTextColor(color);
  int line = 0;
  int pos = 0;
  while (pos < (int)s.length() && line < maxLines) {
    int end = min<int>(pos + maxChars, s.length());
    if (end < (int)s.length()) {
      int sp = s.lastIndexOf(' ', end);
      if (sp > pos) end = sp;
    }
    g.setCursor(x, y + line * 10);
    g.print(s.substring(pos, end));
    pos = end;
    while (pos < (int)s.length() && s[pos] == ' ') pos++;
    line++;
  }
  return line;
}

void drawDegrees(GFXcanvas16& g, const GFXfont* font, int x, int top, uint16_t color, const String& value) {
  drawSmooth(g, font, x, top, color, value);
  int16_t bx, by;
  uint16_t bw, bh;
  g.getTextBounds(value, 0, 0, &bx, &by, &bw, &bh);
  g.drawCircle(x + bw + 5, top + 3, 2, color);
}

void drawPage3() {
  if (!tftCanvas) return;
  GFXcanvas16& g = *tftCanvas;
  g.setTextWrap(false);
  g.fillScreen(C2_BG);

  Metar m = metarSnapshot();

  drawSmooth(g, &FreeSansBold12pt7b, 14, 12, C2_VALUE, "METAR " + (m.station.length() ? m.station : String(METAR_STATION_VALUE)));
  if (m.valid && m.flightRules.length()) {
    uint16_t c = flightRulesColor(m.flightRules);
    g.fillRoundRect(178, 10, 50, 22, 11, c);
    drawSmooth(g, &FreeSansBold9pt7b, 203 - m.flightRules.length() * 5, 14, C2_BG, m.flightRules);
  }

  if (!m.valid) {
    drawCard(g, 10, 52, 220, 80, "METEO", C_ORANGE);
    drawSmooth(g, &FreeSansBold9pt7b, 22, 74, C2_VALUE,
               WiFi.status() == WL_CONNECTED ? "Chargement..." : "Pas de WiFi");
    if (m.error.length()) drawWrapped(g, 22, 100, 33, 2, C_RED, m.error);
    drawPageDots(g);
    tft.drawRGBBitmap(0, 0, g.getBuffer(), 240, 240);
    return;
  }

  // Heure d'observation
  String obs = m.timeRepr.length() >= 6
                   ? "Observe le " + m.timeRepr.substring(0, 2) + " a " + m.timeRepr.substring(2, 4) + "h" +
                         m.timeRepr.substring(4, 6) + " UTC"
                   : String("");
  uint32_t ageMin = (millis() - m.fetchedMs) / 60000;
  drawLabel(g, 14, 38, (obs + "  (maj " + String(ageMin) + " min)").c_str());

  // Vent
  drawCard(g, 10, 50, 220, 62, "VENT", C_ACCENT);
  drawWindRose(g, 196, 84, 18, m.windDir);
  String wind;
  if (m.windSpeed == 0) wind = "Calme";
  else wind = windDirText(m.windDir) + "  " + String(m.windSpeed) + " " + m.windUnit;
  drawSmooth(g, &FreeSansBold12pt7b, 22, 68, C2_VALUE, wind);
  String windSub = m.windDir >= 0 ? String("du ") + metarCompass(m.windDir) : String("direction variable");
  if (m.windGust > 0) windSub += ", rafales " + String(m.windGust) + " " + m.windUnit;
  if (m.windVarFrom >= 0) windSub += ", var " + String(m.windVarFrom) + "-" + String(m.windVarTo);
  drawWrapped(g, 22, 96, 26, 1, C2_LABEL, windSub);

  // Température, humidité, pression
  bool hasT = m.temperature != INT16_MIN;
  g.fillRoundRect(10, 118, 70, 46, 8, C2_CARD);
  g.drawRoundRect(10, 118, 70, 46, 8, C2_EDGE);
  drawLabel(g, 18, 125, "TEMP");
  if (hasT) drawDegrees(g, &FreeSansBold12pt7b, 18, 137, C2_VALUE, String(m.temperature));
  else drawSmooth(g, &FreeSansBold12pt7b, 18, 137, C2_VALUE, "--");
  if (m.dewpoint != INT16_MIN) drawLabel(g, 50, 125, ("r" + String(m.dewpoint)).c_str());

  drawTile(g, 85, 118, 70, 46, "HUMIDITE", m.humidity >= 0 ? String(m.humidity) + "%" : String("--"),
           nullptr, C_ACCENT);
  drawTile(g, 160, 118, 70, 46, "QNH", m.altimeter >= 0 ? String(m.altimeter) : String("--"),
           nullptr, C2_VALUE);

  // Ciel, visibilité, phénomènes, tendance
  drawCard(g, 10, 170, 220, 62, "CIEL", flightRulesColor(m.flightRules));
  String sky;
  if (m.visibilityRepr == "CAVOK") sky = "CAVOK";
  else if (m.visibility >= 9999) sky = "Visi 10 km+";
  else if (m.visibility >= 0) sky = "Visi " + (m.visibility >= 5000 ? String(m.visibility / 1000) + " km"
                                                                 : String(m.visibility) + " m");
  drawSmooth(g, &FreeSansBold9pt7b, 64, 177, C2_VALUE, sky);

  String detail;
  for (int i = 0; i < m.cloudCount; i++) detail += (detail.length() ? ", " : "") + metarCloudText(m.clouds[i]);
  if (m.weather.length()) detail += (detail.length() ? ", " : "") + m.weather;
  if (detail.isEmpty() && m.visibilityRepr != "CAVOK") detail = "pas de nuages significatifs";

  int y = 196;
  if (detail.length()) y += 10 * drawWrapped(g, 22, y, 33, 1, C2_VALUE, detail);
  if (m.trend.length()) drawWrapped(g, 22, y, 33, (226 - y) / 10, C_ORANGE, m.trend);

  drawPageDots(g);
  tft.drawRGBBitmap(0, 0, g.getBuffer(), 240, 240);
}

void drawCurrentPage() {
  if (tftPage == 0) drawPage1();
  else if (tftPage == 1) drawPage2();
  else drawPage3();
}

// ============================================================
// WIFI
// ============================================================

IPAddress readIp(const char* key, const IPAddress& fallback) {
  IPAddress ip;
  return ip.fromString(prefs.getString(key, "")) ? ip : fallback;
}

void loadNetConfig() {
  prefs.begin("net", true);
  if (prefs.isKey("ssid")) {
    net.ssid    = prefs.getString("ssid", net.ssid);
    net.pass    = prefs.getString("pass", net.pass);
    net.dhcp    = prefs.getBool("dhcp", net.dhcp);
    net.ip      = readIp("ip", net.ip);
    net.gateway = readIp("gw", net.gateway);
    net.subnet  = readIp("mask", net.subnet);
    net.dns1    = readIp("dns1", net.dns1);
    net.dns2    = readIp("dns2", net.dns2);
    Serial.println("Config reseau chargee depuis la NVS");
  }
  prefs.end();
}

void saveNetConfig() {
  prefs.begin("net", false);
  prefs.putString("ssid", net.ssid);
  prefs.putString("pass", net.pass);
  prefs.putBool("dhcp", net.dhcp);
  prefs.putString("ip", net.ip.toString());
  prefs.putString("gw", net.gateway.toString());
  prefs.putString("mask", net.subnet.toString());
  prefs.putString("dns1", net.dns1.toString());
  prefs.putString("dns2", net.dns2.toString());
  prefs.end();
}

void beginStation() {
  // Sans veille modem : sinon les réponses NTP attendent le prochain beacon (jusqu'à ~300 ms)
  WiFi.setSleep(false);
  if (net.dhcp) {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  } else if (!WiFi.config(net.ip, net.gateway, net.subnet, net.dns1, net.dns2)) {
    Serial.println("Echec WiFi.config()");
  }
  WiFi.begin(net.ssid.c_str(), net.pass.c_str());
}

void startSetupAp() {
  if (setupApActive) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASS);
  setupApActive = true;
  Serial.print("Point d'acces de secours ");
  Serial.print(SETUP_AP_SSID);
  Serial.print(" : http://");
  Serial.println(WiFi.softAPIP());
}

void stopSetupAp() {
  if (!setupApActive) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  setupApActive = false;
  Serial.println("Point d'acces de secours arrete");
}

void connectWifi() {
  Serial.println();
  Serial.println("Configuration WiFi...");

  WiFi.mode(WIFI_STA);
  beginStation();

  Serial.print("Connexion a ");
  Serial.println(net.ssid);

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
    startSetupAp();
  }
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    stopSetupAp();
    return;
  }

  if (millis() - lastWifiAttempt >= WIFI_RETRY_MS) {
    lastWifiAttempt = millis();
    Serial.println("Reconnexion WiFi...");
    startSetupAp();
    // Un client connecté à l'AP : on évite le balayage de canaux qui le décrocherait
    if (WiFi.softAPgetStationNum() > 0) return;
    WiFi.disconnect();
    beginStation();
  }
}

// ============================================================
// WEB / JSON
// ============================================================

unsigned long restartAt = 0;

String jsonEscape(const String& s) {
  String out;
  for (char c : s) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c >= 0x20) out += c;
  }
  return out;
}

String htmlEscape(const String& s) {
  String out;
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

String buildConfigPage(const String& message) {
  String html = FPSTR(CONFIG_HTML);
  html.replace("%MESSAGE%", message);
  html.replace("%SSID%", htmlEscape(net.ssid));
  html.replace("%DHCP%", net.dhcp ? "checked" : "");
  html.replace("%IP%", net.ip.toString());
  html.replace("%MASK%", net.subnet.toString());
  html.replace("%GW%", net.gateway.toString());
  html.replace("%DNS1%", net.dns1.toString());
  html.replace("%DNS2%", net.dns2.toString());
  return html;
}

void handleConfigGet() {
  server.send(200, "text/html; charset=utf-8", buildConfigPage(""));
}

void handleConfigPost() {
  NetConfig next = net;
  String err;

  next.ssid = server.arg("ssid");
  next.ssid.trim();
  if (server.arg("pass").length()) next.pass = server.arg("pass");
  next.dhcp = server.hasArg("dhcp");

  if (next.ssid.isEmpty() || next.ssid.length() > 32) err = "SSID vide ou trop long.";
  else if (next.pass.length() && (next.pass.length() < 8 || next.pass.length() > 63))
    err = "Le mot de passe WPA doit faire 8 à 63 caractères.";

  if (err.isEmpty() && !next.dhcp) {
    struct { const char* field; const char* name; IPAddress* dst; } fields[] = {
      {"ip", "Adresse IP", &next.ip},     {"mask", "Masque", &next.subnet},
      {"gw", "Passerelle", &next.gateway}, {"dns1", "DNS principal", &next.dns1},
      {"dns2", "DNS secondaire", &next.dns2},
    };
    for (auto& f : fields) {
      if (!f.dst->fromString(server.arg(f.field))) {
        err = String(f.name) + " invalide.";
        break;
      }
    }
  }

  if (err.length()) {
    server.send(400, "text/html; charset=utf-8",
                buildConfigPage("<div class=\"msg err\">" + htmlEscape(err) + "</div>"));
    return;
  }

  net = next;
  saveNetConfig();

  String where = net.dhcp ? String("l'adresse attribuée par la box (affichée sur l'écran, page 2)")
                          : "<a href=\"http://" + net.ip.toString() + "/\">http://" + net.ip.toString() + "/</a>";
  server.send(200, "text/html; charset=utf-8",
              buildConfigPage("<div class=\"msg ok\">Enregistré. Redémarrage en cours, le dashboard sera sur " +
                              where + ".</div>"));
  Serial.println("Config reseau enregistree, redemarrage");
  restartAt = millis() + 1500;
}

String buildJson() {
  String json;
  json.reserve(1200);

  json += "{";
  json += "\"wifi\":{";
  json += "\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ssid\":\"" + jsonEscape(net.ssid) + "\",";
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
  json += "},";

  json += "\"ntp\":{";
  json += "\"synced\":" + String(gpsClockSynced() ? "true" : "false") + ",";
  json += "\"source\":\"" + String(gpsClockUsesPps() ? "PPS" : "NMEA") + "\",";
  uint32_t syncAge = gpsClockSyncAgeMs();
  json += "\"syncAgeMs\":" + (syncAge == UINT32_MAX ? String("null") : String(syncAge)) + ",";
  json += "\"requests\":" + String(ntpRequestCount());
  json += "},";

  Metar m = metarSnapshot();
  json += "\"metar\":{";
  json += "\"valid\":" + String(m.valid ? "true" : "false") + ",";
  json += "\"error\":\"" + jsonEscape(m.error) + "\",";
  json += "\"raw\":\"" + jsonEscape(m.raw) + "\",";
  json += "\"flightRules\":\"" + jsonEscape(m.flightRules) + "\",";
  json += "\"wind\":\"" + windDirText(m.windDir) + " " + String(m.windSpeed) +
          " " + jsonEscape(m.windUnit) + " (" + metarCompass(m.windDir) + ")\",";
  json += "\"temperature\":" + (m.temperature == INT16_MIN ? String("null") : String(m.temperature)) + ",";
  json += "\"humidity\":" + String(m.humidity) + ",";
  json += "\"qnh\":" + String(m.altimeter) + ",";
  String clouds;
  for (int i = 0; i < m.cloudCount; i++) clouds += (clouds.length() ? ", " : "") + metarCloudText(m.clouds[i]);
  json += "\"clouds\":\"" + jsonEscape(clouds) + "\",";
  json += "\"weather\":\"" + jsonEscape(m.weather) + "\",";
  json += "\"trend\":\"" + jsonEscape(m.trend) + "\"";
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
  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_POST, handleConfigPost);
  server.onNotFound(handleNotFound);
  server.begin();

  IPAddress ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
  Serial.println("Serveur web demarre");
  Serial.print("HTML : http://");
  Serial.print(ip);
  Serial.println("/");
  Serial.print("Config : http://");
  Serial.print(ip);
  Serial.println("/config");
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
  tft.setSPISpeed(40000000);

  tftCanvas = new GFXcanvas16(240, 240);
  if (!tftCanvas->getBuffer()) {
    Serial.println("Pas assez de RAM pour la page 2");
    delete tftCanvas;
    tftCanvas = nullptr;
  }
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
  lastPageSwitch = millis();

  gpsClockBegin(GPS_PPS_PIN);

  loadNetConfig();
  connectWifi();
  initWebServer();
  ntpServerBegin();
  metarBegin(METAR_STATION_VALUE, AVWX_TOKEN_VALUE);

  refreshGpsData();
  drawCurrentPage();
  updateOledTime();
}

void loop() {
  readGpsStream();
  ntpServerLoop();
  server.handleClient();
  ntpServerLoop();
  maintainWifi();

  if (restartAt && (long)(millis() - restartAt) >= 0) ESP.restart();

  if (millis() - lastPageSwitch >= TFT_PAGE_MS) {
    lastPageSwitch = millis();
    tftPage = (tftPage + 1) % TFT_PAGE_COUNT;
    lastDataRefresh = 0;
  }

  if (millis() - lastDataRefresh >= REFRESH_INTERVAL_MS) {
    lastDataRefresh = millis();
    blinkState = !blinkState;
    refreshGpsData();
    drawCurrentPage();
  }

  updateOledTime();
}
