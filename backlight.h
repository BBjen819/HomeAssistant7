// Luminosite de l'ecran (0..100 %).
// Le retroeclairage de cette carte (CH422G EXIO2) est seulement marche/arret :
//   0 %       -> retroeclairage eteint (un toucher le rallume)
//   1..100 %  -> attenuation logicielle (voile noir semi-transparent LVGL)
// backlight_set() est sur depuis n'importe quelle tache ; l'application reelle
// (LVGL + I2C) se fait dans backlight_loop(), appele par la boucle principale.
#pragma once
#include <Arduino.h>

void backlight_begin();          // Apres lvgl_port_init()
void backlight_loop();

void backlight_set(int percent); // 0..100, enregistre en NVS (differe)
void backlight_save_now();       // Ecrit tout de suite la valeur en attente (avant un redemarrage)
int  backlight_get();
bool backlight_is_off();
int  backlight_last_on();        // Derniere valeur non nulle
uint32_t backlight_version();    // Incremente a chaque changement

// Appele par le tactile : si l'ecran est eteint, le rallume et retourne true
// (le toucher doit alors etre ignore jusqu'au relachement).
bool backlight_wake_on_touch();
