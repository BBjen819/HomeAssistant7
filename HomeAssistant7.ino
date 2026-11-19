/*
  HomeAssistant7 - Base materielle pour Waveshare ESP32-S3-Touch-LCD-7 (800x480)

  1. Initialise l'I2C, le CH422G (retroeclairage / resets), l'ecran RGB et le GT911
  2. Wi-Fi : reseau memorise, sinon scan + clavier tactile
  3. Ecran "Connecte" (IP + adresse de configuration) pendant 5 s
  4. Tableau de bord cree depuis la page web http://<ip>/, relie a Home Assistant

  Reglages carte (Arduino IDE / arduino-cli) : voir README.md
*/
#include <Wire.h>
#include <lvgl.h>
#include "board_config.h"
#include "ch422g.h"
#include "gt911.h"
#include "lcd_panel.h"
#include "lvgl_port.h"
#include "app_ui.h"
#include "backlight.h"
#include "battery.h"
#include "ha_client.h"
#include "camera.h"
#include "web_config.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static void halt(const char *msg) {
  Serial.printf("\n*** ERREUR FATALE : %s ***\n", msg);
  while (true) delay(1000);
}

static void touch_hw_reset() {
  // Sequence Waveshare : INT bas pendant la remontee du reset => adresse GT911 0x5D
  ch422g_set(EXIO_TP_RST, false);
  pinMode(PIN_TOUCH_INT, OUTPUT);
  digitalWrite(PIN_TOUCH_INT, LOW);
  delay(100);
  ch422g_set(EXIO_TP_RST, true);
  delay(200);
  pinMode(PIN_TOUCH_INT, INPUT);
}

void setup() {
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0);  // Ne bloque pas si aucun terminal n'est ouvert sur l'USB
#endif
  delay(300);
  Serial.println("\n===== HomeAssistant7 - test materiel =====");
  Serial.printf("Puce %s rev %d, %d coeurs @ %lu MHz\n", ESP.getChipModel(), ESP.getChipRevision(),
                ESP.getChipCores(), (unsigned long)getCpuFrequencyMhz());
  Serial.printf("Flash %lu Mo, PSRAM %lu Mo (libre %lu Ko), SRAM libre %lu Ko\n",
                (unsigned long)(ESP.getFlashChipSize() >> 20), (unsigned long)(ESP.getPsramSize() >> 20),
                (unsigned long)(ESP.getFreePsram() >> 10), (unsigned long)(ESP.getFreeHeap() >> 10));
  if (!psramFound()) halt("PSRAM absente - choisir 'PSRAM: OPI PSRAM' dans le menu Outils");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_FREQ_HZ);

  // CH422G : retroeclairage eteint pendant l'init, LCD hors reset.
  // EXIO5 (USB_SEL) DOIT rester a 0 = mode USB (1 = mode CAN, comme dans la
  // demo Waveshare). A 1, le port USB natif se deconnecte des le demarrage
  // (console muette) et la batterie ne charge plus. ESPHome le laisse a 0.
  if (!ch422g_begin()) Serial.println("[CH422G] Pas de reponse sur l'I2C !");
  ch422g_write_all(1 << EXIO_LCD_RST);
  touch_hw_reset();

  if (!lcd_init()) halt("Initialisation de l'ecran impossible");
  ch422g_set(EXIO_LCD_BL, true);    // Framebuffer alloue a zero => ecran noir
  gt911_begin();

  if (!lvgl_port_init()) halt("Initialisation LVGL impossible");
  backlight_begin();
  battery_begin();
  ha_begin();
  camera_begin();
  app_ui_begin();   // Initialise aussi la pile Wi-Fi
  web_begin();

  Serial.printf("Init terminee. SRAM libre %lu Ko, PSRAM libre %lu Ko\n",
                (unsigned long)(ESP.getFreeHeap() >> 10), (unsigned long)(ESP.getFreePsram() >> 10));
}

void loop() {
  app_ui_loop();
  backlight_loop();
  battery_loop();
  uint32_t wait = lv_timer_handler();
  delay(wait > 5 ? 5 : (wait == 0 ? 1 : wait));
}
