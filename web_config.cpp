#include "i18n.h"
#include "web_config.h"
#include "board_config.h"
#include "ha_client.h"
#include "backlight.h"
#include "battery.h"
#include "camera.h"
#include "web_page.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <FFat.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <functional>
#include <vector>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

const char *DEVICE_HOSTNAME = "ha-panel-7";

static const char *LAYOUT_PATH = "/layout.json";
static const int FONT_SIZES[] = {12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 40, 48};

static WebServer s_server(80);
static SemaphoreHandle_t s_fs_mtx = nullptr;
static bool s_fs_ok = false;
static volatile bool s_layout_changed = false;
static bool s_mdns = false;

// ---------------------------------------------------------------------------
// Stockage du layout (FFat)
// ---------------------------------------------------------------------------
// IMPORTANT : tout acces a la flash (FFat, NVS) desactive le cache, donc la
// PSRAM ou se trouve le framebuffer -> l'image saute. On garde l'etat en RAM
// pour ne jamais toucher la flash periodiquement.
static volatile bool s_layout_present = false;

bool layout_exists() { return s_layout_present; }

bool layout_read(String &out) {
  if (!s_fs_ok) return false;
  xSemaphoreTake(s_fs_mtx, portMAX_DELAY);
  File f = FFat.open(LAYOUT_PATH, "r");
  bool ok = f && !f.isDirectory();
  if (ok) out = f.readString();
  if (f) f.close();
  xSemaphoreGive(s_fs_mtx);
  return ok && out.length() > 0;
}

static bool layout_write(const String &data) {
  if (!s_fs_ok) return false;
  xSemaphoreTake(s_fs_mtx, portMAX_DELAY);
  File f = FFat.open("/layout.tmp", "w");
  bool ok = f && f.print(data) == data.length();
  if (f) f.close();
  if (ok) {
    FFat.remove(LAYOUT_PATH);
    ok = FFat.rename("/layout.tmp", LAYOUT_PATH);
  }
  if (ok) s_layout_present = true;
  xSemaphoreGive(s_fs_mtx);
  return ok;
}

bool web_layout_changed() {
  bool c = s_layout_changed;
  s_layout_changed = false;
  return c;
}

// ---------------------------------------------------------------------------
// Gestionnaires HTTP
// ---------------------------------------------------------------------------
static void send_json(int code, JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  s_server.send(code, "application/json", out);
}

static void send_error(int code, const String &msg) {
  JsonDocument d;
  d["ok"] = false;
  d["message"] = msg;
  send_json(code, d);
}

static bool parse_body(JsonDocument &doc) {
  if (deserializeJson(doc, s_server.arg("plain"))) {
    send_error(400, TR("JSON invalide", "Invalid JSON"));
    return false;
  }
  return true;
}

static bool has_session();

// Langue choisie sur l'ecran admin, lue par les pages web avant leur script
// (route publique : la page de connexion en a besoin).
static void handle_lang_js() {
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send(200, "application/javascript", lang_en() ? "window.LANG='en';" : "window.LANG='fr';");
}

static void handle_index() {
  s_server.sendHeader("Cache-Control", "no-store");
  if (has_session()) s_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML, sizeof(INDEX_HTML) - 1);
  else s_server.send_P(200, "text/html; charset=utf-8", LOGIN_HTML, sizeof(LOGIN_HTML) - 1);
}

static void handle_info() {
  JsonDocument d;
  d["hostname"] = DEVICE_HOSTNAME;
  d["ip"] = WiFi.localIP().toString();
  d["ssid"] = WiFi.SSID();
  d["rssi"] = WiFi.RSSI();
  d["width"] = LCD_H_RES;
  d["height"] = LCD_V_RES;
  JsonArray fs = d["fonts"].to<JsonArray>();
  for (int s : FONT_SIZES) fs.add(s);
  d["fs_ok"] = s_fs_ok;
  d["heap_kb"] = ESP.getFreeHeap() / 1024;
  d["psram_kb"] = ESP.getFreePsram() / 1024;
  send_json(200, d);
}

