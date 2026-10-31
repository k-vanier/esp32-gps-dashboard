#include "gps_sats.h"

static const int MAX_SATS = 64;
static const uint32_t EXPIRE_MS = 6000;

struct Sat {
  char sys;        // 'P' GPS, 'L' GLONASS, 'A' Galileo, 'B' BeiDou, 'Q' QZSS
  int prn;
  int elev;        // degrés, -1 inconnu
  int az;          // degrés, -1 inconnu
  int snr;         // dB-Hz, -1 non reçu
  bool used;
  uint32_t seenMs;
};

static Sat sats[MAX_SATS];
static int satCount = 0;
static uint32_t lastGsvMs = 0;

static void sendUbx(Stream& port, uint8_t cls, uint8_t id, const uint8_t* pl, uint16_t len) {
  uint8_t a = 0, b = 0;
  auto ck = [&](uint8_t x) { a += x; b += a; };
  uint8_t hdr[] = {0xB5, 0x62, cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
  for (int i = 2; i < 6; i++) ck(hdr[i]);
  for (uint16_t i = 0; i < len; i++) ck(pl[i]);
  port.write(hdr, sizeof(hdr));
  port.write(pl, len);
  port.write(a);
  port.write(b);
}

void gpsSatsEnableSentences(Stream& port) {
  // CFG-MSG : débit par port I2C, UART1, UART2, USB, SPI, réservé (non sauvegardé en flash)
  const uint8_t gsa[] = {0xF0, 0x02, 0, 1, 0, 1, 0, 0};
  const uint8_t gsv[] = {0xF0, 0x03, 0, 1, 0, 1, 0, 0};
  sendUbx(port, 0x06, 0x01, gsa, sizeof(gsa));
  delay(50);
  sendUbx(port, 0x06, 0x01, gsv, sizeof(gsv));
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool checksumOk(const char* s) {
  const char* star = strchr(s, '*');
  if (!star || hexv(star[1]) < 0 || hexv(star[2]) < 0) return false;
  uint8_t sum = 0;
  for (const char* p = s + 1; p < star; p++) sum ^= (uint8_t)*p;
  return sum == hexv(star[1]) * 16 + hexv(star[2]);
}

// Découpe en champs (modifie la chaîne). Retourne le nombre de champs.
static int split(char* s, char** f, int maxF) {
  int n = 0;
  char* star = strchr(s, '*');
  if (star) *star = 0;
  f[n++] = s;
  for (char* p = s; *p && n < maxF; p++) {
    if (*p == ',') {
      *p = 0;
      f[n++] = p + 1;
    }
  }
  return n;
}

static int num(const char* s) {
  return (s && *s) ? atoi(s) : -1;
}

static Sat* findOrAdd(char sys, int prn) {
  for (int i = 0; i < satCount; i++)
    if (sats[i].sys == sys && sats[i].prn == prn) return &sats[i];
  // Remplace l'entrée la plus ancienne si le tableau est plein
  int slot = satCount;
  if (satCount >= MAX_SATS) {
    slot = 0;
    for (int i = 1; i < MAX_SATS; i++)
      if (sats[i].seenMs < sats[slot].seenMs) slot = i;
  } else {
    satCount++;
  }
  sats[slot] = {sys, prn, -1, -1, -1, false, 0};
  return &sats[slot];
}

static char sysFromTalker(const char* t) {
  if (t[0] == 'G' && t[1] == 'P') return 'P';
  if (t[0] == 'G' && t[1] == 'L') return 'L';
  if (t[0] == 'G' && t[1] == 'A') return 'A';
  if ((t[0] == 'G' && t[1] == 'B') || (t[0] == 'B' && t[1] == 'D')) return 'B';
  if (t[0] == 'G' && t[1] == 'Q') return 'Q';
  return 'P';
}

// Champ systemId de GSA (NMEA 4.10)
static char sysFromId(int id) {
  switch (id) {
    case 1: return 'P';
    case 2: return 'L';
    case 3: return 'A';
    case 4: return 'B';
    case 5: return 'Q';
    default: return 0;
  }
}

// Sans systemId : on devine le système d'après la plage de PRN
static char sysFromPrn(int prn) {
  if (prn >= 65 && prn <= 96) return 'L';
  if (prn >= 193 && prn <= 202) return 'Q';
  return 'P';
}

void gpsSatsOnSentence(const char* line) {
  if (strlen(line) < 10 || !checksumOk(line)) return;
  const char* type = line + 3;
  bool isGsv = strncmp(type, "GSV", 3) == 0;
  bool isGsa = strncmp(type, "GSA", 3) == 0;
  if (!isGsv && !isGsa) return;

  char buf[100];
  strlcpy(buf, line, sizeof(buf));
  char* f[24];
  int n = split(buf, f, 24);
  uint32_t now = millis();

  if (isGsv) {
    // $xxGSV,total,num,inView,{prn,elev,az,snr}x4[,signalId]
    char sys = sysFromTalker(line + 1);
    for (int i = 4; i + 3 < n; i += 4) {
      int prn = num(f[i]);
      if (prn <= 0) continue;
      Sat* s = findOrAdd(sys, prn);
      s->elev = num(f[i + 1]);
      s->az = num(f[i + 2]);
      s->snr = num(f[i + 3]);
      s->seenMs = now;
    }
    lastGsvMs = now;
    return;
  }

  // $xxGSA,mode,fix,prn x12,pdop,hdop,vdop[,systemId]
  if (n < 17) return;
  char sys = n >= 19 ? sysFromId(num(f[18])) : 0;
  if (sys == 0 && line[1] == 'G' && line[2] != 'N') sys = sysFromTalker(line + 1);

  // Remise à zéro des "utilisés" du système concerné, puis marquage
  for (int i = 0; i < satCount; i++)
    if (sys == 0 || sats[i].sys == sys) sats[i].used = false;
  for (int i = 3; i < 15; i++) {
    int prn = num(f[i]);
    if (prn <= 0) continue;
    char s = sys ? sys : sysFromPrn(prn);
    for (int k = 0; k < satCount; k++)
      if (sats[k].sys == s && sats[k].prn == prn) sats[k].used = true;
  }
}

bool gpsSatsHaveGsv() {
  return lastGsvMs && millis() - lastGsvMs < EXPIRE_MS;
}

String gpsSatsJson() {
  String out = "[";
  uint32_t now = millis();
  bool first = true;
  for (int i = 0; i < satCount; i++) {
    const Sat& s = sats[i];
    if (now - s.seenMs > EXPIRE_MS) continue;
    if (!first) out += ",";
    first = false;
    out += "{\"sys\":\"";
    out += s.sys;
    out += "\",\"prn\":" + String(s.prn) + ",\"elev\":" + String(s.elev) + ",\"az\":" + String(s.az) +
           ",\"snr\":" + String(s.snr) + ",\"used\":" + (s.used ? "true" : "false") + "}";
  }
  out += "]";
  return out;
}
