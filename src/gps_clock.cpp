#include "gps_clock.h"

// Délai entre le début de la seconde GPS et le '$' de la RMC, sans PPS.
// Valeur typique du NEO-M8N à 38400 bauds ; à ajuster en comparant à un NTP de référence.
static const uint32_t NMEA_LATENCY_US = 60000;
// Au-delà, l'horloge n'est plus considérée comme synchronisée (holdover sur micros()).
static const uint32_t SYNC_TIMEOUT_MS = 30000;
static const uint32_t NTP_UNIX_OFFSET = 2208988800UL;

static volatile uint32_t ppsUs = 0;
static volatile bool ppsSeen = false;
static int ppsPinUsed = -1;

static uint32_t baseUnix = 0;     // seconde UTC (Unix) ...
static uint32_t baseUs = 0;       // ... qui a commencé à ce micros()
static uint32_t lastSyncMs = 0;
static bool everSynced = false;

static char line[100];
static size_t len = 0;
static uint32_t lineStartUs = 0;

static void IRAM_ATTR onPps() {
  ppsUs = micros();
  ppsSeen = true;
}

void gpsClockBegin(int ppsPin) {
  ppsPinUsed = ppsPin;
  if (ppsPin >= 0) {
    pinMode(ppsPin, INPUT);
    attachInterrupt(digitalPinToInterrupt(ppsPin), onPps, RISING);
  }
}

static int hex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool checksumOk(const char* s) {
  const char* star = strchr(s, '*');
  if (!star || star[1] == 0 || star[2] == 0) return false;
  uint8_t sum = 0;
  for (const char* p = s + 1; p < star; p++) sum ^= (uint8_t)*p;
  int hi = hex(star[1]), lo = hex(star[2]);
  return hi >= 0 && lo >= 0 && sum == (uint8_t)(hi * 16 + lo);
}

// Jours depuis 1970-01-01 (algorithme de Howard Hinnant)
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int32_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

// $xxRMC,hhmmss.ss,A,lat,N,lon,E,spd,cog,ddmmyy,...
static void handleRmc(const char* s, uint32_t startUs) {
  const char* f[12] = {0};
  int n = 0;
  f[n++] = s;
  for (const char* p = s; *p && *p != '*' && n < 12; p++)
    if (*p == ',') f[n++] = p + 1;
  if (n < 11) return;
  if (f[2][0] != 'A') return;  // pas de fix : heure non fiable
  const char* t = f[1];
  const char* d = f[9];
  if (strlen(t) < 6 || f[10] - d < 7) return;

  int hh = (t[0] - '0') * 10 + (t[1] - '0');
  int mi = (t[2] - '0') * 10 + (t[3] - '0');
  int ss = (t[4] - '0') * 10 + (t[5] - '0');
  int dd = (d[0] - '0') * 10 + (d[1] - '0');
  int mo = (d[2] - '0') * 10 + (d[3] - '0');
  int yy = 2000 + (d[4] - '0') * 10 + (d[5] - '0');
  if (hh > 23 || mi > 59 || ss > 60 || dd < 1 || dd > 31 || mo < 1 || mo > 12) return;

  uint32_t unix = (uint32_t)daysFromCivil(yy, mo, dd) * 86400UL + hh * 3600UL + mi * 60UL + ss;

  uint32_t edgeUs;
  noInterrupts();
  bool havePps = ppsSeen;
  uint32_t lastPps = ppsUs;
  interrupts();

  // Le PPS de cette seconde précède la RMC de moins d'une seconde
  if (ppsPinUsed >= 0 && havePps && (uint32_t)(startUs - lastPps) < 1000000UL) edgeUs = lastPps;
  else edgeUs = startUs - NMEA_LATENCY_US;

  baseUnix = unix;
  baseUs = edgeUs;
  lastSyncMs = millis();
  everSynced = true;
}

void gpsClockOnChar(char c) {
  if (c == '$') {
    len = 0;
    lineStartUs = micros();
  }
  if (c == '\r' || c == '\n') {
    if (len > 10 && line[0] == '$') {
      line[len] = 0;
      if (strncmp(line + 3, "RMC", 3) == 0 && checksumOk(line)) handleRmc(line, lineStartUs);
    }
    len = 0;
    return;
  }
  if (len < sizeof(line) - 1) line[len++] = c;
}

bool gpsClockSynced() {
  return everSynced && millis() - lastSyncMs < SYNC_TIMEOUT_MS;
}

bool gpsClockUsesPps() {
  return ppsPinUsed >= 0 && ppsSeen && (uint32_t)(micros() - ppsUs) < 2000000UL;
}

uint32_t gpsClockSyncAgeMs() {
  return everSynced ? millis() - lastSyncMs : UINT32_MAX;
}

NtpTime gpsClockAt(uint32_t us) {
  // micros() repasse à zéro toutes les ~71 min, mais la base est rafraîchie chaque seconde
  uint32_t elapsed = us - baseUs;
  NtpTime t;
  t.seconds = baseUnix + NTP_UNIX_OFFSET + elapsed / 1000000UL;
  t.fraction = (uint32_t)(((uint64_t)(elapsed % 1000000UL) << 32) / 1000000UL);
  return t;
}

NtpTime gpsClockNow() {
  return gpsClockAt(micros());
}

NtpTime gpsClockLastSync() {
  return {baseUnix + NTP_UNIX_OFFSET, 0};
}