static void handle_layout_get() {
  String txt;
  if (layout_read(txt)) s_server.send(200, "application/json", txt);
  else s_server.send(200, "application/json", "null");
}

static void handle_layout_post() {
  const String &body = s_server.arg("plain");
  JsonDocument doc;
  if (deserializeJson(doc, body)) return send_error(400, TR("JSON invalide", "Invalid JSON"));
  if (!doc["pages"].is<JsonArray>() || doc["pages"].size() == 0) return send_error(400, TR("Aucune page", "No page"));
  if (!s_fs_ok) return send_error(500, TR("Système de fichiers indisponible", "File system unavailable"));
  if (!layout_write(body)) return send_error(500, TR("Écriture impossible", "Write failed"));
  s_layout_changed = true;
  Serial.printf("[WEB] Layout enregistre (%u octets)\n", (unsigned)body.length());
  JsonDocument d;
  d["ok"] = true;
  send_json(200, d);
}

static void handle_ha_get() {
  String url;
  bool has_token;
  ha_get_config(url, has_token);
  JsonDocument d;
  d["url"] = url;
  d["has_token"] = has_token;
  HaStatus st = ha_status();
  d["status"] = st == HaStatus::OK ? "ok" : st == HaStatus::NOT_CONFIGURED ? "not_configured"
              : st == HaStatus::CONNECTING ? "connecting" : "error";
  d["message"] = ha_status_text();
  send_json(200, d);
}

static void handle_ha_post() {
  JsonDocument b;
  if (!parse_body(b)) return;
  String url = b["url"] | "";
  if (url.isEmpty()) return send_error(400, "URL requise");
  ha_set_config(url, b["token"] | "");
  JsonDocument d;
  d["ok"] = true;
  send_json(200, d);
}

static void handle_ha_test() {
  JsonDocument b;
  if (!parse_body(b)) return;
  String msg;
  bool ok = ha_test(b["url"] | "", b["token"] | "", msg);
  JsonDocument d;
  d["ok"] = ok;
  d["message"] = msg;
  send_json(200, d);
}

static void handle_ha_entities() {
  String out, err;
  if (ha_list_entities(out, err)) s_server.send(200, "application/json", out);
  else send_error(502, err);
}

static void handle_ha_services() {
  String out, err;
  if (ha_list_services(out, err)) s_server.send(200, "application/json", out);
  else send_error(502, err);
}

// Image d'une camera pour l'apercu du designer : /api/ha/camera?id=camera.x&w=400&h=225
static void handle_ha_camera() {
  String id = s_server.arg("id");
  bool ok = id.startsWith("camera.") && id.length() < 128;
  for (size_t i = 0; ok && i < id.length(); i++) {
    char c = id[i];
    ok = isalnum((unsigned char)c) || c == '_' || c == '.';
  }
  if (!ok) {
    send_error(400, TR("Entité caméra invalide", "Invalid camera entity"));
    return;
  }
  int w = constrain(s_server.arg("w").toInt(), 16, LCD_H_RES);
  int h = constrain(s_server.arg("h").toInt(), 16, LCD_V_RES);
  uint8_t *jpg = nullptr;
  size_t len = 0;
  String err;
  if (!camera_snapshot(id, w, h, &jpg, &len, err)) {
    send_error(502, err);
    return;
  }
  s_server.sendHeader("Cache-Control", "no-store");
  s_server.send_P(200, "image/jpeg", (const char *)jpg, len);
  free(jpg);
}

static void handle_ha_state() {
  String out, err;
  if (ha_get_state(s_server.arg("id"), out, err)) s_server.send(200, "application/json", out);
  else send_error(err == TR("Entité introuvable", "Entity not found") ? 404 : 502, err);
}

static void handle_brightness_get() {
  JsonDocument d;
  d["value"] = backlight_get();
  d["on"] = !backlight_is_off();
  send_json(200, d);
}

static void handle_battery_get() {
  JsonDocument d;
  d["enabled"] = battery_enabled();
  d["ratio"] = battery_ratio();
  d["pin"] = PIN_BATTERY_ADC;
  d["valid"] = battery_valid();
  if (battery_valid()) {
    d["voltage"] = roundf(battery_voltage() * 100) / 100;
    d["percent"] = battery_percent();
  }
  send_json(200, d);
}

