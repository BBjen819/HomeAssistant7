// Pilote minimal de l'extension d'E/S CH422G (sorties EXIO0..7)
#pragma once
#include <Arduino.h>

bool ch422g_begin();                    // Configure IO0..7 en sorties
bool ch422g_write_all(uint8_t value);   // Ecrit les 8 sorties d'un coup
bool ch422g_set(uint8_t bit, bool level);
uint8_t ch422g_state();
