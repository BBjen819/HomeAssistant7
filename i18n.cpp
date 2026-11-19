#include "i18n.h"
#include <Preferences.h>

static volatile bool s_en = false;

void lang_begin() {
  Preferences p;
  p.begin("ui", true);
  s_en = p.getString("lang", "fr") == "en";
  p.end();
}

bool lang_en() { return s_en; }

void lang_set_en(bool en) {
  if (en == s_en) return;
  s_en = en;
  Preferences p;          // Ecriture unique, seulement sur action de l'utilisateur
  p.begin("ui", false);
  p.putString("lang", en ? "en" : "fr");
  p.end();
  Serial.printf("[UI] Langue : %s\n", en ? "anglais" : "francais");
}
