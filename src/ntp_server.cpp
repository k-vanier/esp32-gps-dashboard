#include "ntp_server.h"

#include <WiFiUdp.h>

#include "gps_clock.h"

static const uint16_t NTP_PORT = 123;
static const size_t NTP_PACKET_SIZE = 48;

static WiFiUDP udp;
static uint32_t requests = 0;

struct NtpClient {
  uint32_t ip;
  uint32_t requests;
  uint32_t lastMs;
};
static const int MAX_CLIENTS = 8;
static NtpClient clients[MAX_CLIENTS];
static int clientCount = 0;

static void noteClient(IPAddress ip) {
  uint32_t key = (uint32_t)ip;
  int slot = -1;
  for (int i = 0; i < clientCount; i++)
    if (clients[i].ip == key) slot = i;
  if (slot < 0) {
    if (clientCount < MAX_CLIENTS) {
      slot = clientCount++;
    } else {
      slot = 0;  // remplace le client le plus ancien
      for (int i = 1; i < MAX_CLIENTS; i++)
        if (clients[i].lastMs < clients[slot].lastMs) slot = i;
    }
    clients[slot] = {key, 0, 0};
  }
  clients[slot].requests++;
  clients[slot].lastMs = millis();
}

String ntpClientsJson() {
  String out = "[";
  for (int i = 0; i < clientCount; i++) {
    if (i) out += ",";
    out += "{\"ip\":\"" + IPAddress(clients[i].ip).toString() + "\",\"requests\":" +
           String(clients[i].requests) + ",\"lastSeenS\":" + String((millis() - clients[i].lastMs) / 1000) + "}";
  }
  out += "]";
  return out;
}

static void put32(uint8_t* p, uint32_t v) {
  p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static void putTime(uint8_t* p, const NtpTime& t) {
  put32(p, t.seconds);
  put32(p + 4, t.fraction);
}

void ntpServerBegin() {
  udp.begin(NTP_PORT);
}

uint32_t ntpRequestCount() {
  return requests;
}

void ntpServerLoop() {
  int size = udp.parsePacket();
  if (size <= 0) return;
  uint32_t rxUs = micros();

  uint8_t req[NTP_PACKET_SIZE];
  if (size < (int)NTP_PACKET_SIZE || udp.read(req, NTP_PACKET_SIZE) != (int)NTP_PACKET_SIZE) {
    udp.flush();
    return;
  }
  udp.flush();

  uint8_t version = (req[0] >> 3) & 0x07;
  uint8_t mode = req[0] & 0x07;
  if (mode != 3 && mode != 1) return;  // client ou symétrique actif uniquement
  if (version < 1 || version > 4) version = 4;

  bool synced = gpsClockSynced();
  bool pps = gpsClockUsesPps();
  uint32_t syncAgeMs = gpsClockSyncAgeMs();
  bool everSynced = syncAgeMs != UINT32_MAX;

  uint8_t res[NTP_PACKET_SIZE] = {0};
  // LI = 3 (non synchronisé) tant que le GPS n'a pas donné l'heure.
  // Mode 4 (serveur) pour un client, mode 2 (symétrique passif) pour un symétrique actif (RFC 4330 §5).
  res[0] = ((synced ? 0 : 3) << 6) | (version << 3) | (mode == 3 ? 4 : 2);
  res[1] = synced ? 1 : 16;                  // stratum
  res[2] = req[2];                           // poll : on reprend celui du client
  res[3] = (uint8_t)(pps ? -18 : -10);       // précision : ~4 µs avec PPS, ~1 ms sinon
  put32(res + 4, 0);                         // root delay
  // Root dispersion (format 16.16) : ~1 ms avec PPS, ~50 ms sans, plus la dérive
  // depuis la dernière seconde GPS (PHI = 15 ppm, RFC 5905 §7.3)
  uint32_t disp = pps ? 0x00000042 : 0x00000CCD;
  if (everSynced) disp += (uint32_t)((uint64_t)syncAgeMs * 15 * 65536 / 1000000000ULL);
  put32(res + 8, disp);
  memcpy(res + 12, pps ? "PPS" : "GPS", 4);  // reference ID
  memcpy(res + 24, req + 40, 8);             // origin = transmit du client
  // Jamais synchronisé : horodatages à zéro (RFC 5905 §7.3), l'horloge interne n'a pas d'heure
  if (everSynced) {
    putTime(res + 16, gpsClockLastSync());   // reference timestamp
    putTime(res + 32, gpsClockAt(rxUs));     // receive
    putTime(res + 40, gpsClockNow());        // transmit
  }

  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.write(res, NTP_PACKET_SIZE);
  udp.endPacket();
  requests++;
  noteClient(udp.remoteIP());
}
