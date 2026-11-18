#include "ch422g.h"
#include <Wire.h>

// Le CH422G n'a pas d'adresse I2C classique : l'"adresse" est la commande.
static const uint8_t CH422G_CMD_SYSTEM = 0x24;  // Parametres systeme
static const uint8_t CH422G_CMD_IO_OUT = 0x38;  // Ecriture des sorties IO0..7

static uint8_t s_state = 0;

static bool ch422g_cmd(uint8_t cmd, uint8_t data) {
  Wire.beginTransmission(cmd);
  Wire.write(data);
  return Wire.endTransmission() == 0;
}

bool ch422g_begin() {
  return ch422g_cmd(CH422G_CMD_SYSTEM, 0x01);  // IO_OE = 1 : IO0..7 en sorties
}

bool ch422g_write_all(uint8_t value) {
  s_state = value;
  return ch422g_cmd(CH422G_CMD_IO_OUT, value);
}

bool ch422g_set(uint8_t bit, bool level) {
  uint8_t v = level ? (s_state | (1u << bit)) : (s_state & ~(1u << bit));
  return ch422g_write_all(v);
}

uint8_t ch422g_state() { return s_state; }
