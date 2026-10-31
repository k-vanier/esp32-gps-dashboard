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
#include "gps_sats.h"
#include "metar.h"
#include "ntp_server.h"
#include "secrets.h"
#include "shift_canvas.h"
#include "web_page.h"
#include "web_pages_extra.h"

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
  String metarStation = METAR_STATION_VALUE;
  String avwxToken = AVWX_TOKEN_VALUE;
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
const unsigned long REFRESH_INTERVAL_MS = 500;
const unsigned long OLED_SWITCH_MS      = 5000;
const unsigned long WIFI_RETRY_MS       = 10000;
const unsigned long TFT_PAGE_MS         = 10000;
const int TFT_PAGE_COUNT                = 4;

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

// Pages dessinées hors écran puis envoyées d'un bloc : pas de scintillement.
ShiftCanvas* tftCanvas = nullptr;

TinyGPSPlus gps;
TinyGPSCustom gpsQuality(gps, "GNGGA", 6);
HardwareSerial* gpsPort = nullptr;

// Dernières trames brutes, pour l'affichage "paquet GPS" de la page 2
char nmeaLine[100];
size_t nmeaLen = 0;
char lastRmc[100] = "";
char lastGga[100] = "";
char lastGsa[100] = "";
char lastGsv[100] = "";

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
      gpsSatsOnSentence(nmeaLine);
      char* star = strchr(nmeaLine, '*');
      if (star) *star = '\0';
      if (strncmp(nmeaLine + 3, "RMC", 3) == 0) strlcpy(lastRmc, nmeaLine, sizeof(lastRmc));
      else if (strncmp(nmeaLine + 3, "GGA", 3) == 0) strlcpy(lastGga, nmeaLine, sizeof(lastGga));
      else if (strncmp(nmeaLine + 3, "GSA", 3) == 0) strlcpy(lastGsa, nmeaLine, sizeof(lastGsa));
      else if (strncmp(nmeaLine + 3, "GSV", 3) == 0) strlcpy(lastGsv, nmeaLine, sizeof(lastGsv));
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
// TFT : STYLE CLAIR + ANIMATIONS
// ============================================================
// Chaque page est dessinée dans tftCanvas avec un décalage x (ShiftCanvas),
// ce qui permet la transition glissée. Hors animation, seules la pastille
// et la barre de progression sont renvoyées à l'écran (zones partielles).

#define D_BG     0xFFDF   // #f8fafc
#define D_HEAD   0x08A5   // #0f172a
#define D_TEXT   0x08A5
#define D_LABEL  0x63B1   // #64748b
#define D_DIV    0xE75E   // #e2e8f0
#define D_GREEN  0x1509   // #16a34a
#define D_GREEN2 0x8775   // #86efac
#define D_BLUE   0x231D   // #2563eb
#define D_RED    0xD924   // #dc2626
#define D_AMBER  0xDBA0   // #d97706
#define D_VIOLET 0xC13A   // #c026d3
#define D_WHITE  0xFFFF

const uint32_t TRANSITION_MS   = 350;
const uint32_t ENTER_ANIM_MS   = 1000;  // barres WiFi, flèche du vent
const uint32_t FRAME_FAST_MS   = 33;    // pendant une animation
const uint32_t FRAME_IDLE_MS   = 66;    // pulsation + barre de progression
const uint32_t FULL_PUSH_MS    = 500;   // rafraîchissement complet des données

struct {
  bool active = false;
  uint8_t from = 0, to = 0;
  uint32_t startMs = 0;
} transition;

uint32_t pageEnterMs = 0;
uint32_t lastFrameMs = 0;
uint32_t lastFullPushMs = 0;

// Valeurs affichées qui glissent vers la valeur réelle
float spdShown = NAN, altShown = NAN;

float easeOut(float t) {
  if (t >= 1) return 1;
  if (t <= 0) return 0;
  float u = 1 - t;
  return 1 - u * u * u;
}

uint16_t blend565(uint16_t a, uint16_t b, float t) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return ((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t);
}

void drawSmooth(ShiftCanvas& g, const GFXfont* font, int x, int top, uint16_t color, const String& s) {
  g.setFont(font);
  g.setTextColor(color);
  int16_t bx, by;
  uint16_t bw, bh;
  g.getTextBounds("Ay", 0, 0, &bx, &by, &bw, &bh);
  g.setCursor(x, top - by);
  g.print(s);
}

