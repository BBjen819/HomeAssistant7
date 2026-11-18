// Liaison LVGL <-> ecran RGB + tactile GT911
#pragma once
#include <lvgl.h>

bool lvgl_port_init();

// Dernier etat tactile lu (pour l'affichage de test)
bool lvgl_port_last_touch(int *x, int *y);
