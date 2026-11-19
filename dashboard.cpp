#include "dashboard.h"
#include "board_config.h"
#include "ha_client.h"
#include "web_config.h"
#include "backlight.h"
#include "battery.h"
#include "camera.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

// Les styles reproduisent exactement l'apercu du designer (web/index.html) :
// toute modification de rendu doit etre faite des deux cotes.

struct Bind {
  String type, entity, attr, action, mode, target, valueType, prefix, suffix;
  String field;          // Parametre numerique envoye par un curseur (ex. "position")
  float factor = 1.0f;   // Valeur HA = valeur du curseur x facteur (ex. 0,01 pour volume_level)
  int decimals = 0;
  int32_t min = 0, max = 100;
  lv_obj_t *obj = nullptr;
  lv_obj_t *lbl = nullptr;   // Libelle de valeur (jauge) ou message (camera)
  // Camera
  int cam = -1;              // Emplacement camera.cpp
  lv_obj_t *img = nullptr;
  lv_obj_t *page = nullptr;  // Ecran (page) qui contient le widget
  uint32_t interval = 1000;
  bool full = true;          // Plein ecran au toucher
};

struct Page {
  String id;
  lv_obj_t *scr;
};

static std::vector<Bind> s_binds;
static std::vector<Page> s_pages;
static bool s_active = false;
static void (*s_exit_cb)() = nullptr;
static lv_obj_t *s_dot = nullptr;
static int s_long_count = 0;
static uint32_t s_last_dot = 0;
static uint32_t s_bl_version = 0;
static uint32_t s_batt_version = 0;
static std::vector<size_t> s_cams;   // Index des widgets camera dans s_binds

// Camera en plein ecran (ouverte en touchant un widget camera)
static lv_obj_t *s_full = nullptr, *s_full_img = nullptr, *s_full_lbl = nullptr;
static int s_full_cam = -1;

static void solid_bg(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel = 0);

// ---------------------------------------------------------------------------
// Aides
// ---------------------------------------------------------------------------
static const lv_font_t *font_for(int size) {
  static const struct { int s; const lv_font_t *f; } F[] = {
    {12, &lv_font_montserrat_12}, {14, &lv_font_montserrat_14}, {16, &lv_font_montserrat_16},
    {18, &lv_font_montserrat_18}, {20, &lv_font_montserrat_20}, {22, &lv_font_montserrat_22},
    {24, &lv_font_montserrat_24}, {28, &lv_font_montserrat_28}, {32, &lv_font_montserrat_32},
    {36, &lv_font_montserrat_36}, {40, &lv_font_montserrat_40}, {48, &lv_font_montserrat_48},
  };
  const lv_font_t *best = F[0].f;
  int bd = 1000;
  for (auto &e : F) {
    int d = abs(e.s - size);
    if (d < bd) { bd = d; best = e.f; }
  }
  return best;
}

static lv_color_t color_of(const char *hex, uint32_t def) {
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return lv_color_hex(def);
  return lv_color_hex(strtoul(hex + 1, nullptr, 16));
}

static uint32_t hex_of(const char *hex, uint32_t def) {
  if (!hex || hex[0] != '#' || strlen(hex) < 7) return def;
  return strtoul(hex + 1, nullptr, 16);
}

static int gi(JsonObject o, const char *k, int def) {
  JsonVariant v = o[k];
  return v.isNull() ? def : (int)lroundf(v.as<float>());
}

static String gs(JsonObject o, const char *k, const char *def = "") {
  const char *s = o[k] | def;
  return String(s);
}

