#include "app_ui.h"
#include "board_config.h"
#include "lvgl_port.h"
#include "lcd_panel.h"
#include "dashboard.h"
#include "web_config.h"
#include "ha_client.h"
#include "backlight.h"
#include "battery.h"
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>

// Note : les polices lv_font_fr_XX (src/fonts) contiennent les accents (Latin-1),
// les textes a l'ecran peuvent donc etre accentues (fichiers en UTF-8).

// Duree de l'ecran "Connecte" (admin) au demarrage, avant le tableau de bord.
// Choix proposes sur l'ecran ; valeur enregistree en NVS (espace "ui", cle "admin_s").
static const uint8_t ADMIN_DELAYS[] = {1, 5, 10, 15};
static uint8_t s_admin_delay_s = 5;
static const uint32_t CONNECT_TIMEOUT_MS = 20000;
static const uint32_t SCAN_FRESH_MS      = 15000;

enum class Scr { NONE, LIST, PASSWORD, CONNECTING, CONNECTED, FAILED, DASHBOARD };

struct Net {
  String  ssid;
  int32_t rssi;
  bool    open;
  uint8_t channel;
};

static Scr s_screen  = Scr::NONE;
static Scr s_request = Scr::NONE;   // Changement d'ecran demande (traite dans la boucle)

static Preferences      s_prefs;
static std::vector<Net> s_nets;
static bool             s_scanning = false;
static uint32_t         s_scan_done_at = 0;

static String s_ssid, s_pass;        // Reseau cible
static bool   s_save_on_success = false;
static String s_fail_reason;

static volatile int     s_disc_count = 0;
static volatile uint8_t s_disc_reason = 0;

static uint32_t s_screen_start = 0;
static uint32_t s_last_info = 0;

// Widgets de l'ecran courant (remis a nullptr a chaque changement d'ecran)
static uint32_t s_last_log = 0;
static lv_obj_t *w_list, *w_list_status, *w_spinner, *w_scan_btn;  // liste
static lv_obj_t *w_ta, *w_kb, *w_eye_lbl;                     // mot de passe
static lv_obj_t *w_ip, *w_info, *w_cfg, *w_dash_btn;          // connecte
static lv_obj_t *w_bright, *w_bright_lbl;
static uint32_t s_bright_version = 0;
static bool s_auto_dash = true;   // Ouvre le tableau de bord apres la 1re connexion

static void load_admin_delay() {
  Preferences p;
  p.begin("ui", true);
  uint8_t v = p.getUChar("admin_s", 5);
  p.end();
  for (uint8_t d : ADMIN_DELAYS)
    if (d == v) s_admin_delay_s = v;
}

static void reset_widgets() {
  w_list = w_list_status = w_spinner = w_scan_btn = nullptr;
  w_ta = w_kb = w_eye_lbl = nullptr;
  w_ip = w_info = w_cfg = w_dash_btn = nullptr;
  w_bright = w_bright_lbl = nullptr;
}

static void go(Scr s) { s_request = s; }

// ---------------------------------------------------------------------------
// Aides LVGL
// ---------------------------------------------------------------------------
static lv_obj_t *new_screen() {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

static void load_screen(lv_obj_t *scr) {
  // Les pages du tableau de bord sont detruites par le module dashboard
  if (dashboard_active()) dashboard_leave(scr);
  else lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, true);
}

static lv_obj_t *make_header(lv_obj_t *scr, const char *title) {
  lv_obj_t *hdr = lv_obj_create(scr);
  lv_obj_set_size(hdr, LCD_H_RES, 68);
  lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_radius(hdr, 0, 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, lv_color_hex(0x1B2838), 0);
  lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lbl = lv_label_create(hdr);
  lv_label_set_text(lbl, title);
  lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl, 420);
  lv_obj_set_style_text_font(lbl, &lv_font_fr_28, 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
  return hdr;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud = nullptr) {
  lv_obj_t *btn = lv_button_create(parent);
  lv_obj_set_height(btn, 50);
  lv_obj_t *lbl = lv_label_create(btn);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
  return btn;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
  lv_obj_t *lbl = lv_label_create(parent);
  lv_label_set_text(lbl, text);
  lv_obj_set_style_text_font(lbl, font, 0);
  lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
  return lbl;
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  return o;
}

