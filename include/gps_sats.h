// Satellites en vue, d'après les trames NMEA GSV (position, signal) et GSA (utilisés).
#pragma once

#include <Arduino.h>

// Demande au module d'envoyer GSV et GSA sur l'UART1 (si le TX de l'ESP est relié au RX du GPS).
void gpsSatsEnableSentences(Stream& port);
// Ligne NMEA complète, checksum inclus
void gpsSatsOnSentence(const char* line);
// Tableau JSON des satellites vus récemment
String gpsSatsJson();
// Vrai si des GSV arrivent
bool gpsSatsHaveGsv();
