// Client de l'API REST de Home Assistant (tache de fond).
//  - Configuration (URL + jeton longue duree) enregistree en NVS
//  - Lecture periodique des entites surveillees : GET /api/states/<id>
//  - Appels de service : POST /api/services/<domaine>/<service>
// Toutes les fonctions sont sures depuis n'importe quelle tache.
#pragma once
#include <Arduino.h>
#include <map>
#include <vector>

struct HaWatch {
  String entity;
  String attr;     // vide = etat principal
};

struct HaUpdate {
  String entity;
  bool   found;    // false si l'entite n'existe pas dans HA
  String state;
  std::map<String, String> attrs;
};

enum class HaStatus { NOT_CONFIGURED, CONNECTING, OK, AUTH_ERROR, ERROR };

void ha_begin();

void ha_get_config(String &url, bool &has_token);
void ha_set_config(const String &url, const String &token);   // token vide = conserver l'actuel
bool ha_get_credentials(String &url, String &token);           // false si non configure (camera)

// Tests et requetes synchrones (pour le serveur web). token vide = jeton enregistre.
bool ha_test(const String &url, const String &token, String &message);
bool ha_list_entities(String &json_out, String &error);
bool ha_list_services(String &json_out, String &error);   // Actions par domaine (format compact)
bool ha_get_state(const String &entity, String &json_out, String &error);

void ha_set_watch(const std::vector<HaWatch> &watch);
void ha_call(const String &service, const String &json_body);  // "light.toggle", "{...}"
void ha_take_updates(std::vector<HaUpdate> &out);

HaStatus ha_status();
String ha_status_text();
