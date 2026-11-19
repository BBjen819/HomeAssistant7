#include "i18n.h"
#include "ha_client.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const uint32_t POLL_PERIOD_MS = 2000;
static const uint32_t HTTP_TIMEOUT_MS = 5000;

struct Entity {
  bool found = true;
  bool dirty = false;
  String state;
  std::map<String, String> attrs;
};

static SemaphoreHandle_t s_mtx = nullptr;
static TaskHandle_t s_task = nullptr;
static Preferences s_prefs;

static String s_url, s_token;
static std::map<String, std::vector<String>> s_watch;   // entite -> attributs
static std::vector<std::pair<String, String>> s_calls;   // service, corps JSON
static std::map<String, Entity> s_ents;
static HaStatus s_status = HaStatus::NOT_CONFIGURED;
static String s_status_msg = "-";

struct Lock {
  Lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mtx); }
};

static void set_status(HaStatus st, const String &msg) {
  Lock l;
  s_status = st;
  s_status_msg = msg;
}

static String normalize_url(String u) {
  u.trim();
  while (u.endsWith("/")) u.remove(u.length() - 1);
  if (u.length() && !u.startsWith("http://") && !u.startsWith("https://")) u = "http://" + u;
  return u;
}

// Connexion HTTP persistante (keep-alive) utilisee par la tache de fond :
// evite d'ouvrir une connexion TCP a chaque lecture, ce qui limite l'activite
// Wi-Fi et l'execution de code depuis la flash (concurrente de la PSRAM).
struct HttpConn {
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  String base;
};
static HttpConn *s_conn = nullptr;

static int do_request(HttpConn &c, const String &base, const String &token, const char *method,
                      const String &path, const String &body, String *resp, uint32_t timeout_ms) {
  if (WiFi.status() != WL_CONNECTED) return HTTPC_ERROR_NOT_CONNECTED;
  bool https = base.startsWith("https://");
  if (c.base != base) {   // Nouveau serveur : on repart de zero
    c.http.end();
    c.plain.stop();
    c.secure.stop();
    c.base = base;
  }
  if (https) c.secure.setInsecure();   // Certificat non verifie (reseau local)
  c.http.setReuse(true);
  c.http.setConnectTimeout(timeout_ms);
  c.http.setTimeout(timeout_ms);
  if (!c.http.begin(https ? (WiFiClient &)c.secure : c.plain, base + path)) return HTTPC_ERROR_CONNECTION_REFUSED;
  c.http.addHeader("Authorization", "Bearer " + token);
  c.http.addHeader("Content-Type", "application/json");
  int code = (strcmp(method, "POST") == 0) ? c.http.POST(body) : c.http.GET();
  if (resp && code > 0) *resp = c.http.getString();
  c.http.end();   // Avec setReuse(true), la connexion TCP reste ouverte
  if (code < 0) {
    c.plain.stop();
    c.secure.stop();
  }
  return code;
}

// Requete HTTP vers HA. Retourne le code HTTP (<0 = erreur reseau HTTPClient).
// persistent = true seulement depuis la tache de fond.
static int http_req(const String &base, const String &token, const char *method, const String &path,
                    const String &body, String *resp, bool persistent = false,
                    uint32_t timeout_ms = HTTP_TIMEOUT_MS) {
  if (persistent && s_conn) return do_request(*s_conn, base, token, method, path, body, resp, timeout_ms);
  HttpConn c;
  int code = do_request(c, base, token, method, path, body, resp, timeout_ms);
  c.plain.stop();
  c.secure.stop();
  return code;
}

static String error_text(int code) {
  if (code == 401) return TR("Jeton refusé (401)", "Token rejected (401)");
  if (code == 404) return TR("API introuvable (404) – vérifiez l'URL", "API not found (404) – check the URL");
  if (code < 0) return TR("Injoignable : ", "Unreachable: ") + HTTPClient::errorToString(code);
  return TR("Erreur HTTP ", "HTTP error ") + String(code);
}