static int signal_quality(int32_t rssi) {
  return constrain(2 * (rssi + 100), 0, 100);
}

static uint32_t signal_color(int32_t rssi) {
  if (rssi >= -60) return 0x4CAF50;
  if (rssi >= -75) return 0xFFA726;
  return 0xEF5350;
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
static void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info) {
  // Appele depuis la tache Wi-Fi : on ne touche pas a LVGL ici.
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    s_disc_reason = info.wifi_sta_disconnected.reason;
    s_disc_count = s_disc_count + 1;
  }
}

static void start_scan() {
  if (s_scanning) return;
  WiFi.scanDelete();
  int r = WiFi.scanNetworks(true /*async*/, false /*caches*/);
  s_scanning = (r == WIFI_SCAN_RUNNING);
  Serial.printf("[WIFI] Scan lance (%d)\n", r);
}

static void fill_list();

static void poll_scan() {
  if (!s_scanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  s_scanning = false;
  s_scan_done_at = millis();
  s_nets.clear();

  if (n < 0) {
    Serial.printf("[WIFI] Echec du scan (%d)\n", n);
  } else {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;  // Reseau masque
      int32_t rssi = WiFi.RSSI(i);
      auto it = std::find_if(s_nets.begin(), s_nets.end(), [&](const Net &x) { return x.ssid == ssid; });
      if (it != s_nets.end()) {
        if (rssi > it->rssi) { it->rssi = rssi; it->channel = WiFi.channel(i); }
        continue;
      }
      s_nets.push_back({ssid, rssi, WiFi.encryptionType(i) == WIFI_AUTH_OPEN, (uint8_t)WiFi.channel(i)});
    }
    WiFi.scanDelete();
    std::sort(s_nets.begin(), s_nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
    Serial.printf("[WIFI] %u reseau(x) trouve(s)\n", (unsigned)s_nets.size());
  }
  if (s_screen == Scr::LIST) fill_list();
}

static void start_connect() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect();
    delay(100);
  }
  s_disc_count = 0;
  s_disc_reason = 0;
  Serial.printf("[WIFI] Connexion a \"%s\"...\n", s_ssid.c_str());
  WiFi.begin(s_ssid.c_str(), s_pass.length() ? s_pass.c_str() : nullptr);
}

static String reason_text(uint8_t r) {
  switch (r) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
      return "Mot de passe incorrect ?";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
      return "Réseau introuvable";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
      return "Association refusée par le point d'accès";
    default:
      return String("Échec (code ") + r + ")";
  }
}

// ---------------------------------------------------------------------------
// Liste des reseaux
// ---------------------------------------------------------------------------
static void on_scan_clicked(lv_event_t *) {
  start_scan();
  if (w_list_status) lv_label_set_text(w_list_status, "Recherche des réseaux…");
  if (w_spinner) lv_obj_remove_flag(w_spinner, LV_OBJ_FLAG_HIDDEN);
  if (w_scan_btn && s_scanning) lv_obj_add_state(w_scan_btn, LV_STATE_DISABLED);
}

static void on_net_clicked(lv_event_t *e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= (int)s_nets.size()) return;
  s_ssid = s_nets[idx].ssid;
  s_save_on_success = true;
  if (s_nets[idx].open) {
    s_pass = "";
    go(Scr::CONNECTING);
  } else {
    go(Scr::PASSWORD);
  }
}

