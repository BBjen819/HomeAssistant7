#include "gt911.h"
#include "board_config.h"
#include <Wire.h>

static const uint16_t GT911_REG_CONFIG     = 0x8047;
static const uint16_t GT911_REG_PRODUCT_ID = 0x8140;
static const uint16_t GT911_REG_STATUS     = 0x814E;
static const uint16_t GT911_REG_POINT1     = 0x8150;

static uint8_t  s_addr = 0x5D;
static bool     s_pressed = false;
static uint16_t s_x = 0, s_y = 0;

static bool gt_read(uint16_t reg, uint8_t *buf, size_t len) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(s_addr, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static bool gt_write(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(s_addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool gt911_begin() {
  uint8_t id[4] = {0};
  const uint8_t candidates[] = {0x5D, 0x14};
  for (uint8_t a : candidates) {
    s_addr = a;
    if (gt_read(GT911_REG_PRODUCT_ID, id, 4)) {
      uint8_t cfg[5] = {0};
      gt_read(GT911_REG_CONFIG, cfg, 5);
      uint16_t xmax = cfg[1] | (cfg[2] << 8);
      uint16_t ymax = cfg[3] | (cfg[4] << 8);
      Serial.printf("[GT911] Detecte a 0x%02X, ID \"%c%c%c%c\", config %ux%u, version cfg 0x%02X\n",
                    a, id[0], id[1], id[2], id[3] ? id[3] : ' ', xmax, ymax, cfg[0]);
      gt_write(GT911_REG_STATUS, 0);
      return true;
    }
  }
  Serial.println("[GT911] Non detecte !");
  return false;
}

bool gt911_read(uint16_t *x, uint16_t *y) {
  uint8_t status;
  if (gt_read(GT911_REG_STATUS, &status, 1) && (status & 0x80)) {
    uint8_t points = status & 0x0F;
    if (points > 0 && points <= 5) {
      uint8_t p[4];
      if (gt_read(GT911_REG_POINT1, p, 4)) {
        s_x = p[0] | (p[1] << 8);
        s_y = p[2] | (p[3] << 8);
        if (s_x >= LCD_H_RES) s_x = LCD_H_RES - 1;
        if (s_y >= LCD_V_RES) s_y = LCD_V_RES - 1;
        s_pressed = true;
      }
    } else {
      s_pressed = false;
    }
    gt_write(GT911_REG_STATUS, 0);  // Acquitte la lecture
  }
  *x = s_x;
  *y = s_y;
  return s_pressed;
}
