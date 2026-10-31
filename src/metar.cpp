#include "metar.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

static const uint32_t REFRESH_MS = 10UL * 60UL * 1000UL;  // un METAR sort toutes les 30 min
static const uint32_t RETRY_MS = 60UL * 1000UL;

static Metar current;
static SemaphoreHandle_t lock = nullptr;
static String stationId;
static String apiToken;

// ============================================================
// TRADUCTIONS
// ============================================================

struct Code { const char* code; const char* fr; };

static const Code PHENOMENA[] = {
  {"DZ", "bruine"}, {"RA", "pluie"}, {"SN", "neige"}, {"SG", "neige en grains"},
  {"PL", "granules de glace"}, {"GR", "grele"}, {"GS", "gresil"}, {"UP", "precipitations"},
  {"IC", "cristaux de glace"}, {"BR", "brume"}, {"FG", "brouillard"}, {"FU", "fumee"},
  {"VA", "cendres"}, {"DU", "poussiere"}, {"SA", "sable"}, {"HZ", "brume seche"},
  {"PO", "tourbillons"}, {"SQ", "grains"}, {"FC", "trombe"}, {"SS", "tempete de sable"},
  {"DS", "tempete de poussiere"},
};

static const char* lookup(const Code* table, size_t n, const String& code) {
  for (size_t i = 0; i < n; i++)
    if (code == table[i].code) return table[i].fr;
  return nullptr;
}

#define LOOKUP(table, code) lookup(table, sizeof(table) / sizeof(table[0]), code)

// Ex. "-SHRA" -> "averses de pluie faible", "VCFG" -> "brouillard au voisinage"
String metarTranslateWeather(const String& code) {
  String s = code;
  String suffix;
  if (s.startsWith("-")) { suffix = " faible"; s.remove(0, 1); }
  else if (s.startsWith("+")) { suffix = " fort"; s.remove(0, 1); }
  if (s.startsWith("VC")) { suffix += " au voisinage"; s.remove(0, 2); }
  if (s.startsWith("RE")) { suffix += " recent"; s.remove(0, 2); }

  String prefix, phen, after;
  for (size_t i = 0; i + 1 < s.length(); i += 2) {
    String c = s.substring(i, i + 2);
    if (c == "SH") prefix += "averses de ";
    else if (c == "TS") prefix += "orage ";
    else if (c == "BC") prefix += "bancs de ";
    else if (c == "MI") after += " mince";
    else if (c == "PR") after += " partiel";
    else if (c == "DR") after += " chasse-basse";
    else if (c == "BL") after += " chasse-haute";
    else if (c == "FZ") after += " givrant";
    else if (const char* fr = LOOKUP(PHENOMENA, c)) phen += (phen.length() ? ", " : "") + String(fr);
    else return code;  // code inconnu : on garde le brut
  }
  if (phen.isEmpty() && prefix == "orage ") return "orage" + suffix;
  if (prefix == "orage ") prefix = "orage, ";
  String out = prefix + phen + after + suffix;
  out.trim();
  return out;
}

String metarCloudText(const MetarCloud& c) {
  static const Code COVER[] = {
    {"FEW", "Peu"}, {"SCT", "Epars"}, {"BKN", "Fragmente"}, {"OVC", "Couvert"}, {"VV", "Plafond"},
  };
  const char* fr = LOOKUP(COVER, c.type);
  String out = fr ? fr : c.type;
  if (c.altitudeFt >= 0) out += " " + String(c.altitudeFt) + "ft";
  if (c.modifier.length()) out += " " + c.modifier;
  return out;
}

static bool isDigits(const String& s) {
  if (s.isEmpty()) return false;
  for (char ch : s) if (!isdigit((unsigned char)ch)) return false;
  return true;
}

static String visText(int meters) {
  if (meters >= 9999) return "visi 10km+";
  if (meters >= 5000) return "visi " + String(meters / 1000) + "km";
  return "visi " + String(meters) + "m";
}

// Tendance et remarques : "TEMPO 0500 FG VV///" -> "Temporaire: visi 500m, brouillard, ciel invisible"
String metarTranslateRemarks(const String& remarks) {
  String out;
  int start = 0;
  String src = remarks + " ";
  while (start < (int)src.length()) {
    int sp = src.indexOf(' ', start);
    String t = src.substring(start, sp);
    start = sp + 1;
    if (t.isEmpty()) continue;

    String fr;
    bool header = false;
    if (t == "TEMPO") { fr = "Temporaire:"; header = true; }
    else if (t == "BECMG") { fr = "Evolution:"; header = true; }
    else if (t == "NOSIG") fr = "pas d'evolution";
    else if (t == "RMK") { fr = "Rq:"; header = true; }
    else if (t == "CAVOK") fr = "CAVOK";
    else if (t == "NSW") fr = "fin des phenomenes";
    else if (t == "NSC" || t == "SKC" || t == "CLR") fr = "pas de nuages";
    else if (t.startsWith("FM") && isDigits(t.substring(2)) && t.length() == 6)
      fr = "a partir de " + t.substring(2, 4) + "h" + t.substring(4);
    else if (t.startsWith("TL") && isDigits(t.substring(2)) && t.length() == 6)
      fr = "jusqu'a " + t.substring(2, 4) + "h" + t.substring(4);
    else if (t.startsWith("AT") && isDigits(t.substring(2)) && t.length() == 6)
      fr = "a " + t.substring(2, 4) + "h" + t.substring(4);
    else if (t.length() == 4 && isDigits(t)) fr = visText(t.toInt());
    else if (t.startsWith("VV")) {
      String h = t.substring(2);
      fr = isDigits(h) ? "plafond " + String(h.toInt() * 100) + "ft" : String("ciel invisible");
    } else if (t.length() >= 6 && (t.startsWith("FEW") || t.startsWith("SCT") ||
                                   t.startsWith("BKN") || t.startsWith("OVC"))) {
      MetarCloud c{t.substring(0, 3), isDigits(t.substring(3, 6)) ? (int)t.substring(3, 6).toInt() * 100 : -1,
                   t.substring(6)};
      fr = metarCloudText(c);
      fr.toLowerCase();
    } else if (t.endsWith("KT") && t.length() >= 7) {
      fr = "vent " + t.substring(0, 3) + " " + t.substring(3, t.length() - 2) + "kt";
    } else {
      fr = metarTranslateWeather(t);
    }

    if (out.length()) out += header ? " " : (out.endsWith(":") ? " " : ", ");
    out += fr;
  }
  return out;
}

