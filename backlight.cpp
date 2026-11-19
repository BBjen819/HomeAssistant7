#include "backlight.h"
#include "board_config.h"
#include "ch422g.h"
#include <lvgl.h>
#include <Preferences.h>

static const int      MIN_VISIBLE = 5;       // Luminosite minimale au reveil / depuis l'ecran
static const uint32_t SAVE_DELAY_MS = 3000;  // Ecriture NVS differee (evite les sauts d'image)

static volatile int      s_target = 100;
static volatile uint32_t s_version = 1;
static int      s_applied = -1;
static int      s_last_on = 100;
static lv_obj_t *s_veil = nullptr;
static uint32_t s_changed_at = 0;
static bool     s_dirty = false;
static Preferences s_prefs;

void backlight_begin() {
  s_prefs.begin("display", false);
  int v = s_prefs.getInt("bright", 100);
  s_last_on = s_prefs.getInt("last_on", 100);
  s_target = constrain(v, 0, 100);
  if (s_last_on < MIN_VISIBLE) s_last_on = 100;

  s_veil = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_veil);
  lv_obj_remove_flag(s_veil, LV_OBJ_FLAG_CLICKABLE);   // Les touchers passent au travers
  lv_obj_set_pos(s_veil, 0, 0);
  lv_obj_set_size(s_veil, LCD_H_RES, LCD_V_RES);
  lv_obj_set_style_bg_color(s_veil, lv_color_black(), 0);
  lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
  backlight_loop();
  Serial.printf("[ECRAN] Luminosite %d %%\n", (int)s_target);
}

void backlight_loop() {
  int t = s_target;
  if (t != s_applied) {
    if (t == 0) {
      ch422g_set(EXIO_LCD_BL, false);
    } else {
      if (s_applied <= 0) ch422g_set(EXIO_LCD_BL, true);
      s_last_on = t;
      if (t >= 100) {
        lv_obj_add_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
      } else {
        // 99 % -> voile leger, 1 % -> voile presque opaque
        lv_opa_t opa = (lv_opa_t)((100 - t) * 235 / 99);
        lv_obj_set_style_bg_opa(s_veil, opa, 0);
        lv_obj_remove_flag(s_veil, LV_OBJ_FLAG_HIDDEN);
      }
    }
    s_applied = t;
    s_changed_at = millis();
    s_dirty = true;
  }
  if (s_dirty && millis() - s_changed_at > SAVE_DELAY_MS) {
    s_dirty = false;
    s_prefs.putInt("bright", s_applied);
    s_prefs.putInt("last_on", s_last_on);
  }
}

void backlight_save_now() {
  if (!s_dirty) return;
  s_dirty = false;
  s_prefs.putInt("bright", s_applied);
  s_prefs.putInt("last_on", s_last_on);
}

void backlight_set(int percent) {
  percent = constrain(percent, 0, 100);
  if (percent == s_target) return;
  s_target = percent;
  s_version = s_version + 1;
}

int backlight_get() { return s_target; }
bool backlight_is_off() { return s_target == 0; }
int backlight_last_on() { return s_last_on; }
uint32_t backlight_version() { return s_version; }

bool backlight_wake_on_touch() {
  if (s_target != 0) return false;
  backlight_set(max(s_last_on, MIN_VISIBLE));
  return true;
}
