#include "lvgl_port.h"
#include "board_config.h"
#include "lcd_panel.h"
#include "gt911.h"
#include "backlight.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

// Rendu LVGL en mode PARTIAL dans un tampon en SRAM interne (rapide, ne
// touche pas la PSRAM), puis copie des seules zones modifiees dans le
// framebuffer. Les modes DIRECT/FULL dessinent directement en PSRAM et
// provoquent des sauts d'image (bande passante PSRAM saturee).
static const int DRAW_BUF_LINES = 40;

static bool s_touch_pressed = false;
static int  s_touch_x = 0, s_touch_y = 0;

static uint32_t tick_cb() { return millis(); }

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  lcd_draw(area->x1, area->y1, area->x2, area->y2, px);
  lv_display_flush_ready(disp);
}

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  static bool s_swallow = false;   // Toucher qui a rallume l'ecran : ignore jusqu'au relachement
  uint16_t x, y;
  s_touch_pressed = gt911_read(&x, &y);
  s_touch_x = x;
  s_touch_y = y;
  data->point.x = x;
  data->point.y = y;
  if (s_touch_pressed && backlight_wake_on_touch()) s_swallow = true;
  if (!s_touch_pressed) s_swallow = false;
  data->state = (s_touch_pressed && !s_swallow) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

bool lvgl_port_init() {
  lv_init();
  lv_tick_set_cb(tick_cb);

  lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);

  size_t buf_bytes = LCD_H_RES * DRAW_BUF_LINES * 2;
  void *buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!buf) {
    Serial.println("[LVGL] Tampon en SRAM impossible, repli en PSRAM");
    buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM);
  }
  if (!buf) {
    Serial.println("[LVGL] Aucun tampon de rendu !");
    return false;
  }
  lv_display_set_buffers(disp, buf, nullptr, buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush_cb);

  lv_theme_t *th = lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE),
                                         lv_palette_main(LV_PALETTE_TEAL), true, LV_FONT_DEFAULT);
  lv_display_set_theme(disp, th);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touch_cb);

  Serial.printf("[LVGL] v%d.%d.%d prete, tampon %u octets\n",
                LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH, (unsigned)buf_bytes);
  return true;
}

bool lvgl_port_last_touch(int *x, int *y) {
  *x = s_touch_x;
  *y = s_touch_y;
  return s_touch_pressed;
}