static String variant_to_string(JsonVariantConst v) {
  if (v.isNull()) return "";
  if (v.is<const char *>()) return v.as<const char *>();
  String s;
  serializeJson(v, s);
  return s;
}

// ---------------------------------------------------------------------------
// Tache de fond
// ---------------------------------------------------------------------------
static bool fetch_entity(const String &url, const String &tok, const String &id, const std::vector<String> &attrs) {
  String resp;
  int code = http_req(url, tok, "GET", "/api/states/" + id, "", &resp, true);
  if (code == 404) {
    Lock l;
    Entity &e = s_ents[id];
    if (e.found) { e.found = false; e.state = ""; e.attrs.clear(); e.dirty = true; }
    return true;
  }
  if (code != 200) {
    set_status(code == 401 ? HaStatus::AUTH_ERROR : HaStatus::ERROR, error_text(code));
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) return true;

  String st = doc["state"] | "";
  std::map<String, String> av;
  for (const String &a : attrs) av[a] = variant_to_string(doc["attributes"][a]);

  Lock l;
  Entity &e = s_ents[id];
  if (!e.found || e.state != st || e.attrs != av) {
    e.found = true;
    e.state = st;
    e.attrs = av;
    e.dirty = true;
  }
  s_status = HaStatus::OK;
  s_status_msg = TR("Connecté", "Connected");
  return true;
}

static void process_calls(const String &url, const String &tok) {
  std::vector<std::pair<String, String>> calls;
  {
    Lock l;
    calls.swap(s_calls);
  }
  for (auto &c : calls) {
    int dot = c.first.indexOf('.');
    if (dot <= 0) continue;
    String path = "/api/services/" + c.first.substring(0, dot) + "/" + c.first.substring(dot + 1);
    int code = http_req(url, tok, "POST", path, c.second, nullptr, true);
    Serial.printf("[HA] %s %s -> %d\n", c.first.c_str(), c.second.c_str(), code);
    if (code != 200 && code != 201) set_status(code == 401 ? HaStatus::AUTH_ERROR : HaStatus::ERROR, error_text(code));
  }
}

static void ha_task(void *) {
  s_conn = new HttpConn();
  for (;;) {
    String url, tok;
    std::map<String, std::vector<String>> watch;
    {
      Lock l;
      url = s_url;
      tok = s_token;
      watch = s_watch;
    }
    if (url.isEmpty() || tok.isEmpty()) {
      set_status(HaStatus::NOT_CONFIGURED, TR("Non configuré", "Not configured"));
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      set_status(HaStatus::ERROR, TR("Wi-Fi déconnecté", "Wi-Fi disconnected"));
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
      continue;
    }

    process_calls(url, tok);
    if (watch.empty()) {
      // Rien a surveiller : simple verification de la connexion
      String resp;
      int code = http_req(url, tok, "GET", "/api/", "", &resp, true);
      if (code == 200) set_status(HaStatus::OK, TR("Connecté", "Connected"));
      else set_status(code == 401 ? HaStatus::AUTH_ERROR : HaStatus::ERROR, error_text(code));
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
      continue;
    }
    for (auto &w : watch) {
      process_calls(url, tok);   // Les actions passent avant la lecture
      if (!fetch_entity(url, tok, w.first, w.second)) break;
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_PERIOD_MS));
  }
}

// ---------------------------------------------------------------------------
// API publique
// ---------------------------------------------------------------------------
void ha_begin() {
  s_mtx = xSemaphoreCreateMutex();
  s_prefs.begin("ha", false);
  s_url = s_prefs.getString("url", "");
  s_token = s_prefs.getString("token", "");
  xTaskCreatePinnedToCore(ha_task, "ha", 12288, nullptr, 1, &s_task, 0);
  Serial.printf("[HA] URL : %s, jeton : %s\n", s_url.length() ? s_url.c_str() : "(aucune)",
                s_token.length() ? "oui" : "non");
}

void ha_get_config(String &url, bool &has_token) {
  Lock l;
  url = s_url;
  has_token = s_token.length() > 0;
}