// Les polices Montserrat de LVGL ne couvrent que l'ASCII (+ ° et •) :
// on retire les accents (meme table que le designer, pour un apercu exact).
static String deaccent(const String &in) {
  static const char LATIN1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
  String out;
  const uint8_t *p = (const uint8_t *)in.c_str();
  while (*p) {
    uint32_t cp;
    int n;
    if (*p < 0x80) { cp = *p; n = 1; }
    else if ((*p & 0xE0) == 0xC0 && p[1]) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); n = 2; }
    else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
    else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { cp = 0xFFFF; n = 4; }
    else { cp = '?'; n = 1; }

    if (cp < 0x80) out += (char)cp;
    else if (cp == 0xB0 || cp == 0x2022) out.concat((const char *)p, n);   // ° et • existent
    else if (cp >= 0xC0 && cp <= 0xFF) out += LATIN1[cp - 0xC0];
    else if (cp == 0x152) out += "OE";
    else if (cp == 0x153) out += "oe";
    else if (cp == 0x2018 || cp == 0x2019) out += '\'';
    else if (cp == 0xAB || cp == 0xBB || cp == 0x201C || cp == 0x201D) out += '"';
    else if (cp == 0x2013 || cp == 0x2014) out += '-';
    else if (cp == 0xA0) out += ' ';
    else out += '?';
    p += n;
  }
  return out;
}

static String domain_of(const String &entity) {
  int d = entity.indexOf('.');
  return d > 0 ? entity.substring(0, d) : String();
}

// Meme logique que defaultAction() du designer
static String default_action(const Bind &b) {
  String d = domain_of(b.entity);
  if (b.type == "slider") return d == "light" ? "light.turn_on" : (d.length() ? d + ".set_value" : "");
  if (b.type == "action") {
    if (d == "scene") return "scene.turn_on";
    if (d == "script") return "script.turn_on";
    if (d == "button") return "button.press";
    if (d == "input_button") return "input_button.press";
    if (d == "automation") return "automation.trigger";
    return "homeassistant.toggle";
  }
  static const char *TOGGLABLE[] = {"light", "switch", "fan", "input_boolean", "cover", "media_player", "climate"};
  for (auto t : TOGGLABLE)
    if (d == t) return d + ".toggle";
  return "homeassistant.toggle";
}

static String format_value(const Bind &b, const String &raw, bool with_prefix) {
  String pre = with_prefix ? b.prefix : String();
  if (b.valueType == "text") return deaccent(pre + raw + b.suffix);
  char *end = nullptr;
  float f = strtof(raw.c_str(), &end);
  if (raw.isEmpty() || end == raw.c_str()) return deaccent(pre + "--" + b.suffix);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.*f", max(0, b.decimals), f);
  return deaccent(pre + buf + b.suffix);
}

// Valeur HA -> echelle du widget (inverse du facteur du curseur)
static int32_t numeric(const String &raw, int32_t def, float factor = 1.0f) {
  char *end = nullptr;
  float f = strtof(raw.c_str(), &end);
  return (raw.isEmpty() || end == raw.c_str()) ? def : (int32_t)lroundf(f / factor);
}

// ---------------------------------------------------------------------------
// Evenements
// ---------------------------------------------------------------------------
static void call_service(const Bind &b, const char *num_key = nullptr, double num = 0) {
  String service = b.action.length() ? b.action : default_action(b);
  if (service.isEmpty()) return;
  JsonDocument d;
  if (b.entity.length()) d["entity_id"] = b.entity;
  if (num_key) {
    if (b.factor == 1.0f) d[num_key] = (int32_t)lround(num);
    else d[num_key] = num;
  }
  String body;
  serializeJson(d, body);
  ha_call(service, body);
}

static void show_page(const String &id) {
  for (auto &p : s_pages) {
    if (p.id == id) {
      lv_screen_load(p.scr);
      return;
    }
  }
}

// Entites locales (pas dans Home Assistant) :
//   screen.brightness  luminosite 0..100
//   screen.backlight   retroeclairage on/off
static bool is_local(const String &entity) { return entity.startsWith("screen."); }

static void local_action(const Bind &b) {
  if (b.type == "slider") {
    backlight_set(max<int32_t>(1, lv_slider_get_value(b.obj)));   // Le curseur n'eteint jamais l'ecran
  } else if (b.type == "toggle" || b.type == "switch") {
    bool on = lv_obj_has_state(b.obj, LV_STATE_CHECKED);
    backlight_set(on ? max(backlight_last_on(), 5) : 0);
  } else if (b.type == "action") {
    backlight_set(backlight_is_off() ? max(backlight_last_on(), 5) : 0);
  }
}

