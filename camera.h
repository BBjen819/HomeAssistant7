// Cameras Home Assistant : images JPEG lues en boucle sur
//   GET /api/camera_proxy/<entite>?width=W&height=H
// puis decodees (esp_jpeg) et mises a l'echelle en RGB565 dans un double
// tampon en PSRAM. Le telechargement et le decodage tournent dans une tache
// de fond ; l'interface ne fait qu'echanger les tampons (camera_poll).
#pragma once
#include <Arduino.h>
#include <lvgl.h>

enum CamFit : uint8_t {
  CAM_CONTAIN = 0,   // Image entiere, bandes de la couleur de fond
  CAM_COVER,         // Remplit le cadre, bords rognes
  CAM_STRETCH        // Deformee pour remplir le cadre
};

CamFit camera_fit_from(const String &s);

void camera_begin();

// Fonctions de l'interface (tache LVGL)
int  camera_open(const String &entity, int w, int h, uint32_t interval_ms, CamFit fit, uint32_t bg_rgb);  // -1 si echec
void camera_close(int id);
void camera_close_all();
void camera_set_active(int id, bool active);   // Telecharge seulement si actif
// 1 = nouvelle image dans *dsc, -1 = nouvelle erreur dans err, 0 = rien de nouveau
int  camera_poll(int id, const lv_image_dsc_t **dsc, String &err);

// Image JPEG brute pour l'apercu du designer (a liberer avec free()).
bool camera_snapshot(const String &entity, int w, int h, uint8_t **jpg, size_t *len, String &err);
