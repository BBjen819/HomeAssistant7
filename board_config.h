// Brochage de la Waveshare ESP32-S3-Touch-LCD-7 (800x480, ST7262 RGB, GT911, CH422G)
#pragma once

// ---------- Ecran RGB parallele ----------
#define LCD_H_RES           800
#define LCD_V_RES           480
// Rafraichissement = PCLK / ((800+4+8+8) x (480+4+8+8)) = PCLK / 410 000
//   16 MHz -> 39 Hz (valeur Waveshare / ESPHome)
// NE PAS augmenter : a 21 MHz le controleur LCD doit lire 42 Mo/s en PSRAM et,
// des que LVGL ou le Wi-Fi sollicitent aussi la PSRAM, l'image saute.
// Valeur retenue apres essais sur la carte :
//   8 MHz  -> ecran blanc (la dalle ne synchronise plus)
//   12 MHz -> ~29 Hz, stable, pas de scintillement   <== retenu
//   16 MHz -> leger scintillement avec Wi-Fi + tableau de bord
//   21 MHz -> scintillement marque (PSRAM saturee)
#define LCD_PCLK_HZ         (12 * 1000 * 1000)
// Tampons "bounce" en SRAM interne (2 x 800 x N px x 2 octets). Doit diviser 480.
#define LCD_BOUNCE_LINES    10

#define LCD_HSYNC_PULSE     4
#define LCD_HSYNC_BACK      8
#define LCD_HSYNC_FRONT     8
#define LCD_VSYNC_PULSE     4
#define LCD_VSYNC_BACK      8
#define LCD_VSYNC_FRONT     8

#define PIN_LCD_DE          5
#define PIN_LCD_VSYNC       3
#define PIN_LCD_HSYNC       46
#define PIN_LCD_PCLK        7

// Ordre attendu par esp_lcd en RGB565 : B0..B4, G0..G5, R0..R4
#define PIN_LCD_B0 14
#define PIN_LCD_B1 38
#define PIN_LCD_B2 18
#define PIN_LCD_B3 17
#define PIN_LCD_B4 10
#define PIN_LCD_G0 39
#define PIN_LCD_G1 0
#define PIN_LCD_G2 45
#define PIN_LCD_G3 48
#define PIN_LCD_G4 47
#define PIN_LCD_G5 21
#define PIN_LCD_R0 1
#define PIN_LCD_R1 2
#define PIN_LCD_R2 42
#define PIN_LCD_R3 41
#define PIN_LCD_R4 40

// ---------- Bus I2C (GT911 + CH422G) ----------
#define PIN_I2C_SDA         8
#define PIN_I2C_SCL         9
#define I2C_FREQ_HZ         400000

// ---------- Tactile GT911 ----------
#define PIN_TOUCH_INT       4

// ---------- Batterie ----------
// La charge (CS8501, 580 mA) est entierement materielle : aucune broche de
// l'ESP32 ne la commande. Aucune mesure de tension n'est cablee d'origine :
// voir battery.h pour le pont diviseur a ajouter sur le connecteur "Sensor AD".
#define PIN_BATTERY_ADC     6

// ---------- Extension d'E/S CH422G (bits EXIO) ----------
// D'apres le schema Waveshare : EXIO6 = LCD_VDD_EN (laisse a 0 comme dans la demo Waveshare)
#define EXIO_TP_RST         1   // Reset tactile
#define EXIO_LCD_BL         2   // Retroeclairage
#define EXIO_LCD_RST        3   // Reset LCD
#define EXIO_SD_CS          4   // CS carte SD
#define EXIO_USB_SEL        5   // 0 = USB (obligatoire : console + charge batterie), 1 = CAN
