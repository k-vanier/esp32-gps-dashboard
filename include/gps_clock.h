// Horloge disciplinée par le GPS, base du serveur NTP.
//
// Sans PPS, le début de seconde est estimé par l'arrivée du '$' de la trame RMC
// (le u-blox envoie RMC en premier à chaque époque), moins une latence fixe.
// Avec le PPS branché (GPS_PPS_PIN), le front montant donne le début exact.
#pragma once

#include <Arduino.h>

struct NtpTime {
  uint32_t seconds;   // secondes depuis 1900
  uint32_t fraction;  // 2^-32 s
};

void gpsClockBegin(int ppsPin);
void gpsClockOnChar(char c);         // à appeler pour chaque caractère NMEA reçu
bool gpsClockSynced();               // dernière seconde GPS reçue il y a peu
bool gpsClockUsesPps();
NtpTime gpsClockNow();
NtpTime gpsClockAt(uint32_t us);     // horodatage d'un instant micros() passé
NtpTime gpsClockLastSync();          // instant de la dernière discipline
uint32_t gpsClockSyncAgeMs();