bool ha_get_credentials(String &url, String &token) {
  Lock l;
  url = s_url;
  token = s_token;
  return url.length() && token.length();
}

void ha_set_config(const String &url, const String &token) {
  {
    Lock l;
    s_url = normalize_url(url);
    if (token.length()) s_token = token;
    s_prefs.putString("url", s_url);
    s_prefs.putString("token", s_token);
    s_status = HaStatus::CONNECTING;
    s_status_msg = TR("Connexion…", "Connecting…");
    for (auto &e : s_ents) e.second.dirty = true;
  }
  Serial.printf("[HA] Nouvelle configuration : %s\n", s_url.c_str());
  if (s_task) xTaskNotifyGive(s_task);
}

bool ha_test(const String &url_in, const String &token_in, String &message) {
  String url = normalize_url(url_in), tok = token_in;
  if (tok.isEmpty()) {
    Lock l;
    tok = s_token;
  }
  if (url.isEmpty() || tok.isEmpty()) {
    message = TR("URL et jeton requis", "URL and token required");
    return false;
  }
  String resp;
  int code = http_req(url, tok, "GET", "/api/config", "", &resp);
  if (code != 200) {
    message = error_text(code);
    return false;
  }
  JsonDocument doc;
  deserializeJson(doc, resp);
  message = String(TR("Connecté à \"", "Connected to \"")) + (doc["location_name"] | "Home Assistant") + "\" (version " +
            (doc["version"] | "?") + ")";
  return true;
}

// GET sur HA en lisant la reponse en flux avec un filtre ArduinoJson : seules
// les cles utiles sont gardees en memoire (les reponses completes font
// plusieurs centaines de Ko).
static bool stream_get(const String &path, JsonDocument &filter, JsonDocument &doc, String &error) {
  String url, tok;
  {
    Lock l;
    url = s_url;
    tok = s_token;
  }
  if (url.isEmpty() || tok.isEmpty()) {
    error = TR("Home Assistant non configuré", "Home Assistant not configured");
    return false;
  }
  WiFiClient plain;
  WiFiClientSecure secure;
  bool https = url.startsWith("https://");
  if (https) secure.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);   // Pas de "chunked" : on peut lire le flux directement
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  http.setTimeout(15000);
  if (!http.begin(https ? (WiFiClient &)secure : plain, url + path)) {
    error = "URL invalide";
    return false;
  }
  http.addHeader("Authorization", "Bearer " + tok);
  int code = http.GET();
  if (code != 200) {
    http.end();
    error = error_text(code);
    return false;
  }
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    error = String(TR("Réponse illisible : ", "Unreadable response: ")) + err.c_str();
    return false;
  }
  return true;
}

// Actions disponibles, par domaine, en format compact pour le designer :
//   {"light": {"turn_on": {"n": "Turn on", "f": {"brightness": [0, 255], ...}, "r": 1?}}}
//   f : champs numeriques [min, max] (null si non borne)
//   r : 1 si l'action exige d'autres parametres que entity_id
bool ha_list_services(String &out, String &error) {
  JsonDocument filter;
  JsonObject fs = filter[0].to<JsonObject>();
  fs["domain"] = true;
  JsonObject svc = fs["services"]["*"].to<JsonObject>();
  svc["name"] = true;
  JsonObject fld = svc["fields"]["*"].to<JsonObject>();
  fld["required"] = true;
  fld["selector"]["number"] = true;
  JsonObject sub = fld["fields"]["*"].to<JsonObject>();   // Sections repliables (advanced_fields)
  sub["required"] = true;
  sub["selector"]["number"] = true;

  JsonDocument doc;
  if (!stream_get("/api/services", filter, doc, error)) return false;

  JsonDocument o;
  JsonObject root = o.to<JsonObject>();
  for (JsonObject d : doc.as<JsonArray>()) {
    const char *dom = d["domain"] | "";
    if (!*dom) continue;
    JsonObject od = root[dom].to<JsonObject>();
    for (JsonPair sp : d["services"].as<JsonObject>()) {
      JsonObject s = sp.value();
      JsonObject os = od[sp.key().c_str()].to<JsonObject>();
      os["n"] = s["name"] | sp.key().c_str();
      JsonObject nf = os["f"].to<JsonObject>();
      bool needs = false;
      auto handle = [&](const char *name, JsonObject f) {
        JsonVariant num = f["selector"]["number"];
        bool numeric = !num.isNull();
        if (numeric) {
          JsonArray a = nf[name].to<JsonArray>();
          a.add(num["min"]);
          a.add(num["max"]);
        }
        if ((f["required"] | false) && strcmp(name, "entity_id") != 0) needs = true;
      };
      for (JsonPair fp : s["fields"].as<JsonObject>()) {
        JsonObject f = fp.value();
        if (f["fields"].is<JsonObject>()) {
          for (JsonPair sf : f["fields"].as<JsonObject>()) handle(sf.key().c_str(), sf.value());
        } else {
          handle(fp.key().c_str(), f);
        }
      }
      if (needs) os["r"] = 1;
    }
  }
  out = "";
  serializeJson(o, out);
  return true;
}