// ---------------------------------------------------------------------------
// Cameras
// ---------------------------------------------------------------------------
// Message (chargement, erreur) affiche au centre d'une camera
static lv_obj_t *cam_message(lv_obj_t *parent) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0xC8CDD6), 0);
  lv_obj_set_style_bg_color(l, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_60, 0);
  lv_obj_set_style_pad_hor(l, 8, 0);
  lv_obj_set_style_pad_ver(l, 4, 0);
  lv_obj_set_style_radius(l, 6, 0);
  lv_label_set_text(l, "Camera...");
  lv_obj_center(l);
  return l;
}

static lv_obj_t *cam_image(lv_obj_t *parent, int w, int h) {
  lv_obj_t *img = lv_image_create(parent);
  lv_obj_set_pos(img, 0, 0);
  lv_obj_set_size(img, w, h);
  lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
  return img;
}

// Applique la derniere image (ou erreur) recue pour une camera
static void cam_poll_into(int cam, lv_obj_t *img, lv_obj_t *lbl) {
  const lv_image_dsc_t *dsc = nullptr;
  String err;
  int r = camera_poll(cam, &dsc, err);
  if (r == 1) {
    lv_image_cache_drop(dsc);
    lv_image_set_src(img, dsc);
    if (lv_obj_has_flag(img, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    if (!lv_obj_has_flag(lbl, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
  } else if (r == -1) {
    // Le message reste par-dessus la derniere image : on voit qu'elle est figee
    lv_label_set_text(lbl, err.c_str());
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(lbl);
  }
}

// Oublie la camera plein ecran (son objet est detruit avec sa page)
static void fullscreen_forget() {
  if (s_full_cam >= 0) camera_close(s_full_cam);
  s_full_cam = -1;
  s_full = s_full_img = s_full_lbl = nullptr;
}

static void fullscreen_event_cb(lv_event_t *) {
  if (!s_full) return;
  lv_obj_delete_async(s_full);
  fullscreen_forget();
}

static void fullscreen_open(const Bind &b) {
  if (s_full || b.entity.isEmpty()) return;
  s_full = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(s_full);
  lv_obj_set_pos(s_full, 0, 0);
  lv_obj_set_size(s_full, LCD_H_RES, LCD_V_RES);
  solid_bg(s_full, lv_color_hex(0x000000));
  lv_obj_remove_flag(s_full, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_full, LV_OBJ_FLAG_CLICKABLE);
  s_full_lbl = cam_message(s_full);
  s_full_img = cam_image(s_full, LCD_H_RES, LCD_V_RES);
  lv_obj_add_event_cb(s_full, fullscreen_event_cb, LV_EVENT_CLICKED, nullptr);
  s_full_cam = camera_open(b.entity, LCD_H_RES, LCD_V_RES, b.interval, CAM_CONTAIN, 0x000000);
  if (s_full_cam < 0) lv_label_set_text(s_full_lbl, "Memoire insuffisante");
}

static void cameras_close() {
  fullscreen_forget();
  camera_close_all();
  s_cams.clear();
}

static void cameras_loop() {
  lv_obj_t *act = lv_screen_active();
  bool lit = !backlight_is_off();
  for (size_t i : s_cams) {
    Bind &b = s_binds[i];
    if (b.cam < 0) continue;
    // Seules les cameras visibles sont telechargees
    camera_set_active(b.cam, lit && !s_full && b.page == act);
    cam_poll_into(b.cam, b.img, b.lbl);
  }
  if (s_full_cam >= 0) {
    camera_set_active(s_full_cam, lit);
    cam_poll_into(s_full_cam, s_full_img, s_full_lbl);
  }
}

static void widget_event_cb(lv_event_t *e) {
  size_t idx = (size_t)(uintptr_t)lv_event_get_user_data(e);
  if (idx >= s_binds.size()) return;
  const Bind &b = s_binds[idx];
  lv_event_code_t code = lv_event_get_code(e);
  bool local = is_local(b.entity);

  if (b.type == "camera") {
    if (code == LV_EVENT_CLICKED && b.full) fullscreen_open(b);
  } else if (b.type == "action" && code == LV_EVENT_CLICKED) {
    if (b.mode == "page") show_page(b.target);
    else if (b.mode == "admin") { if (s_exit_cb) s_exit_cb(); }
    else if (local) local_action(b);
    else if (b.entity.length() || b.action.length()) call_service(b);
  } else if ((b.type == "toggle" && code == LV_EVENT_CLICKED) ||
             (b.type == "switch" && code == LV_EVENT_VALUE_CHANGED)) {
    if (local) local_action(b);
    else if (b.entity.length()) call_service(b);
  } else if (b.type == "slider") {
    if (local && code == LV_EVENT_VALUE_CHANGED) local_action(b);   // Reglage en direct
    else if (!local && code == LV_EVENT_RELEASED && b.entity.length()) {
      int32_t v = lv_slider_get_value(b.obj);
      String key = b.field.length() ? b.field : (domain_of(b.entity) == "light" ? String("brightness") : String("value"));
      call_service(b, key.c_str(), (double)v * b.factor);
    }
  }
}

static void screen_event_cb(lv_event_t *e) {
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) {
    s_long_count = 0;
  } else if (code == LV_EVENT_LONG_PRESSED_REPEAT) {
    // ~0,4 s + 25 x 0,1 s : environ 3 s d'appui sur le fond
    if (++s_long_count == 25 && s_exit_cb) s_exit_cb();
  }
}

// ---------------------------------------------------------------------------
// Construction des widgets
// ---------------------------------------------------------------------------
static void base_geometry(lv_obj_t *o, JsonObject w, bool auto_height = false) {
  lv_obj_set_pos(o, gi(w, "x", 0), gi(w, "y", 0));
  lv_obj_set_width(o, max(1, gi(w, "w", 100)));
  if (auto_height) lv_obj_set_height(o, LV_SIZE_CONTENT);
  else lv_obj_set_height(o, max(1, gi(w, "h", 40)));
}

static void solid_bg(lv_obj_t *o, lv_color_t c, lv_style_selector_t sel) {
  lv_obj_set_style_bg_color(o, c, sel);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, sel);
}

static lv_text_align_t align_of(const String &a) {
  if (a == "CENTER") return LV_TEXT_ALIGN_CENTER;
  if (a == "RIGHT") return LV_TEXT_ALIGN_RIGHT;
  return LV_TEXT_ALIGN_LEFT;
}

static void add_bind(Bind &b, lv_obj_t *obj, lv_event_code_t code) {
  b.obj = obj;
  s_binds.push_back(b);
  if (code != LV_EVENT_ALL) lv_obj_add_event_cb(obj, widget_event_cb, code, (void *)(uintptr_t)(s_binds.size() - 1));
}

static void create_widget(lv_obj_t *scr, JsonObject w) {
  String type = gs(w, "type");
  Bind b;
  b.type = type;
  b.entity = gs(w, "entity");
  b.entity.trim();
  b.attr = gs(w, "attribute");
  b.action = gs(w, "action");
  b.action.trim();
  b.mode = gs(w, "mode", "ha");
  b.target = gs(w, "target");
  b.valueType = gs(w, "valueType", "number");
  b.prefix = gs(w, "prefix");
  b.suffix = gs(w, "suffix");
  b.decimals = gi(w, "decimals", 0);
  b.field = gs(w, "field");
  b.factor = w["factor"] | 1.0f;
  if (b.factor == 0.0f) b.factor = 1.0f;
  b.min = gi(w, "min", 0);
  b.max = gi(w, "max", 100);
  if (b.max <= b.min) b.max = b.min + 1;

  const lv_font_t *font = font_for(gi(w, "size", 20));
  lv_color_t color = color_of(w["color"], 0xFFFFFF);
  lv_color_t bg = color_of(w["bg"], 0x2A2D34);
  lv_color_t on_bg = color_of(w["onBg"], 0xF2B233);
  int radius = gi(w, "radius", 0);
  int ww = gi(w, "w", 100), hh = gi(w, "h", 40);
  lv_obj_t *main_obj = nullptr;

  if (type == "tile") {
    lv_obj_t *o = lv_obj_create(scr);
    main_obj = o;
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    base_geometry(o, w);
    solid_bg(o, bg);
    lv_obj_set_style_radius(o, radius, 0);
    int bw = gi(w, "borderW", 0);
    if (bw > 0) {
      lv_obj_set_style_border_width(o, bw, 0);
      lv_obj_set_style_border_color(o, color_of(w["borderColor"], 0x454B57), 0);
      lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    }
  } else if (type == "label" || type == "value") {
    lv_obj_t *l = lv_label_create(scr);
    main_obj = l;
    base_geometry(l, w, true);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, align_of(gs(w, "align", "LEFT")), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    if (type == "label") {
      lv_label_set_text(l, deaccent(gs(w, "text")).c_str());
    } else {
      lv_label_set_text(l, format_value(b, "", true).c_str());
      add_bind(b, l, LV_EVENT_ALL);
    }
  } else if (type == "toggle" || type == "action") {
    lv_obj_t *btn = lv_button_create(scr);
    main_obj = btn;
    lv_obj_remove_style_all(btn);
    base_geometry(btn, w);
    solid_bg(btn, bg);
    lv_obj_set_style_radius(btn, radius, 0);
    lv_obj_set_style_text_font(btn, font, 0);
    lv_obj_set_style_text_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_70, LV_STATE_PRESSED);
    if (type == "toggle") {
      lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
      solid_bg(btn, on_bg, LV_STATE_CHECKED);
      lv_obj_set_style_text_color(btn, color_of(w["onColor"], 0x1A1A1A), LV_STATE_CHECKED);
    }
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, deaccent(gs(w, "text")).c_str());
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, max(1, ww - 12));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    add_bind(b, btn, LV_EVENT_CLICKED);
  } else if (type == "switch") {
    lv_obj_t *sw = lv_switch_create(scr);
    main_obj = sw;
    lv_obj_remove_style_all(sw);
    base_geometry(sw, w);
    solid_bg(sw, bg);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
    solid_bg(sw, on_bg, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    solid_bg(sw, color_of(w["knob"], 0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);   // Bouton de h-8 px, 4 px du bord
    add_bind(b, sw, LV_EVENT_VALUE_CHANGED);
  } else if (type == "slider") {
    lv_obj_t *s = lv_slider_create(scr);
    main_obj = s;
    lv_obj_remove_style_all(s);
    base_geometry(s, w);
    solid_bg(s, bg);
    lv_obj_set_style_radius(s, hh / 2, 0);
    solid_bg(s, on_bg, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, hh / 2, LV_PART_INDICATOR);
    solid_bg(s, color_of(w["knob"], 0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 5, LV_PART_KNOB);    // Bouton de h+10 px
    lv_slider_set_range(s, b.min, b.max);
    lv_slider_set_value(s, b.min, LV_ANIM_OFF);
    if (domain_of(b.entity) == "light" && b.attr.isEmpty()) b.attr = "brightness";
    add_bind(b, s, LV_EVENT_RELEASED);
    if (is_local(b.entity))
      lv_obj_add_event_cb(s, widget_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)(s_binds.size() - 1));
  } else if (type == "arc") {
    int t = max(1, gi(w, "thick", 10));
    lv_obj_t *a = lv_arc_create(scr);
    main_obj = a;
    lv_obj_remove_style_all(a);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    base_geometry(a, w);
    lv_obj_set_style_arc_color(a, bg, 0);
    lv_obj_set_style_arc_width(a, t, 0);
    lv_obj_set_style_arc_rounded(a, true, 0);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, 0);
    lv_obj_set_style_arc_color(a, on_bg, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, t, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(a, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_arc_set_bg_angles(a, 135, 45);
    lv_arc_set_range(a, b.min, b.max);
    lv_arc_set_value(a, b.min);
    if (w["showValue"] | false) {
      b.lbl = lv_label_create(a);
      lv_obj_set_style_text_font(b.lbl, font, 0);
      lv_obj_set_style_text_color(b.lbl, color, 0);
      lv_label_set_text(b.lbl, format_value(b, "", false).c_str());
      lv_obj_center(b.lbl);
    }
    add_bind(b, a, LV_EVENT_ALL);
  } else if (type == "bar") {
    lv_obj_t *bar = lv_bar_create(scr);
    main_obj = bar;
    lv_obj_remove_style_all(bar);
    base_geometry(bar, w);
    solid_bg(bar, bg);
    lv_obj_set_style_radius(bar, radius, 0);
    solid_bg(bar, on_bg, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, radius, LV_PART_INDICATOR);
    lv_bar_set_range(bar, b.min, b.max);
    lv_bar_set_value(bar, b.min, LV_ANIM_OFF);
    add_bind(b, bar, LV_EVENT_ALL);
  } else if (type == "camera") {
    uint32_t bg_rgb = hex_of(w["bg"], 0x000000);
    lv_obj_t *box = lv_obj_create(scr);
    main_obj = box;
    lv_obj_remove_style_all(box);
    base_geometry(box, w);
    solid_bg(box, lv_color_hex(bg_rgb));
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    b.full = w["full"] | true;
    if (!b.full) lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    b.interval = (uint32_t)max(100, gi(w, "interval", 1000));
    b.page = scr;
    b.lbl = cam_message(box);
    b.img = cam_image(box, max(1, ww), max(1, hh));
    if (b.entity.isEmpty()) {
      lv_label_set_text(b.lbl, "Aucune camera");
    } else {
      b.cam = camera_open(b.entity, max(1, ww), max(1, hh), b.interval, camera_fit_from(gs(w, "fit", "contain")), bg_rgb);
      if (b.cam < 0) lv_label_set_text(b.lbl, "Memoire insuffisante");
    }
    add_bind(b, box, b.full ? LV_EVENT_CLICKED : LV_EVENT_ALL);
    s_cams.push_back(s_binds.size() - 1);
  }
  (void)hh;

  // Rotation par pas de 90 degres autour du centre (meme rendu que rotate() CSS
  // dans le designer). LVGL ramene aussi les touchers dans le repere de l'objet.
  int rot = ((gi(w, "rot", 0) % 360) + 360) % 360;
  rot = (rot / 90) * 90;
  if (main_obj && rot) {
    lv_obj_set_style_transform_pivot_x(main_obj, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(main_obj, lv_pct(50), 0);
    lv_obj_set_style_transform_rotation(main_obj, rot * 10, 0);   // en 0,1 degre
  }
}

static bool build() {
  String txt;
  if (!layout_read(txt)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, txt)) {
    Serial.println("[DASH] Layout illisible");
    return false;
  }
  JsonArray pages = doc["pages"];
  if (pages.isNull() || pages.size() == 0) return false;

  // Les cameras de l'ancien affichage sont liberees : il est remplace avant
  // le prochain rendu (dashboard_reload) ou n'est plus affiche.
  cameras_close();
  s_binds.clear();
  s_binds.reserve(256);   // Les index servent de user_data : pas de reallocation
  for (JsonObject p : pages) {
    lv_obj_t *scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    solid_bg(scr, color_of(p["bg"], 0x15171C));
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, screen_event_cb, LV_EVENT_ALL, nullptr);
    s_pages.push_back({gs(p, "id"), scr});
    for (JsonObject w : p["widgets"].as<JsonArray>()) {
      if (s_binds.size() >= 256) break;
      create_widget(scr, w);
    }
  }

  std::vector<HaWatch> watch;
  for (auto &b : s_binds)
    if (b.entity.length() && !is_local(b.entity) && b.type != "camera") watch.push_back({b.entity, b.attr});
  s_bl_version = 0;   // Force l'envoi de l'etat local aux widgets
  s_batt_version = 0;
  ha_set_watch(watch);
  Serial.printf("[DASH] %u page(s), %u widget(s) relies, %u entite(s) surveillee(s), %u camera(s)\n",
                (unsigned)s_pages.size(), (unsigned)s_binds.size(), (unsigned)watch.size(), (unsigned)s_cams.size());
  return true;
}

static void destroy(std::vector<Page> &pages) {
  for (auto &p : pages) lv_obj_delete_async(p.scr);
  pages.clear();
}

// ---------------------------------------------------------------------------
// Mise a jour depuis Home Assistant
// ---------------------------------------------------------------------------
static void apply(Bind &b, const HaUpdate &u) {
  String raw = u.state;
  if (b.attr.length()) {
    auto it = u.attrs.find(b.attr);
    raw = it != u.attrs.end() ? it->second : String();
  }
  if (!u.found) raw = "";

  if (b.type == "value") {
    lv_label_set_text(b.obj, format_value(b, raw, true).c_str());
  } else if (b.type == "toggle" || b.type == "switch") {
    if (lv_obj_has_state(b.obj, LV_STATE_PRESSED)) return;
    bool on = u.state == "on" || u.state == "open";
    if (on) lv_obj_add_state(b.obj, LV_STATE_CHECKED);
    else lv_obj_remove_state(b.obj, LV_STATE_CHECKED);
  } else if (b.type == "slider") {
    if (lv_obj_has_state(b.obj, LV_STATE_PRESSED)) return;
    lv_slider_set_value(b.obj, numeric(raw, b.min, b.factor), LV_ANIM_ON);
  } else if (b.type == "arc") {
    lv_arc_set_value(b.obj, numeric(raw, b.min, b.factor));
    if (b.lbl) lv_label_set_text(b.lbl, format_value(b, raw, false).c_str());
  } else if (b.type == "bar") {
    lv_bar_set_value(b.obj, numeric(raw, b.min, b.factor), LV_ANIM_ON);
  }
}

static void update_dot() {
  if (!s_dot) {
    s_dot = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_dot);
    lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_dot, 12, 12);
    lv_obj_set_pos(s_dot, LCD_H_RES - 18, 6);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
  }
  // Pastille visible seulement en cas de probleme avec Home Assistant.
  // On ne touche a l'objet que si l'etat change (chaque modification force un
  // nouveau rendu de l'ecran).
  static int last = -1;
  HaStatus st = ha_status();
  int code;
  if (!s_active || st == HaStatus::OK) code = 0;
  else if (st == HaStatus::NOT_CONFIGURED || st == HaStatus::CONNECTING) code = 1;
  else code = 2;
  if (code == last) return;
  last = code;
  if (code == 0) {
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(s_dot, lv_color_hex(code == 1 ? 0xFFA726 : 0xEF5350), 0);
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void dashboard_set_exit_cb(void (*cb)()) { s_exit_cb = cb; }
bool dashboard_active() { return s_active; }

bool dashboard_show() {
  if (s_active) return true;
  if (!build()) {
    destroy(s_pages);
    cameras_close();
    s_binds.clear();
    return false;
  }
  lv_obj_t *old = lv_screen_active();
  lv_screen_load(s_pages[0].scr);
  if (old) lv_obj_delete_async(old);
  s_active = true;
  update_dot();
  return true;
}

void dashboard_leave(lv_obj_t *next_scr) {
  lv_screen_load(next_scr);
  cameras_close();
  destroy(s_pages);
  s_binds.clear();
  s_active = false;
  ha_set_watch({});
  update_dot();
}

void dashboard_reload() {
  if (!s_active) return;
  String current;
  lv_obj_t *act = lv_screen_active();
  for (auto &p : s_pages)
    if (p.scr == act) current = p.id;

  std::vector<Page> old;
  old.swap(s_pages);
  if (!build()) {
    // Layout invalide : on garde l'ancien affichage
    s_pages.swap(old);
    return;
  }
  lv_obj_t *target = s_pages[0].scr;
  for (auto &p : s_pages)
    if (p.id == current) target = p.scr;
  lv_screen_load(target);
  destroy(old);
}

void dashboard_loop() {
  if (!s_active) return;
  static std::vector<HaUpdate> ups;
  ha_take_updates(ups);
  // Etat local de l'ecran, presente comme des entites
  uint32_t v = backlight_version();
  if (v != s_bl_version) {
    s_bl_version = v;
    int pct = backlight_get();
    ups.push_back({"screen.brightness", true, String(pct), {}});
    ups.push_back({"screen.backlight", true, pct > 0 ? "on" : "off", {}});
  }
  uint32_t bv = battery_version();
  if (bv != s_batt_version) {
    s_batt_version = bv;
    bool ok = battery_valid();
    // Etat vide si la mesure est desactivee ou implausible -> "--" a l'ecran
    ups.push_back({"screen.battery", true, ok ? String(battery_percent()) : String(), {}});
    ups.push_back({"screen.battery_voltage", true, ok ? String(battery_voltage(), 2) : String(), {}});
  }
  for (auto &u : ups)
    for (auto &b : s_binds)
      if (b.entity == u.entity) apply(b, u);
  cameras_loop();

  if (millis() - s_last_dot > 1000) {
    s_last_dot = millis();
    update_dot();
  }
}
