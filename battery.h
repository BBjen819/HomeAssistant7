// Mesure de la batterie.
//
// Sur la ESP32-S3-Touch-LCD-7, la charge est geree entierement par le CS8501
// (aucune commande ni aucun signal d'etat vers l'ESP32), et la tension de la
// batterie n'est reliee a aucune entree. Pour l'afficher, il faut un pont
// diviseur entre le + batterie et le connecteur "Sensor AD" (GPIO6) :
//
//   BAT+ ---[ 100 k ]---+--- AD (GPIO6)        rapport = (R1 + R2) / R2 = 2
//                       |
//   GND  ---[ 100 k ]---+
//
// La mesure est desactivee par defaut (broche flottante = valeurs fausses) ;
// elle s'active depuis la page web.
#pragma once
#include <Arduino.h>

void battery_begin();
void battery_loop();

bool  battery_enabled();
float battery_ratio();
void  battery_configure(bool enabled, float ratio);   // Enregistre en NVS

bool  battery_valid();        // Mesure activee et tension plausible
float battery_voltage();      // V
int   battery_percent();      // 0..100 (approximation Li-ion au repos)
uint32_t battery_version();   // Incremente a chaque changement affiche
