#include "lcd_panel.h"
#include "board_config.h"
#include <string.h>
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"

static esp_lcd_panel_handle_t s_panel = nullptr;
static volatile uint32_t s_frame_count = 0;
static uint32_t s_meas_t0 = 0, s_meas_n0 = 0;
static float s_meas_hz = 0;

// Appele (en ISR) a chaque image complete : sert uniquement a la mesure
static bool IRAM_ATTR on_frame_done(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  s_frame_count = s_frame_count + 1;
  return false;
}

bool lcd_init() {
  esp_lcd_rgb_panel_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));

  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = LCD_PCLK_HZ;
  cfg.timings.h_res = LCD_H_RES;
  cfg.timings.v_res = LCD_V_RES;
  cfg.timings.hsync_pulse_width = LCD_HSYNC_PULSE;
  cfg.timings.hsync_back_porch = LCD_HSYNC_BACK;
  cfg.timings.hsync_front_porch = LCD_HSYNC_FRONT;
  cfg.timings.vsync_pulse_width = LCD_VSYNC_PULSE;
  cfg.timings.vsync_back_porch = LCD_VSYNC_BACK;
  cfg.timings.vsync_front_porch = LCD_VSYNC_FRONT;
  cfg.timings.flags.pclk_active_neg = 1;

  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 1;
  cfg.bounce_buffer_size_px = LCD_H_RES * LCD_BOUNCE_LINES;
  cfg.dma_burst_size = 64;

  cfg.hsync_gpio_num = PIN_LCD_HSYNC;
  cfg.vsync_gpio_num = PIN_LCD_VSYNC;
  cfg.de_gpio_num = PIN_LCD_DE;
  cfg.pclk_gpio_num = PIN_LCD_PCLK;
  cfg.disp_gpio_num = -1;

  const int data_pins[16] = {
    PIN_LCD_B0, PIN_LCD_B1, PIN_LCD_B2, PIN_LCD_B3, PIN_LCD_B4,
    PIN_LCD_G0, PIN_LCD_G1, PIN_LCD_G2, PIN_LCD_G3, PIN_LCD_G4, PIN_LCD_G5,
    PIN_LCD_R0, PIN_LCD_R1, PIN_LCD_R2, PIN_LCD_R3, PIN_LCD_R4,
  };
  for (int i = 0; i < 16; i++) cfg.data_gpio_nums[i] = data_pins[i];

  cfg.flags.fb_in_psram = 1;

  esp_err_t err = esp_lcd_new_rgb_panel(&cfg, &s_panel);
  if (err != ESP_OK) {
    Serial.printf("[LCD] esp_lcd_new_rgb_panel a echoue : %s\n", esp_err_to_name(err));
    return false;
  }

  esp_lcd_rgb_panel_event_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.on_frame_buf_complete = on_frame_done;
  esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr);

  esp_lcd_panel_reset(s_panel);
  esp_lcd_panel_init(s_panel);

  s_meas_t0 = millis();
  s_meas_n0 = s_frame_count;
  Serial.printf("[LCD] %dx%d, PCLK %.1f MHz, theorique %.1f Hz, bounce %d lignes, 1 framebuffer\n",
                LCD_H_RES, LCD_V_RES, LCD_PCLK_HZ / 1e6, lcd_refresh_hz_theoretical(), LCD_BOUNCE_LINES);
  return true;
}

void lcd_draw(int x1, int y1, int x2, int y2, const void *pixels) {
  if (!s_panel) return;
  esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2 + 1, y2 + 1, pixels);
}

float lcd_refresh_hz() {
  uint32_t now = millis();
  uint32_t dt = now - s_meas_t0;
  if (dt >= 1000) {
    uint32_t n = s_frame_count;
    s_meas_hz = (n - s_meas_n0) * 1000.0f / dt;
    s_meas_t0 = now;
    s_meas_n0 = n;
  }
  return s_meas_hz;
}

float lcd_refresh_hz_theoretical() {
  float htotal = LCD_H_RES + LCD_HSYNC_PULSE + LCD_HSYNC_BACK + LCD_HSYNC_FRONT;
  float vtotal = LCD_V_RES + LCD_VSYNC_PULSE + LCD_VSYNC_BACK + LCD_VSYNC_FRONT;
  return LCD_PCLK_HZ / (htotal * vtotal);
}