int textWidth(ShiftCanvas& g, const GFXfont* font, const String& s) {
  g.setFont(font);
  int16_t bx, by;
  uint16_t bw, bh;
  g.getTextBounds(s, 0, 0, &bx, &by, &bw, &bh);
  return bw + bx;
}

void drawLabel(ShiftCanvas& g, int x, int y, const String& s, uint16_t color = D_LABEL) {
  g.setFont(nullptr);
  g.setTextSize(1);
  g.setTextColor(color);
  g.setCursor(x, y);
  g.print(s);
}

void drawDivider(ShiftCanvas& g, int y) {
  g.fillRect(12, y, 216, 2, D_DIV);
}

void drawHeader(ShiftCanvas& g, const String& title, const String& pill, uint16_t pillColor) {
  g.fillRect(0, 0, 240, 32, D_HEAD);
  drawSmooth(g, &FreeSansBold9pt7b, 12, 10, D_WHITE, title);
  if (pill.length()) {
    int titleEnd = 12 + textWidth(g, &FreeSansBold9pt7b, title) + 8;
    int w = textWidth(g, &FreeSansBold9pt7b, pill) + 18;
    if (230 - w >= titleEnd) {
      g.fillRoundRect(230 - w, 6, w, 21, 10, pillColor);
      drawSmooth(g, &FreeSansBold9pt7b, 239 - w, 10, D_WHITE, pill);
    } else {
      // Pas la place à côté du titre : pastille en petite police
      w = pill.length() * 6 + 14;
      g.fillRoundRect(230 - w, 7, w, 19, 9, pillColor);
      drawLabel(g, 237 - w, 13, pill, D_WHITE);
    }
  }
}

