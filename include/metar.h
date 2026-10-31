// METAR récupéré sur avwx.rest et traduit en français pour l'affichage.
#pragma once

#include <Arduino.h>

struct MetarCloud {
  String type;      // FEW, SCT, BKN, OVC, VV
  int altitudeFt;   // -1 si inconnue
  String modifier;  // CB, TCU ou vide
};

struct Metar {
  bool valid = false;
  String error;              // dernière erreur de récupération, vide si OK
  uint32_t fetchedMs = 0;    // millis() de la dernière récupération réussie

  String station;
  String raw;
  String timeRepr;           // ex. 312100Z
  String flightRules;        // VFR, MVFR, IFR, LIFR
  int windDir = -1;          // -1 : variable (VRB) ou inconnu
  int windSpeed = -1;
  int windGust = -1;
  int windVarFrom = -1, windVarTo = -1;
  String windUnit;
  int visibility = -1;       // en mètres (9999 = 10 km ou plus)
  String visibilityRepr;     // ex. CAVOK, 9999, 0500
  int temperature = INT16_MIN;
  int dewpoint = INT16_MIN;
  int humidity = -1;         // %
  int altimeter = -1;
  String altimeterUnit;
  MetarCloud clouds[4];
  int cloudCount = 0;
  String weather;            // phénomènes traduits
  String trend;              // tendance / remarques traduites
};

void metarBegin(const char* station, const char* token);
// Copie thread-safe du dernier METAR
Metar metarSnapshot();

// Traductions (utilisées aussi pour l'affichage)
String metarTranslateWeather(const String& code);
String metarTranslateRemarks(const String& remarks);
String metarCloudText(const MetarCloud& c);
const char* metarCompass(int deg);