// Etat complet d'une entite (attributs min/max, min_temp...) pour le designer
bool ha_get_state(const String &id, String &out, String &error) {
  for (size_t i = 0; i < id.length(); i++) {
    char c = id[i];
    if (!(isalnum((unsigned char)c) || c == '_' || c == '.')) {
      error = TR("Entité invalide", "Invalid entity");
      return false;
    }
  }
  String url, tok;
  {
    Lock l;
    url = s_url;
    tok = s_token;
  }
  if (url.isEmpty() || tok.isEmpty()) {
    error = TR("Home Assistant non configuré", "Home Assistant not configured");
    return false;
  }
  int code = http_req(url, tok, "GET", "/api/states/" + id, "", &out);
  if (code != 200) {
    error = code == 404 ? String(TR("Entité introuvable", "Entity not found")) : error_text(code);
    return false;
  }
  return true;
}

bool ha_list_entities(String &out, String &error) {
  JsonDocument filter;
  filter[0]["entity_id"] = true;
  filter[0]["attributes"]["friendly_name"] = true;
  JsonDocument doc;
  if (!stream_get("/api/states", filter, doc, error)) return false;

  std::vector<std::pair<String, String>> list;
  for (JsonObject e : doc.as<JsonArray>()) {
    list.push_back({e["entity_id"] | "", e["attributes"]["friendly_name"] | ""});
  }
  std::sort(list.begin(), list.end());
  JsonDocument o;
  JsonArray arr = o.to<JsonArray>();
  for (auto &p : list) {
    JsonObject x = arr.add<JsonObject>();
    x["id"] = p.first;
    x["name"] = p.second;
  }
  out = "";
  serializeJson(o, out);
  return true;
}

void ha_set_watch(const std::vector<HaWatch> &watch) {
  {
    Lock l;
    s_watch.clear();
    for (auto &w : watch) {
      auto &attrs = s_watch[w.entity];
      if (w.attr.length() && std::find(attrs.begin(), attrs.end(), w.attr) == attrs.end()) attrs.push_back(w.attr);
    }
    // Les entites deja connues seront renvoyees a l'interface
    for (auto &e : s_ents) e.second.dirty = true;
  }
  if (s_task) xTaskNotifyGive(s_task);
}

void ha_call(const String &service, const String &json_body) {
  {
    Lock l;
    s_calls.push_back({service, json_body});
  }
  if (s_task) xTaskNotifyGive(s_task);
}

void ha_take_updates(std::vector<HaUpdate> &out) {
  out.clear();
  Lock l;
  for (auto &e : s_ents) {
    if (!e.second.dirty || !s_watch.count(e.first)) continue;
    e.second.dirty = false;
    out.push_back({e.first, e.second.found, e.second.state, e.second.attrs});
  }
}

HaStatus ha_status() {
  Lock l;
  return s_status;
}

String ha_status_text() {
  Lock l;
  return s_status_msg;
}
