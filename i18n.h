// Langue de l'interface : francais (defaut) ou anglais.
// Choisie sur l'ecran "Connecte" (admin), enregistree en NVS (espace "ui", cle "lang").
// S'applique a l'ecran admin, aux messages systeme et a la page web.
// Les textes des tableaux de bord crees par l'utilisateur ne sont jamais traduits.
#pragma once
#include <Arduino.h>

void lang_begin();            // Lit la NVS (a appeler tot dans setup)
bool lang_en();               // true = anglais
void lang_set_en(bool en);    // Change et enregistre la langue

// Texte selon la langue : TR("Bonjour", "Hello")
#define TR(fr, en) (lang_en() ? (en) : (fr))