static void fill_list() {
  if (!w_list) return;
  lv_obj_clean(w_list);
  if (w_spinner) lv_obj_add_flag(w_spinner, LV_OBJ_FLAG_HIDDEN);
  if (w_scan_btn) lv_obj_remove_state(w_scan_btn, LV_STATE_DISABLED);

  if (s_nets.empty()) {
    lv_label_set_text(w_list_status, "Aucun réseau trouvé. Appuyez sur Scanner.");
    return;
  }
  lv_label_set_text_fmt(w_list_status, "%u reseau(x) trouve(s) - touchez un reseau pour vous connecter",
                        (unsigned)s_nets.size());

  for (size_t i = 0; i < s_nets.size(); i++) {
    const Net &n = s_nets[i];
    lv_obj_t *btn = lv_list_add_button(w_list, LV_SYMBOL_WIFI, n.ssid.c_str());
    lv_obj_set_height(btn, 60);
    lv_obj_t *icon = lv_obj_get_child(btn, 0);
    if (icon) lv_obj_set_style_text_color(icon, lv_color_hex(signal_color(n.rssi)), 0);

    lv_obj_t *info = lv_label_create(btn);
    lv_label_set_text_fmt(info, "%s    %ld dBm  (%d%%)    ch %u",
                          n.open ? "Ouvert" : "Securise", (long)n.rssi, signal_quality(n.rssi), n.channel);
    lv_obj_set_style_text_font(info, &lv_font_fr_16, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0xB0BEC5), 0);
    lv_obj_add_event_cb(btn, on_net_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
  }
}

static void build_list() {
  lv_obj_t *scr = new_screen();
  lv_obj_t *hdr = make_header(scr, LV_SYMBOL_WIFI "  Réseaux Wi-Fi");
  w_scan_btn = make_button(hdr, LV_SYMBOL_REFRESH "  Scanner", on_scan_clicked);
  lv_obj_align(w_scan_btn, LV_ALIGN_RIGHT_MID, 0, 0);

  w_list_status = make_label(scr, "Recherche des réseaux…", &lv_font_fr_16, 0xB0BEC5);
  lv_obj_align(w_list_status, LV_ALIGN_TOP_LEFT, 16, 82);

  w_spinner = lv_spinner_create(scr);
  lv_obj_set_size(w_spinner, 32, 32);
  lv_obj_align(w_spinner, LV_ALIGN_TOP_RIGHT, -20, 76);

  w_list = lv_list_create(scr);
  lv_obj_set_size(w_list, LCD_H_RES - 20, LCD_V_RES - 68 - 46 - 10);
  lv_obj_align(w_list, LV_ALIGN_BOTTOM_MID, 0, -6);

  load_screen(scr);

  if (s_scanning) {
    lv_obj_add_state(w_scan_btn, LV_STATE_DISABLED);
  } else if (s_scan_done_at && millis() - s_scan_done_at < SCAN_FRESH_MS) {
    fill_list();
  } else {
    on_scan_clicked(nullptr);
  }
}

// ---------------------------------------------------------------------------
// Saisie du mot de passe
// ---------------------------------------------------------------------------
static void do_connect_from_ta() {
  if (!w_ta) return;
  s_pass = lv_textarea_get_text(w_ta);
  go(Scr::CONNECTING);
}

static void on_connect_clicked(lv_event_t *) { do_connect_from_ta(); }
static void on_back_to_list(lv_event_t *) { go(Scr::LIST); }