const char* metarCompass(int deg) {
  static const char* DIRS[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                               "S", "SSO", "SO", "OSO", "O", "ONO", "NO", "NNO"};
  if (deg < 0) return "VRB";
  return DIRS[((deg * 10 + 112) / 225) % 16];
}

// ============================================================
// RECUPERATION
// ============================================================

static int intOr(JsonVariantConst v, int fallback) {
  return v.isNull() ? fallback : v.as<int>();
}

static bool parse(const String& body, Metar& m) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    m.error = String("JSON: ") + err.c_str();
    return false;
  }
  if (!doc["error"].isNull()) {
    m.error = doc["error"].as<String>();
    return false;
  }

  m.station = doc["station"].as<String>();
  m.raw = doc["raw"].as<String>();
  m.timeRepr = doc["time"]["repr"].as<String>();
  m.flightRules = doc["flight_rules"].as<String>();
  m.windDir = intOr(doc["wind_direction"]["value"], -1);
  m.windSpeed = intOr(doc["wind_speed"]["value"], -1);
  m.windGust = intOr(doc["wind_gust"]["value"], -1);
  m.windUnit = doc["units"]["wind_speed"] | "kt";
  JsonArrayConst var = doc["wind_variable_direction"];
  if (var.size() == 2) {
    m.windVarFrom = intOr(var[0]["value"], -1);
    m.windVarTo = intOr(var[1]["value"], -1);
  }
  m.visibility = intOr(doc["visibility"]["value"], -1);
  m.visibilityRepr = doc["visibility"]["repr"] | "";
  m.temperature = intOr(doc["temperature"]["value"], INT16_MIN);
  m.dewpoint = intOr(doc["dewpoint"]["value"], INT16_MIN);
  m.humidity = doc["relative_humidity"].isNull() ? -1 : (int)lround(doc["relative_humidity"].as<float>() * 100);
  m.altimeter = intOr(doc["altimeter"]["value"], -1);
  m.altimeterUnit = doc["units"]["altimeter"] | "hPa";

  m.cloudCount = 0;
  for (JsonObjectConst c : doc["clouds"].as<JsonArrayConst>()) {
    if (m.cloudCount >= 4) break;
    MetarCloud& mc = m.clouds[m.cloudCount++];
    mc.type = c["type"] | "";
    mc.altitudeFt = c["altitude"].isNull() ? -1 : c["altitude"].as<int>() * 100;
    mc.modifier = c["modifier"] | "";
  }

  m.weather = "";
  for (JsonObjectConst w : doc["wx_codes"].as<JsonArrayConst>()) {
    if (m.weather.length()) m.weather += ", ";
    m.weather += metarTranslateWeather(w["repr"] | "");
  }

  m.trend = metarTranslateRemarks(doc["remarks"] | "");
  m.valid = true;
  m.error = "";
  return true;
}

static bool fetchOnce() {
  WiFiClientSecure client;
  client.setInsecure();  // pas de vérification du certificat (pas de magasin de CA embarqué)
  HTTPClient http;
  http.setTimeout(8000);
  String url = "https://avwx.rest/api/metar/" + stationId;

  Metar m;
  bool ok = false;
  if (!http.begin(client, url)) {
    m.error = "connexion impossible";
  } else {
    http.addHeader("Authorization", "Token " + apiToken);
    int code = http.GET();
    if (code == 200) {
      ok = parse(http.getString(), m);
    } else {
      m.error = code > 0 ? "HTTP " + String(code) : String(http.errorToString(code));
    }
    http.end();
  }

  xSemaphoreTake(lock, portMAX_DELAY);
  if (ok) {
    m.fetchedMs = millis();
    current = m;
  } else {
    current.error = m.error;  // on garde le dernier METAR valide
  }
  xSemaphoreGive(lock);

  Serial.println(ok ? "METAR " + m.raw : "METAR erreur : " + m.error);
  return ok;
}

static void fetchTask(void*) {
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }
    bool ok = fetchOnce();
    vTaskDelay(pdMS_TO_TICKS(ok ? REFRESH_MS : RETRY_MS));
  }
}

void metarBegin(const char* station, const char* token) {
  stationId = station;
  apiToken = token;
  lock = xSemaphoreCreateMutex();
  // Tâche à part : le TLS bloque plusieurs secondes, la boucle principale (NTP, écran) continue
  xTaskCreate(fetchTask, "metar", 12288, nullptr, 1, nullptr);
}

Metar metarSnapshot() {
  if (!lock) return Metar();
  xSemaphoreTake(lock, portMAX_DELAY);
  Metar copy = current;
  xSemaphoreGive(lock);
  return copy;
}