// {"enabled": bool, "ratio": float}
static void handle_battery_post() {
  JsonDocument b;
  if (!parse_body(b)) return;
  battery_configure(b["enabled"] | battery_enabled(), b["ratio"] | battery_ratio());
  handle_battery_get();
}

// Accepte {"value": 0..100} et/ou {"on": true|false}
static void handle_brightness_post() {
  JsonDocument b;
  if (!parse_body(b)) return;
  if (b["value"].is<float>() || b["value"].is<int>()) {
    backlight_set((int)lroundf(b["value"].as<float>()));
  } else if (b["on"].is<bool>()) {
    backlight_set(b["on"].as<bool>() ? max(backlight_last_on(), 5) : 0);
  } else {
    return send_error(400, "Indiquez \"value\" (0-100) ou \"on\" (true/false)");
  }
  handle_brightness_get();
}

// ---------------------------------------------------------------------------
// Authentification
//  - Mot de passe : 6 chiffres aleatoires (RNG materiel), nouveau a chaque
//    demarrage, affiche sur l'ecran "Connecte".
//  - Session : jeton aleatoire de 128 bits dans un cookie HttpOnly.
//  - Anti-force brute : apres 5 echecs, blocage 30 s, double a chaque echec (max 10 min).
//  - Aucune cle API : seul un navigateur connecte avec le mot de passe peut
//    modifier l'ecran (Home Assistant ne peut pas le piloter).
// ---------------------------------------------------------------------------
static const char *COOKIE_NAME = "hapanel";
static const size_t MAX_SESSIONS = 8;

static String s_pin;
static std::vector<String> s_sessions;
static int s_failures = 0;
static uint32_t s_lock_until = 0;

static String random_hex(int bytes) {
  String s;
  for (int i = 0; i < bytes; i += 4) {
    uint32_t r = esp_random();
    char buf[9];
    snprintf(buf, sizeof(buf), "%08lx", (unsigned long)r);
    s += buf;
  }
  return s.substring(0, bytes * 2);
}

static String cookie_token() {
  String c = s_server.header("Cookie");
  String key = String(COOKIE_NAME) + "=";
  int i = c.indexOf(key);
  if (i < 0) return "";
  String t = c.substring(i + key.length());
  int e = t.indexOf(';');
  if (e >= 0) t = t.substring(0, e);
  t.trim();
  return t;
}

static bool has_session() {
  String t = cookie_token();
  if (t.length() != 32) return false;
  for (auto &s : s_sessions)
    if (s == t) return true;
  return false;
}

// Enregistre une route protegee
static void on_auth(const char *path, HTTPMethod method, std::function<void()> fn) {
  s_server.on(path, method, [fn] {
    if (has_session()) fn();
    else send_error(401, TR("Authentification requise", "Authentication required"));
  });
}

static void handle_login() {
  uint32_t now = millis();
  if ((int32_t)(s_lock_until - now) > 0) {
    return send_error(429, String(TR("Trop d'essais. Réessayez dans ", "Too many attempts. Try again in ")) + ((s_lock_until - now) / 1000 + 1) + " s.");
  }
  JsonDocument b;
  if (!parse_body(b)) return;
  String pin = b["pin"] | "";
  pin.trim();
  if (pin.length() == 6 && pin == s_pin) {
    s_failures = 0;
    String tok = random_hex(16);
    if (s_sessions.size() >= MAX_SESSIONS) s_sessions.erase(s_sessions.begin());
    s_sessions.push_back(tok);
    s_server.sendHeader("Set-Cookie", String(COOKIE_NAME) + "=" + tok + "; Path=/; HttpOnly; SameSite=Strict");
    JsonDocument d;
    d["ok"] = true;
    send_json(200, d);
    Serial.printf("[WEB] Connexion depuis %s\n", s_server.client().remoteIP().toString().c_str());
    return;
  }
  s_failures++;
  String msg = TR("Mot de passe incorrect.", "Wrong password.");
  if (s_failures >= 5) {
    uint32_t lock_s = min<uint32_t>(600, 30u << min(s_failures - 5, 5));
    s_lock_until = now + lock_s * 1000;
    msg += String(TR(" Blocage pendant ", " Locked for ")) + lock_s + " s.";
  }
  Serial.printf("[WEB] Echec de connexion (%d) depuis %s\n", s_failures, s_server.client().remoteIP().toString().c_str());
  send_error(401, msg);
}

