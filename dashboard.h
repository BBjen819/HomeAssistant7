// Tableau de bord : construit les pages LVGL a partir du layout cree dans le
// designer web et les relie a Home Assistant.
// Un appui long (~3 s) sur le fond d'une page appelle le callback de sortie.
#pragma once
#include <lvgl.h>

void dashboard_set_exit_cb(void (*cb)());
bool dashboard_active();

bool dashboard_show();                     // Construit et affiche la 1re page
void dashboard_leave(lv_obj_t *next_scr);  // Affiche next_scr et detruit les pages
void dashboard_reload();                   // Reconstruit (layout modifie)
void dashboard_loop();                     // Applique les etats Home Assistant
