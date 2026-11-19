// Serveur web de configuration (port 80, tache de fond) + stockage du layout.
//
//   GET  /                 Designer WYSIWYG (web/index.html)
//   GET  /api/info         Infos appareil (IP, resolution, tailles de police...)
//   GET  /api/layout       Layout enregistre (JSON) ou null
//   POST /api/layout       Enregistre le layout et rafraichit l'ecran
//   GET  /api/ha           Configuration Home Assistant (sans le jeton)
//   POST /api/ha           {url, token} - token vide = conserve l'actuel
//   POST /api/ha/test      {url, token} - teste la connexion
//   GET  /api/ha/entities  Liste des entites [{id, name}]
//   GET  /api/brightness   {value: 0..100, on}
//   POST /api/brightness   {value: 0..100} ou {on: true|false}
#pragma once
#include <Arduino.h>

// Toutes les routes /api/* exigent une session (sauf /api/login).
// Il n'y a pas de cle API : Home Assistant ne peut pas piloter l'ecran.
//   POST /api/login        {pin} -> cookie de session
//   POST /api/logout
extern const char *DEVICE_HOSTNAME;

// Mot de passe a 6 chiffres de cette session (affiche sur l'ecran "Connecte")
const String &web_password();

void web_begin();
void web_on_wifi_connected();   // Demarre mDNS (http://<hostname>.local/)

bool layout_exists();
bool layout_read(String &out);
bool web_layout_changed();      // true une seule fois apres chaque enregistrement