static void handle_logout() {
  String t = cookie_token();
  for (size_t i = 0; i < s_sessions.size(); i++)
    if (s_sessions[i] == t) { s_sessions.erase(s_sessions.begin() + i); break; }
  s_server.sendHeader("Set-Cookie", String(COOKIE_NAME) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  JsonDocument d;
  d["ok"] = true;
  send_json(200, d);
}

const String &web_password() { return s_pin; }

static void web_task(void *) {
  for (;;) {
    s_server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

// ---------------------------------------------------------------------------
void web_begin() {
  s_fs_mtx = xSemaphoreCreateMutex();
  s_fs_ok = FFat.begin(true);   // Formate au premier demarrage
  s_layout_present = s_fs_ok && FFat.exists(LAYOUT_PATH);
  Serial.printf("[WEB] FFat %s (%lu Ko libres), layout %s\n", s_fs_ok ? "OK" : "INDISPONIBLE",
                s_fs_ok ? (unsigned long)(FFat.freeBytes() / 1024) : 0UL, s_layout_present ? "present" : "absent");

  // Mot de passe a 6 chiffres, uniforme sur 000000..999999, nouveau a chaque demarrage
  char pin[8];
  snprintf(pin, sizeof(pin), "%06lu", (unsigned long)(esp_random() % 1000000UL));
  s_pin = pin;
  Preferences p;
  p.begin("web", false);
  if (p.isKey("apikey")) p.remove("apikey");   // Ancienne cle API : effacee
  p.end();
  Serial.printf("[WEB] Mot de passe de cette session : %s\n", s_pin.c_str());

  const char *headers[] = {"Cookie"};
  s_server.collectHeaders(headers, 1);

  s_server.on("/", HTTP_GET, handle_index);
  s_server.on("/lang.js", HTTP_GET, handle_lang_js);   // Page de connexion si pas de session
  s_server.on("/api/login", HTTP_POST, handle_login);
  s_server.on("/api/logout", HTTP_POST, handle_logout);
  on_auth("/api/info", HTTP_GET, handle_info);
  on_auth("/api/layout", HTTP_GET, handle_layout_get);
  on_auth("/api/layout", HTTP_POST, handle_layout_post);
  on_auth("/api/ha", HTTP_GET, handle_ha_get);
  on_auth("/api/ha", HTTP_POST, handle_ha_post);
  on_auth("/api/ha/test", HTTP_POST, handle_ha_test);
  on_auth("/api/ha/entities", HTTP_GET, handle_ha_entities);
  on_auth("/api/ha/services", HTTP_GET, handle_ha_services);
  on_auth("/api/ha/state", HTTP_GET, handle_ha_state);
  on_auth("/api/ha/camera", HTTP_GET, handle_ha_camera);
  on_auth("/api/brightness", HTTP_GET, handle_brightness_get);
  on_auth("/api/brightness", HTTP_POST, handle_brightness_post);
  on_auth("/api/battery", HTTP_GET, handle_battery_get);
  on_auth("/api/battery", HTTP_POST, handle_battery_post);
  s_server.onNotFound([] { s_server.send(404, "text/plain", "Introuvable"); });
  s_server.begin();

  xTaskCreatePinnedToCore(web_task, "web", 12288, nullptr, 1, nullptr, 0);
  Serial.println("[WEB] Serveur demarre sur le port 80");
}

void web_on_wifi_connected() {
  if (s_mdns) return;
  s_mdns = MDNS.begin(DEVICE_HOSTNAME);
  if (s_mdns) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[WEB] mDNS : http://%s.local/\n", DEVICE_HOSTNAME);
  }
}