static void on_eye_clicked(lv_event_t *) {
  bool pw = lv_textarea_get_password_mode(w_ta);
  lv_textarea_set_password_mode(w_ta, !pw);
  lv_label_set_text(w_eye_lbl, pw ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void on_kb_event(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_READY) {
    do_connect_from_ta();
  } else if (code == LV_EVENT_CANCEL) {
    lv_obj_add_flag(w_kb, LV_OBJ_FLAG_HIDDEN);
  }
}

static void on_ta_clicked(lv_event_t *) {
  if (w_kb) lv_obj_remove_flag(w_kb, LV_OBJ_FLAG_HIDDEN);
}

static void build_password() {
  lv_obj_t *scr = new_screen();
  String title = String(LV_SYMBOL_WIFI "  ") + s_ssid;
  lv_obj_t *hdr = make_header(scr, title.c_str());
  lv_obj_t *back = make_button(hdr, LV_SYMBOL_LEFT "  Retour", on_back_to_list);
  lv_obj_align(back, LV_ALIGN_RIGHT_MID, 0, 0);

  w_ta = lv_textarea_create(scr);
  lv_textarea_set_one_line(w_ta, true);
  lv_textarea_set_password_mode(w_ta, true);
  lv_textarea_set_max_length(w_ta, 64);
  lv_textarea_set_placeholder_text(w_ta, "Mot de passe");
  lv_obj_set_width(w_ta, 480);
  lv_obj_align(w_ta, LV_ALIGN_TOP_LEFT, 16, 80);
  lv_obj_add_state(w_ta, LV_STATE_FOCUSED);
  lv_obj_add_event_cb(w_ta, on_ta_clicked, LV_EVENT_CLICKED, nullptr);
  if (s_ssid == s_prefs.getString("ssid", "")) {
    lv_textarea_set_text(w_ta, s_prefs.getString("pass", "").c_str());
  }

  lv_obj_t *eye = make_button(scr, "", on_eye_clicked);
  w_eye_lbl = lv_obj_get_child(eye, 0);
  lv_label_set_text(w_eye_lbl, LV_SYMBOL_EYE_OPEN);
  lv_obj_set_width(eye, 64);
  lv_obj_align_to(eye, w_ta, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

  lv_obj_t *ok = make_button(scr, LV_SYMBOL_OK "  Connecter", on_connect_clicked);
  lv_obj_set_width(ok, 190);
  lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -16, 80);

  w_kb = lv_keyboard_create(scr);
  lv_obj_set_size(w_kb, LCD_H_RES, 320);
  lv_obj_align(w_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_keyboard_set_textarea(w_kb, w_ta);
  lv_obj_add_event_cb(w_kb, on_kb_event, LV_EVENT_ALL, nullptr);

  load_screen(scr);
}

// ---------------------------------------------------------------------------
// Connexion en cours
// ---------------------------------------------------------------------------
static void on_cancel_connect(lv_event_t *) {
  WiFi.disconnect();
  go(Scr::LIST);
}

static void build_connecting() {
  lv_obj_t *scr = new_screen();
  make_header(scr, LV_SYMBOL_WIFI "  Connexion");

  lv_obj_t *sp = lv_spinner_create(scr);
  lv_obj_set_size(sp, 110, 110);
  lv_obj_align(sp, LV_ALIGN_CENTER, 0, -30);

  String msg = String("Connexion à \"") + s_ssid + "\"…";
  lv_obj_t *l = make_label(scr, msg.c_str(), &lv_font_fr_24, 0xFFFFFF);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, 60);

  lv_obj_t *c = make_button(scr, LV_SYMBOL_CLOSE "  Annuler", on_cancel_connect);
  lv_obj_set_width(c, 200);
  lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -24);

  load_screen(scr);
}

static void update_connecting() {
  if (WiFi.status() == WL_CONNECTED) {
    go(Scr::CONNECTED);
    return;
  }
  uint8_t r = s_disc_reason;
  bool fatal = (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_NO_AP_FOUND);
  if (s_disc_count >= 3 && fatal) {
    s_fail_reason = reason_text(r);
  } else if (millis() - s_screen_start > CONNECT_TIMEOUT_MS) {
    s_fail_reason = r ? reason_text(r) : String("Délai de connexion dépassé");
  } else {
    return;
  }
  Serial.printf("[WIFI] Echec : %s (raison %u, %d deconnexions)\n", s_fail_reason.c_str(), r, s_disc_count);
  WiFi.disconnect();
  go(Scr::FAILED);
}

// ---------------------------------------------------------------------------
// Connecte : affiche l'IP
// ---------------------------------------------------------------------------
static void on_change_network(lv_event_t *) {
  WiFi.disconnect();
  go(Scr::LIST);
}

static void on_forget_network(lv_event_t *) {
  s_prefs.remove("ssid");
  s_prefs.remove("pass");
  WiFi.disconnect();
  Serial.println("[WIFI] Identifiants effaces");
  go(Scr::LIST);
}

static void on_open_dashboard(lv_event_t *) { go(Scr::DASHBOARD); }

