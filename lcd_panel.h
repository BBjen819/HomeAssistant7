// Ecran RGB 800x480 via le peripherique LCD_CAM de l'ESP32-S3 (esp_lcd).
// Un seul framebuffer en PSRAM, lu par DMA a travers des tampons "bounce" en
// SRAM interne. LVGL dessine en SRAM interne et ne copie dans le framebuffer
// que les zones modifiees : c'est ce qui sollicite le moins la PSRAM.
#pragma once
#include <Arduino.h>

bool lcd_init();

// Copie un bloc RGB565 dans le framebuffer. Coordonnees inclusives.
void lcd_draw(int x1, int y1, int x2, int y2, const void *pixels);

// Taux de rafraichissement mesure (images reellement balayees par seconde)
float lcd_refresh_hz();
float lcd_refresh_hz_theoretical();