// Texte 6x8 sur plusieurs lignes, coupé aux espaces
int drawWrapped(ShiftCanvas& g, int x, int y, int maxChars, int maxLines, uint16_t color, const String& s) {
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

// Captures d'écran anonymisées (/screen.bmp?anon) : valeurs fictives à la place des infos perso
bool screenAnon = false;

String shownIp(const String& real) { return screenAnon ? String("192.168.1.50") : real; }
String shownGateway(const String& real) { return screenAnon ? String("192.168.1.1") : real; }
String shownSsid(const String& real) { return screenAnon ? String("MonReseauWiFi") : real; }
String shownMac(const String& real) { return screenAnon ? String("AA:BB:CC:11:22:33") : real; }

// ---------- Page 1 : position ----------

uint16_t fixPillColor(uint32_t now) {
  if (fixText == "NO FIX") return gpsError ? D_RED : D_AMBER;
  // Pulsation douce de la pastille quand le fix est là
  float t = (sinf(now * 2 * PI / 1600.0f) + 1) * 0.5f * 0.55f;
  return blend565(fixText == "FIX 3D" ? D_GREEN : D_AMBER, D_GREEN2, t);
}

void drawCoord(ShiftCanvas& g, int labelY, const char* label, double v, char pos, char neg) {
  drawLabel(g, 12, labelY, label);
  bool ok = !gpsError && locValid;
  if (screenAnon && ok) v = (pos == 'N') ? 48.85837 : 2.29448;
  String txt = ok ? fmtCoord(fabs(v), 5) : String("--.-----");
  drawSmooth(g, &FreeSansBold18pt7b, 12, labelY + 13, ok ? D_TEXT : D_DIV, txt);
  if (ok) {
    int x = 12 + textWidth(g, &FreeSansBold18pt7b, txt) + 8;
    drawSmooth(g, &FreeSansBold12pt7b, x, labelY + 18, D_RED, String(v >= 0 ? pos : neg));
  }
}

void drawPagePosition(ShiftCanvas& g, uint32_t now) {
  g.fillRect(0, 0, 240, 240, D_BG);
  drawHeader(g, "Position", fixText == "NO FIX" ? (gpsError ? "ERREUR" : "RECHERCHE") : fixText, fixPillColor(now));

  drawCoord(g, 42, "LATITUDE", lat, 'N', 'S');
  drawCoord(g, 92, "LONGITUDE", lon, 'E', 'O');
  drawDivider(g, 146);

  bool ok = !gpsError;
  drawLabel(g, 12, 156, "ALT (m)");
  drawLabel(g, 96, 156, "VITESSE km/h");
  drawLabel(g, 196, 156, "SAT");
  drawSmooth(g, &FreeSansBold12pt7b, 12, 170, D_TEXT,
             ok && altValid && !isnan(altShown) ? String((long)lround(altShown)) : String("--"));
  drawSmooth(g, &FreeSansBold12pt7b, 96, 170, D_BLUE,
             ok && spdValid && !isnan(spdShown) ? String(spdShown, spdShown < 100 ? 1 : 0) : String("--"));
  drawSmooth(g, &FreeSansBold12pt7b, 196, 170, ok && satsValid && sats >= 4 ? D_GREEN : D_TEXT,
             ok && satsValid ? String(sats) : String("--"));

  bool synced = gpsClockSynced();
  drawLabel(g, 12, 216, synced ? String("NTP synchro (") + (gpsClockUsesPps() ? "PPS" : "GPS") + ")" : String("NTP en attente du GPS"),
            synced ? D_GREEN : D_AMBER);
  drawLabel(g, 228 - 6 * String(ntpRequestCount()).length() - 24, 216, String(ntpRequestCount()) + " req");
}

// ---------- Page 2 : réseau ----------

int wifiBars(int rssi) {
  if (rssi > -55) return 4;
  if (rssi > -65) return 3;
  if (rssi > -75) return 2;
  return 1;
}

void drawPageNetwork(ShiftCanvas& g, uint32_t now) {
  g.fillRect(0, 0, 240, 240, D_BG);
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  bool ap = setupApActive && !wifiOk;
  drawHeader(g, "Reseau", wifiOk ? "WiFi OK" : (ap ? "MODE AP" : "HORS LIGNE"),
             wifiOk ? D_BLUE : (ap ? D_AMBER : D_RED));

  drawLabel(g, 12, 42, ap ? "ADRESSE DU POINT D'ACCES" : "ADRESSE IP");
  String ip = wifiOk ? shownIp(WiFi.localIP().toString()) : (ap ? WiFi.softAPIP().toString() : String("--"));
  drawSmooth(g, &FreeSansBold18pt7b, 12, 56, D_BLUE, ip);
  drawDivider(g, 96);

  drawLabel(g, 12, 106, ap ? "RESEAU A REJOINDRE" : "SSID");
  drawSmooth(g, &FreeSansBold12pt7b, 12, 120, D_TEXT, ap ? String(SETUP_AP_SSID) : shownSsid(net.ssid));

  drawLabel(g, 12, 152, ap ? "MOT DE PASSE" : (net.dhcp ? "PASSERELLE (DHCP)" : "PASSERELLE"));
  drawSmooth(g, &FreeSansBold9pt7b, 12, 166, D_TEXT,
             ap ? String(SETUP_AP_PASS) : shownGateway((wifiOk ? WiFi.gatewayIP() : net.gateway).toString()));

  drawLabel(g, 140, 152, "SIGNAL");
  // Barres qui se remplissent une à une à l'arrivée sur la page
  int bars = wifiOk ? wifiBars(WiFi.RSSI()) : 0;
  uint32_t since = now - pageEnterMs;
  for (int k = 0; k < 4; k++) {
    int h = 6 + k * 5;
    bool lit = k < bars && since > 200 + k * 120;
    g.fillRect(140 + k * 8, 184 - h, 6, h, lit ? D_GREEN : D_DIV);
  }
  if (wifiOk) drawSmooth(g, &FreeSansBold9pt7b, 178, 168, D_TEXT, String(WiFi.RSSI()) + "dB");

  drawLabel(g, 12, 216, "MAC " + shownMac(WiFi.macAddress()));
}

// ---------- Page 3 : paquet GPS ----------

void drawPageGpsPacket(ShiftCanvas& g, uint32_t now) {
  g.fillRect(0, 0, 240, 240, D_BG);
  uint16_t c = gpsError ? D_RED : (locValid ? D_GREEN : D_AMBER);
  drawHeader(g, "Paquet GPS", gpsError ? "AUCUNE DONNEE" : (locValid ? "FIX" : "RECHERCHE"), c);

  // 4 dernières trames (35 caractères max par ligne : la boîte fait 224 px en police 6x8)
  g.fillRoundRect(8, 38, 224, 60, 6, D_DIV);
  g.setFont(nullptr);
  g.setTextColor(D_TEXT);
  // RMC et GGA contiennent la position : remplacées par un exemple en capture anonymisée
  bool anonRaw = screenAnon && locValid;
  const char* lines[4] = {
    anonRaw ? "$GNRMC,123519.00,A,4851.502,N,00217" : (lastRmc[0] ? lastRmc : "$--RMC (rien)"),
    anonRaw ? "$GNGGA,123519.00,4851.502,N,00217.6" : (lastGga[0] ? lastGga : "$--GGA (rien)"),
    lastGsa[0] ? lastGsa : "$--GSA (rien)",
    lastGsv[0] ? lastGsv : "$--GSV (rien)",
  };
  for (int i = 0; i < 4; i++) {
    g.setCursor(14, 45 + i * 13);
    g.print(String(lines[i]).substring(0, 35));
  }

  String qual = "--";
  if (!gpsError && gpsQuality.isValid()) {
    int q = atoi(gpsQuality.value());
    qual = q == 0 ? "AUCUN" : (q == 2 ? "DGPS" : (q == 1 ? "GPS" : String(q)));
  }
  String hdop = !gpsError && gps.hdop.isValid() && locValid ? String(gps.hdop.hdop(), 1) : String("--");
  String age = !gpsError && gps.location.isValid() ? String(gps.location.age() / 1000.0, 1) + "s" : String("--");
  drawLabel(g, 12, 108, "QUALITE");
  drawLabel(g, 116, 108, "HDOP");
  drawLabel(g, 176, 108, "AGE");
  drawSmooth(g, &FreeSansBold12pt7b, 12, 120, D_TEXT, qual);
  drawSmooth(g, &FreeSansBold12pt7b, 116, 120, D_TEXT, hdop);
  drawSmooth(g, &FreeSansBold12pt7b, 176, 120, D_TEXT, age);
  drawDivider(g, 150);

  drawLabel(g, 12, 160, "TRAMES OK");
  drawLabel(g, 116, 160, "ERREURS");
  drawLabel(g, 176, 160, "OCTETS");
  drawSmooth(g, &FreeSansBold12pt7b, 12, 172, D_GREEN, String(gps.passedChecksum()));
  drawSmooth(g, &FreeSansBold12pt7b, 116, 172, gps.failedChecksum() ? D_RED : D_TEXT, String(gps.failedChecksum()));
  uint32_t chars = gps.charsProcessed();
  drawSmooth(g, &FreeSansBold12pt7b, 176, 172, D_TEXT,
             chars >= 1000000 ? String(chars / 1000000.0, 1) + "M" : (chars >= 1000 ? String(chars / 1000) + "k" : String(chars)));

  drawLabel(g, 12, 216, String(GPS_USE_UART1 ? "UART1" : "UART2") + "  " + String(GPS_BAUD) + " bauds");
}

// ---------- Page 4 : METAR ----------

uint16_t flightRulesColor(const String& r) {
  if (r == "VFR") return D_GREEN;
  if (r == "MVFR") return D_BLUE;
  if (r == "IFR") return D_RED;
  if (r == "LIFR") return D_VIOLET;
  return D_LABEL;
}

// La flèche indique où va le vent ; elle tourne jusqu'à sa direction à l'arrivée sur la page
void drawWindRose(ShiftCanvas& g, int cx, int cy, int r, float arrowDeg, bool showArrow) {
  g.drawCircle(cx, cy, r, D_DIV);
  g.drawCircle(cx, cy, r - 1, D_DIV);
  for (int i = 0; i < 4; i++) {
    float a = i * HALF_PI;
    g.drawLine(cx + (r - 5) * sinf(a), cy - (r - 5) * cosf(a), cx + r * sinf(a), cy - r * cosf(a), D_LABEL);
  }
  drawLabel(g, cx - 2, cy - r - 10, "N");
  if (!showArrow) {
    g.fillCircle(cx, cy, 3, D_BLUE);
    return;
  }
  float a = radians(arrowDeg);
  int tx = cx + (r - 3) * sinf(a), ty = cy - (r - 3) * cosf(a);
  int bx = cx - (r - 7) * sinf(a), by = cy + (r - 7) * cosf(a);
  float pa = a + PI;
  int lx = tx + 9 * sinf(pa - 0.45f), ly = ty - 9 * cosf(pa - 0.45f);
  int rx = tx + 9 * sinf(pa + 0.45f), ry = ty - 9 * cosf(pa + 0.45f);
  for (int d = -1; d <= 1; d++) g.drawLine(bx + d, by, tx + d, ty, D_BLUE);
  g.fillTriangle(tx, ty, lx, ly, rx, ry, D_BLUE);
}

void drawPageMetar(ShiftCanvas& g, uint32_t now) {
  g.fillRect(0, 0, 240, 240, D_BG);
  Metar m = metarSnapshot();
  drawHeader(g, "METAR " + (m.valid ? m.station : net.metarStation), m.valid ? m.flightRules : String(""),
             flightRulesColor(m.flightRules));

  if (!m.valid) {
    drawSmooth(g, &FreeSansBold12pt7b, 12, 60, D_TEXT, WiFi.status() == WL_CONNECTED ? "Chargement..." : "Pas de WiFi");
    if (m.error.length()) drawWrapped(g, 12, 100, 35, 3, D_RED, m.error);
    return;
  }

  String obs = m.timeRepr.length() >= 6 ? "Obs. le " + m.timeRepr.substring(0, 2) + " a " + m.timeRepr.substring(2, 4) +
                                              "h" + m.timeRepr.substring(4, 6) + "Z"
                                        : String("");
  drawLabel(g, 12, 40, obs + "  maj " + String((millis() - m.fetchedMs) / 60000) + " min");

  drawLabel(g, 12, 56, "VENT");
  String wind = m.windSpeed == 0 ? String("Calme") : windDirText(m.windDir) + " " + String(m.windSpeed) + m.windUnit;
  drawSmooth(g, &FreeSansBold18pt7b, 12, 68, D_TEXT, wind);
  String sub = m.windDir >= 0 ? String("du ") + metarCompass(m.windDir) : String("variable");
  if (m.windGust > 0) sub += ", raf " + String(m.windGust) + m.windUnit;
  if (m.windVarFrom >= 0) sub += ", " + String(m.windVarFrom) + "-" + String(m.windVarTo);
  drawWrapped(g, 12, 100, 24, 1, D_LABEL, sub);
  float k = easeOut((now - pageEnterMs) / 700.0f);
  drawWindRose(g, 200, 84, 26, (m.windDir + 180) * k, m.windDir >= 0 && m.windSpeed > 0);
  drawDivider(g, 114);

  drawLabel(g, 12, 122, "TEMP");
  drawLabel(g, 92, 122, "HUMIDITE");
  drawLabel(g, 170, 122, "QNH");
  if (m.temperature != INT16_MIN) {
    String t = String(m.temperature);
    drawSmooth(g, &FreeSansBold12pt7b, 12, 136, D_TEXT, t);
    int w = textWidth(g, &FreeSansBold12pt7b, t);
    g.drawCircle(12 + w + 5, 139, 2, D_TEXT);
    g.drawCircle(12 + w + 5, 139, 3, D_TEXT);
  } else {
    drawSmooth(g, &FreeSansBold12pt7b, 12, 136, D_TEXT, "--");
  }
  if (m.dewpoint != INT16_MIN) drawLabel(g, 12, 160, "rosee " + String(m.dewpoint));
  drawSmooth(g, &FreeSansBold12pt7b, 92, 136, D_BLUE, m.humidity >= 0 ? String(m.humidity) + "%" : String("--"));
  drawSmooth(g, &FreeSansBold12pt7b, 170, 136, D_TEXT, m.altimeter >= 0 ? String(m.altimeter) : String("--"));
  drawDivider(g, 172);

  String sky;
  if (m.visibilityRepr == "CAVOK") sky = "CAVOK";
  else if (m.visibility >= 9999) sky = "Visi 10 km+";
  else if (m.visibility >= 0) sky = "Visi " + (m.visibility >= 5000 ? String(m.visibility / 1000) + " km" : String(m.visibility) + " m");
  String detail;
  for (int i = 0; i < m.cloudCount; i++) detail += (detail.length() ? ", " : "") + metarCloudText(m.clouds[i]);
  if (m.weather.length()) detail += (detail.length() ? ", " : "") + m.weather;
  drawSmooth(g, &FreeSansBold9pt7b, 12, 180, D_TEXT, sky);
  int y = 200;
  if (detail.length()) y += 10 * drawWrapped(g, 12, y, 36, 1, D_TEXT, detail);
  if (m.trend.length()) drawWrapped(g, 12, y, 36, (232 - y) / 10, D_AMBER, m.trend);
}

// ---------- Moteur ----------

void drawPage(uint8_t page, int16_t ox, uint32_t now) {
  ShiftCanvas& g = *tftCanvas;
  g.setTextWrap(false);
  g.setOffset(ox);
  switch (page) {
    case 0: drawPagePosition(g, now); break;
    case 1: drawPageNetwork(g, now); break;
    case 2: drawPageGpsPacket(g, now); break;
    default: drawPageMetar(g, now); break;
  }
  g.setOffset(0);
}

// Capture BMP 240x240 RGB565 d'une page, animations d'arrivée terminées
void handleScreenshot() {
  if (!tftCanvas) {
    server.send(503, "text/plain", "Pas de canvas");
    return;
  }
  int page = server.hasArg("page") ? constrain(server.arg("page").toInt(), 0, TFT_PAGE_COUNT - 1) : tftPage;
  uint32_t savedEnter = pageEnterMs;
  pageEnterMs = millis() - 10000;
  screenAnon = server.hasArg("anon");
  drawPage(page, 0, millis());
  screenAnon = false;
  pageEnterMs = savedEnter;

  const uint32_t dataSize = 240 * 240 * 2, offset = 14 + 40 + 12;
  uint8_t hdr[66] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) { memcpy(hdr + at, &v, 4); };
  auto put16 = [&](int at, uint16_t v) { memcpy(hdr + at, &v, 2); };
  put32(2, offset + dataSize);
  put32(10, offset);
  put32(14, 40);          // BITMAPINFOHEADER
  put32(18, 240);
  put32(22, 240);         // hauteur positive : lignes de bas en haut
  put16(26, 1);
  put16(28, 16);
  put32(30, 3);           // BI_BITFIELDS
  put32(34, dataSize);
  put32(54, 0xF800);      // masques RGB565
  put32(58, 0x07E0);
  put32(62, 0x001F);

  server.setContentLength(offset + dataSize);
  server.send(200, "image/bmp", "");
  server.sendContent((const char*)hdr, sizeof(hdr));
  const uint16_t* buf = tftCanvas->getBuffer();
  for (int y = 239; y >= 0; y--) server.sendContent((const char*)(buf + y * 240), 480);
  lastFullPushMs = 0;  // la prochaine image redessine l'écran réel
}