// Redemarrage : un premier appui demande confirmation pendant 4 s
static lv_timer_t *s_reboot_timer = nullptr;

static void reboot_now(lv_timer_t *) {
  backlight_save_now();
  Serial.println("[SYS] Redemarrage demande depuis l'ecran");
  Serial.flush();
  delay(100);
  ESP.restart();
}

static void reboot_disarm(lv_timer_t *t) {
  lv_obj_t *btn = (lv_obj_t *)lv_timer_get_user_data(t);
  s_reboot_timer = nullptr;
  if (lv_obj_is_valid(btn)) lv_label_set_text(lv_obj_get_child(btn, 0), LV_SYMBOL_POWER "  Redémarrer");
}

static void on_reboot(lv_event_t *e) {
  lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(e);
  lv_obj_t *lbl = lv_obj_get_child(btn, 0);
  if (!s_reboot_timer) {
    lv_label_set_text(lbl, LV_SYMBOL_WARNING "  Confirmer ?");
    s_reboot_timer = lv_timer_create(reboot_disarm, 4000, btn);
    lv_timer_set_repeat_count(s_reboot_timer, 1);
    return;
  }
  lv_timer_delete(s_reboot_timer);
  s_reboot_timer = nullptr;
  lv_label_set_text(lbl, "Redémarrage…");
  lv_obj_add_state(btn, LV_STATE_DISABLED);
  lv_timer_t *t = lv_timer_create(reboot_now, 300, nullptr);   // Laisse le texte s'afficher
  lv_timer_set_repeat_count(t, 1);
}

// Redessine tout l'ecran (fond, calques) : efface d'eventuels restes de l'ancien affichage
static void full_refresh(lv_timer_t *) {
  lv_obj_invalidate(lv_screen_active());
  lv_obj_invalidate(lv_layer_top());
  lv_obj_invalidate(lv_layer_sys());
}

static void on_admin_delay_changed(lv_event_t *e) {
  lv_obj_t *m = (lv_obj_t *)lv_event_get_target(e);
  uint32_t i = lv_buttonmatrix_get_selected_button(m);
  if (i >= sizeof(ADMIN_DELAYS)) return;
  s_auto_dash = false;   // L'utilisateur regle l'ecran : on reste ici
  if (ADMIN_DELAYS[i] == s_admin_delay_s) return;
  s_admin_delay_s = ADMIN_DELAYS[i];
  Preferences p;          // Ecriture unique, seulement sur action de l'utilisateur
  p.begin("ui", false);
  p.putUChar("admin_s", s_admin_delay_s);
  p.end();
  Serial.printf("[UI] Ecran admin au demarrage : %u s\n", s_admin_delay_s);
}

static void on_bright_changed(lv_event_t *) {
  if (!w_bright) return;
  s_auto_dash = false;   // L'utilisateur regle l'ecran : on ne part pas vers le tableau de bord
  backlight_set(lv_slider_get_value(w_bright));
}

// Synchronise le curseur si la luminosite change ailleurs (web, HA, tableau de bord)
static void sync_bright_slider() {
  if (!w_bright) return;
  uint32_t v = backlight_version();
  if (v == s_bright_version) return;
  s_bright_version = v;
  int pct = backlight_get();
  if (!lv_obj_has_state(w_bright, LV_STATE_PRESSED)) lv_slider_set_value(w_bright, max(pct, 5), LV_ANIM_OFF);
  lv_label_set_text_fmt(w_bright_lbl, "%d %%", pct);
}

// Chaque modification d'un objet LVGL force un nouveau rendu en PSRAM : on ne
// touche aux libelles que si leur contenu change reellement.
static void set_text_if_changed(lv_obj_t *lbl, const char *txt) {
  if (strcmp(lv_label_get_text(lbl), txt) != 0) lv_label_set_text(lbl, txt);
}

static int s_last_up = -1;

