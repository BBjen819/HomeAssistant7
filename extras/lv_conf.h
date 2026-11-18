/*
 * Configuration LVGL 9.2 pour HomeAssistant7 (Waveshare ESP32-S3-Touch-LCD-7)
 *
 * Ce fichier doit etre copie A COTE du dossier de la bibliotheque lvgl :
 *     Documents/Arduino/libraries/lv_conf.h
 * (LVGL l'inclut via "../../lv_conf.h"). Seules les options modifiees sont
 * listees ; toutes les autres prennent la valeur par defaut de LVGL.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* malloc/memcpy/sprintf de la libc : les grosses allocations vont en PSRAM */
#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE

#define LV_DEF_REFR_PERIOD 20   /* ms */
#define LV_DPI_DEF 130

/* Journal LVGL vers la console serie */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* Polices (ASCII + symboles, sans accents) */
/* Ces tailles sont celles proposees par le designer web (web/index.html) */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_20

/* Mettre a 1 pour afficher FPS / charge CPU en bas a droite */
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0

#define LV_BUILD_EXAMPLES 0

#endif /* LV_CONF_H */