void pushRegion(int x, int y, int w, int h) {
  uint16_t* buf = tftCanvas->getBuffer();
  tft.startWrite();
  tft.setAddrWindow(x, y, w, h);
  for (int r = 0; r < h; r++) tft.writePixels(buf + (y + r) * 240 + x, w);
  tft.endWrite();
}

void startPageTransition() {
  transition.active = true;
  transition.from = tftPage;
  transition.to = (tftPage + 1) % TFT_PAGE_COUNT;
  transition.startMs = millis();
  pageEnterMs = transition.startMs;
}

// Fait glisser les valeurs affichées vers les valeurs réelles. Retourne vrai si ça bouge encore.
bool stepRollingValues() {
  bool moving = false;
  auto step = [&](float& shown, bool valid, float target, float eps) {
    if (!valid || isnan(shown)) { shown = valid ? target : NAN; return; }
    float d = target - shown;
    if (fabsf(d) < eps) shown = target;
    else { shown += d * 0.3f; moving = true; }
  };
  step(spdShown, !gpsError && spdValid, spd, 0.05f);
  step(altShown, !gpsError && altValid, alt, 0.5f);
  return moving;
}

void tftFrame() {
  if (!tftCanvas) return;
  uint32_t now = millis();

  bool rolling = stepRollingValues();
  bool entering = now - pageEnterMs < ENTER_ANIM_MS;
  bool heavy = transition.active || entering || rolling;
  if (now - lastFrameMs < (heavy ? FRAME_FAST_MS : FRAME_IDLE_MS)) return;
  lastFrameMs = now;

  if (transition.active) {
    float e = easeOut((now - transition.startMs) / (float)TRANSITION_MS);
    int16_t shift = (int16_t)lroundf(240 * e);
    drawPage(transition.from, -shift, now);
    drawPage(transition.to, 240 - shift, now);
    if (e >= 1) {
      transition.active = false;
      tftPage = transition.to;
    }
  } else {
    drawPage(tftPage, 0, now);
  }

  // Barre de progression jusqu'à la page suivante
  float p = transition.active ? 0 : min(1.0f, (now - lastPageSwitch) / (float)TFT_PAGE_MS);
  tftCanvas->fillRect(0, 237, 240, 3, D_DIV);
  tftCanvas->fillRect(0, 237, (int)(240 * p), 3, D_BLUE);

  if (heavy || now - lastFullPushMs >= FULL_PUSH_MS) {
    tft.drawRGBBitmap(0, 0, tftCanvas->getBuffer(), 240, 240);
    lastFullPushMs = now;
  } else {
    pushRegion(120, 4, 120, 26);  // pastille (pulsation)
    pushRegion(0, 237, 240, 3);   // barre de progression
  }
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
    net.metarStation = prefs.getString("station", net.metarStation);
    net.avwxToken    = prefs.getString("avwx", net.avwxToken);
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
  prefs.putString("station", net.metarStation);
  prefs.putString("avwx", net.avwxToken);
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
  html.replace("%STATION%", htmlEscape(net.metarStation));
  html.replace("%TOKEN_HINT%", net.avwxToken.length() >= 4
                                   ? "Actuel : ..." + htmlEscape(net.avwxToken.substring(net.avwxToken.length() - 4))
                                   : String("Aucun jeton"));
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
  next.metarStation = server.arg("station");
  next.metarStation.trim();
  next.metarStation.toUpperCase();
  String token = server.arg("token");
  token.trim();
  if (token.length()) next.avwxToken = token;

  bool stationOk = next.metarStation.length() == 4;
  for (char c : next.metarStation) stationOk = stationOk && isalnum((unsigned char)c);

  if (next.ssid.isEmpty() || next.ssid.length() > 32) err = "SSID vide ou trop long.";
  else if (next.pass.length() && (next.pass.length() < 8 || next.pass.length() > 63))
    err = "Le mot de passe WPA doit faire 8 à 63 caractères.";
  else if (!stationOk) err = "Le code OACI doit faire 4 lettres ou chiffres (ex. LFPG).";
  else if (next.avwxToken.length() > 128) err = "Jeton AVWX trop long.";

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

String jsonInt(int v, int missing) {
  return v == missing ? String("null") : String(v);
}

String jsonStr(const String& s) {
  return "\"" + jsonEscape(s) + "\"";
}

String buildMetarJson() {
  Metar m = metarSnapshot();
  String j = "{";
  j += "\"valid\":" + String(m.valid ? "true" : "false");
  j += ",\"error\":" + jsonStr(m.error);
  j += ",\"station\":" + jsonStr(m.valid ? m.station : net.metarStation);
  j += ",\"updatedMin\":" + (m.valid ? String((millis() - m.fetchedMs) / 60000) : String("null"));
  j += ",\"raw\":" + jsonStr(m.raw);
  j += ",\"time\":" + jsonStr(m.timeRepr);
  j += ",\"flightRules\":" + jsonStr(m.flightRules);
  j += ",\"wind\":{\"dir\":" + jsonInt(m.windDir, -1) + ",\"dirText\":" + jsonStr(windDirText(m.windDir)) +
       ",\"compass\":" + jsonStr(metarCompass(m.windDir)) + ",\"speed\":" + jsonInt(m.windSpeed, -1) +
       ",\"gust\":" + jsonInt(m.windGust, -1) + ",\"varFrom\":" + jsonInt(m.windVarFrom, -1) +
       ",\"varTo\":" + jsonInt(m.windVarTo, -1) + ",\"unit\":" + jsonStr(m.windUnit) + "}";
  j += ",\"visibility\":" + jsonInt(m.visibility, -1);
  j += ",\"visibilityRepr\":" + jsonStr(m.visibilityRepr);
  j += ",\"temperature\":" + jsonInt(m.temperature, INT16_MIN);
  j += ",\"dewpoint\":" + jsonInt(m.dewpoint, INT16_MIN);
  j += ",\"humidity\":" + jsonInt(m.humidity, -1);
  j += ",\"qnh\":" + jsonInt(m.altimeter, -1);
  j += ",\"qnhUnit\":" + jsonStr(m.altimeterUnit);
  j += ",\"clouds\":[";
  for (int i = 0; i < m.cloudCount; i++) {
    if (i) j += ",";
    j += "{\"type\":" + jsonStr(m.clouds[i].type) + ",\"altitudeFt\":" + jsonInt(m.clouds[i].altitudeFt, -1) +
         ",\"modifier\":" + jsonStr(m.clouds[i].modifier) + ",\"text\":" + jsonStr(metarCloudText(m.clouds[i])) + "}";
  }
  j += "]";
  j += ",\"weather\":" + jsonStr(m.weather);
  j += ",\"trend\":" + jsonStr(m.trend);
  j += "}";
  return j;
}

String buildSatsJson() {
  uint32_t syncAge = gpsClockSyncAgeMs();
  String j = "{";
  j += "\"gsv\":" + String(gpsSatsHaveGsv() ? "true" : "false");
  j += ",\"sats\":" + gpsSatsJson();
  j += ",\"gps\":{\"error\":" + String(gpsError ? "true" : "false") + ",\"fix\":" + jsonStr(fixText) +
       ",\"used\":" + String(satsValid ? sats : 0) +
       ",\"hdop\":" + (gps.hdop.isValid() && locValid ? String(gps.hdop.hdop(), 2) : String("null")) +
       ",\"utc\":" + jsonStr(getUtcTimeString()) + ",\"france\":" + jsonStr(getFranceTimeString()) + "}";
  j += ",\"ntp\":{\"synced\":" + String(gpsClockSynced() ? "true" : "false") +
       ",\"source\":" + jsonStr(gpsClockUsesPps() ? "PPS" : "NMEA") +
       ",\"stratum\":" + String(gpsClockSynced() ? 1 : 16) +
       ",\"syncAgeMs\":" + (syncAge == UINT32_MAX ? String("null") : String(syncAge)) +
       ",\"requests\":" + String(ntpRequestCount()) + ",\"clients\":" + ntpClientsJson() +
       ",\"address\":" + jsonStr(WiFi.localIP().toString()) + "}";
  j += ",\"uptimeS\":" + String(millis() / 1000);
  j += ",\"heap\":{\"free\":" + String(ESP.getFreeHeap()) + ",\"maxBlock\":" + String(ESP.getMaxAllocHeap()) +
       ",\"minFree\":" + String(ESP.getMinFreeHeap()) + "}";
  j += "}";
  return j;
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

  json += "\"metar\":" + buildMetarJson();

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
  server.on("/metar", [] { server.send_P(200, "text/html; charset=utf-8", METAR_HTML); });
  server.on("/sats", [] { server.send_P(200, "text/html; charset=utf-8", SATS_HTML); });
  server.on("/style.css", [] { server.send_P(200, "text/css; charset=utf-8", COMMON_CSS); });
  server.on("/screen.bmp", handleScreenshot);
  server.on("/metar.json", [] { server.send(200, "application/json; charset=utf-8", buildMetarJson()); });
  server.on("/sats.json", [] { server.send(200, "application/json; charset=utf-8", buildSatsJson()); });
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

  tftCanvas = new ShiftCanvas(240, 240);
  if (!tftCanvas->getBuffer()) {
    Serial.println("Pas assez de RAM pour le canvas TFT");
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
  gpsSatsEnableSentences(*gpsPort);

  lastGpsActivity = millis();
  lastWifiAttempt = millis();
  lastPageSwitch = millis();

  gpsClockBegin(GPS_PPS_PIN);

  loadNetConfig();
  connectWifi();
  initWebServer();
  ntpServerBegin();
  metarBegin(net.metarStation.c_str(), net.avwxToken.c_str());

  refreshGpsData();
  pageEnterMs = millis();
  tftFrame();
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
    startPageTransition();
  }

  if (millis() - lastDataRefresh >= REFRESH_INTERVAL_MS) {
    lastDataRefresh = millis();
    refreshGpsData();
  }

  tftFrame();

  updateOledTime();
}