static void update_connected_info() {
  if (!w_ip || !w_info) return;
  int &last_up = s_last_up;
  bool up = WiFi.status() == WL_CONNECTED;
  String ip = WiFi.localIP().toString();
  char buf[512];
  if (up) {
    set_text_if_changed(w_ip, ip.c_str());
    snprintf(buf, sizeof(buf), LV_SYMBOL_SETTINGS "  Configuration :  http://%s/   ou   http://%s.local/",
             ip.c_str(), DEVICE_HOSTNAME);
    set_text_if_changed(w_cfg, buf);
  } else {
    set_text_if_changed(w_ip, "Reconnexion…");
  }
  if ((int)up != last_up) {
    last_up = up;
    lv_obj_set_style_text_color(w_ip, lv_color_hex(up ? 0x66BB6A : 0xFFA726), 0);
  }
  bool has_layout = layout_exists();
  int32_t rssi = WiFi.RSSI();
  char batt[48];
  if (battery_valid()) snprintf(batt, sizeof(batt), "%.2f V (%d %%)", battery_voltage(), battery_percent());
  else snprintf(batt, sizeof(batt), "%s", battery_enabled() ? "mesure invalide" : "non mesuree");
  snprintf(buf, sizeof(buf),
           "Réseau :  %s   -   %ld dBm (%d%%)   -   canal %ld\n"
           "Masque :  %s      Passerelle :  %s      DNS :  %s\n"
           "MAC :  %s      Nom d'hôte :  %s      Écran :  %d Hz\n"
           "Home Assistant :  %s      Batterie :  %s\n"
           "Tableau de bord :  %s",
           WiFi.SSID().c_str(), (long)rssi, signal_quality(rssi), (long)WiFi.channel(),
           WiFi.subnetMask().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
           WiFi.dnsIP().toString().c_str(), WiFi.macAddress().c_str(), WiFi.getHostname(),
           (int)lroundf(lcd_refresh_hz()), ha_status_text().c_str(), batt,
           has_layout ? "pret  (appui long de 3 s sur le fond pour revenir ici)"
                      : "aucun – créez-le depuis la page de configuration");
  set_text_if_changed(w_info, buf);
  if (w_dash_btn && lv_obj_has_state(w_dash_btn, LV_STATE_DISABLED) == has_layout) {
    if (has_layout) lv_obj_remove_state(w_dash_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(w_dash_btn, LV_STATE_DISABLED);
  }
}

static void build_connected() {
  lv_obj_t *scr = new_screen();
  lv_obj_t *hdr = make_header(scr, LV_SYMBOL_OK "  Connecté");

  // Mot de passe de la page de configuration (nouveau a chaque demarrage)
  String pw = String("Mot de passe :  ") + web_password();
  lv_obj_t *pwl = make_label(hdr, pw.c_str(), &lv_font_fr_28, 0xFFD54F);
  lv_obj_align(pwl, LV_ALIGN_RIGHT_MID, -4, 0);

  lv_obj_t *cap = make_label(scr, "Adresse IP", &lv_font_fr_20, 0x9E9E9E);
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 76);
  w_ip = make_label(scr, "", &lv_font_fr_48, 0x66BB6A);
  lv_obj_align(w_ip, LV_ALIGN_TOP_MID, 0, 100);

  w_cfg = make_label(scr, "", &lv_font_fr_20, 0x80DEEA);
  lv_obj_align(w_cfg, LV_ALIGN_TOP_MID, 0, 166);

  w_info = make_label(scr, "", &lv_font_fr_16, 0xE0E0E0);
  lv_obj_set_style_text_line_space(w_info, 8, 0);
  lv_obj_align(w_info, LV_ALIGN_TOP_LEFT, 40, 214);

  // Luminosite de l'ecran (5..100 % : le curseur n'eteint jamais l'ecran)
  lv_obj_t *bl = make_label(scr, LV_SYMBOL_IMAGE "  Luminosité", &lv_font_fr_20, 0xE0E0E0);
  lv_obj_align(bl, LV_ALIGN_TOP_LEFT, 24, 360);
  w_bright = lv_slider_create(scr);
  lv_obj_set_size(w_bright, 190, 18);
  lv_obj_align(w_bright, LV_ALIGN_TOP_LEFT, 180, 364);
  lv_slider_set_range(w_bright, 5, 100);
  lv_obj_add_event_cb(w_bright, on_bright_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  w_bright_lbl = make_label(scr, "", &lv_font_fr_20, 0xE0E0E0);
  lv_obj_align(w_bright_lbl, LV_ALIGN_TOP_LEFT, 388, 360);

  // Duree de cet ecran au demarrage avant l'ouverture du tableau de bord
  lv_obj_t *dl = make_label(scr, LV_SYMBOL_HOME "  Démarrage", &lv_font_fr_20, 0xE0E0E0);
  lv_obj_align(dl, LV_ALIGN_TOP_LEFT, 456, 360);
  static const char *DELAY_MAP[] = {"1 s", "5 s", "10 s", "15 s", ""};
  lv_obj_t *dm = lv_buttonmatrix_create(scr);
  lv_buttonmatrix_set_map(dm, DELAY_MAP);
  lv_buttonmatrix_set_button_ctrl_all(dm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
  lv_buttonmatrix_set_one_checked(dm, true);
  for (size_t i = 0; i < sizeof(ADMIN_DELAYS); i++)
    if (ADMIN_DELAYS[i] == s_admin_delay_s) lv_buttonmatrix_set_button_ctrl(dm, i, LV_BUTTONMATRIX_CTRL_CHECKED);
  lv_obj_set_size(dm, 188, 44);
  lv_obj_align(dm, LV_ALIGN_TOP_LEFT, 600, 350);
  lv_obj_set_style_pad_all(dm, 3, 0);
  lv_obj_set_style_pad_gap(dm, 3, 0);
  lv_obj_set_style_text_font(dm, &lv_font_fr_16, 0);
  lv_obj_add_event_cb(dm, on_admin_delay_changed, LV_EVENT_VALUE_CHANGED, nullptr);
  s_bright_version = 0;
  sync_bright_slider();

  w_dash_btn = make_button(scr, LV_SYMBOL_HOME "  Tableau de bord", on_open_dashboard);
  lv_obj_t *chg = make_button(scr, LV_SYMBOL_REFRESH "  Changer de réseau", on_change_network);
  lv_obj_t *fg = make_button(scr, LV_SYMBOL_TRASH "  Oublier ce réseau", on_forget_network);
  lv_obj_set_style_bg_color(fg, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_t *rb = make_button(scr, LV_SYMBOL_POWER "  Redémarrer", on_reboot);
  lv_obj_set_style_bg_color(rb, lv_palette_darken(LV_PALETTE_ORANGE, 2), 0);
  s_reboot_timer = nullptr;
  lv_obj_t *btns[4] = {w_dash_btn, chg, fg, rb};
  for (int i = 0; i < 4; i++) {
    lv_obj_set_width(btns[i], 184);
    lv_obj_set_style_text_font(btns[i], &lv_font_fr_16, 0);
    lv_obj_align(btns[i], LV_ALIGN_BOTTOM_MID, (2 * i - 3) * 98, -16);
  }

  s_last_up = -1;
  update_connected_info();
  load_screen(scr);
  // Rafraichissement complet tout de suite, puis une fois l'ancien affichage
  // (tableau de bord, fondu) entierement detruit.
  full_refresh(nullptr);
  lv_timer_t *t = lv_timer_create(full_refresh, 400, nullptr);
  lv_timer_set_repeat_count(t, 1);
}

// ---------------------------------------------------------------------------
// Echec
// ---------------------------------------------------------------------------
static void on_retry(lv_event_t *) { go(Scr::CONNECTING); }
static void on_edit_password(lv_event_t *) {
  s_save_on_success = true;
  go(Scr::PASSWORD);
}

static void build_failed() {
  lv_obj_t *scr = new_screen();
  make_header(scr, LV_SYMBOL_WARNING "  Échec de connexion");

  String msg = String("Impossible de se connecter à \"") + s_ssid + "\"";
  lv_obj_t *l1 = make_label(scr, msg.c_str(), &lv_font_fr_24, 0xFFFFFF);
  lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, 140);
  lv_obj_t *l2 = make_label(scr, s_fail_reason.c_str(), &lv_font_fr_28, 0xEF5350);
  lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, 190);

  lv_obj_t *b1 = make_button(scr, LV_SYMBOL_REFRESH "  Réessayer", on_retry);
  lv_obj_t *b2 = make_button(scr, LV_SYMBOL_EDIT "  Mot de passe", on_edit_password);
  lv_obj_t *b3 = make_button(scr, LV_SYMBOL_LIST "  Liste", on_back_to_list);
  lv_obj_t *btns[3] = {b1, b2, b3};
  for (int i = 0; i < 3; i++) {
    lv_obj_set_width(btns[i], 220);
    lv_obj_align(btns[i], LV_ALIGN_BOTTOM_MID, (i - 1) * 245, -40);
  }
  load_screen(scr);
}

// ---------------------------------------------------------------------------
// Machine d'etats
// ---------------------------------------------------------------------------
static void enter(Scr s) {
  reset_widgets();
  s_screen = s;
  s_screen_start = millis();
  switch (s) {
    case Scr::LIST:       build_list(); break;
    case Scr::PASSWORD:   build_password(); break;
    case Scr::CONNECTING: build_connecting(); start_connect(); break;
    case Scr::CONNECTED:
      Serial.printf("[WIFI] Connecte a \"%s\" - IP %s - RSSI %d dBm\n", WiFi.SSID().c_str(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      if (s_save_on_success) {
        s_prefs.putString("ssid", s_ssid);
        s_prefs.putString("pass", s_pass);
        Serial.println("[WIFI] Identifiants enregistres");
      }
      web_on_wifi_connected();
      build_connected();
      break;
    case Scr::FAILED:     build_failed(); break;
    case Scr::DASHBOARD:
      s_auto_dash = false;
      if (!dashboard_show()) {
        Serial.println("[DASH] Aucun layout valide");
        go(Scr::CONNECTED);
      }
      break;
    default: break;
  }
}

void app_ui_begin() {
  s_prefs.begin("wifi", false);
  load_admin_delay();

  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(on_wifi_event);

  dashboard_set_exit_cb([] { go(Scr::CONNECTED); });

  // Demarrage direct : reseau memorise -> connexion, sinon liste des reseaux
  String ssid = s_prefs.getString("ssid", "");
  if (ssid.length()) {
    s_ssid = ssid;
    s_pass = s_prefs.getString("pass", "");
    s_save_on_success = false;
    enter(Scr::CONNECTING);
  } else {
    start_scan();
    enter(Scr::LIST);
  }
}

void app_ui_loop() {
  if (s_request != Scr::NONE) {
    Scr r = s_request;
    s_request = Scr::NONE;
    enter(r);
  }

  poll_scan();

  if (millis() - s_last_log >= 5000) {
    s_last_log = millis();
    Serial.printf("[ETAT] Ecran %.1f Hz | Wi-Fi %s | SRAM libre %lu Ko | PSRAM libre %lu Ko\n",
                  lcd_refresh_hz(), WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "déconnecté",
                  (unsigned long)(ESP.getFreeHeap() >> 10), (unsigned long)(ESP.getFreePsram() >> 10));
  }

  // Layout enregistre depuis la page web : affichage immediat
  if (web_layout_changed()) {
    if (s_screen == Scr::DASHBOARD) dashboard_reload();
    else if (s_screen == Scr::CONNECTED) go(Scr::DASHBOARD);
  }

  switch (s_screen) {
    case Scr::CONNECTING: update_connecting(); break;
    case Scr::CONNECTED:
      sync_bright_slider();
      if (millis() - s_last_info > 2000) {
        s_last_info = millis();
        update_connected_info();
      }
      if (s_auto_dash && millis() - s_screen_start > s_admin_delay_s * 1000UL) {
        s_auto_dash = false;
        if (layout_exists()) go(Scr::DASHBOARD);
      }
      break;
    case Scr::DASHBOARD:  dashboard_loop(); break;
    default: break;
  }
}
