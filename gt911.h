// Pilote minimal du controleur tactile GT911 (un seul point)
#pragma once
#include <Arduino.h>

// A appeler apres la sequence de reset materielle. Retourne false si non detecte.
bool gt911_begin();

// Retourne true si un doigt est pose ; x/y en pixels ecran.
// Conserve le dernier etat si le GT911 n'a pas de nouvelles donnees.
bool gt911_read(uint16_t *x, uint16_t *y);
