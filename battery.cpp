#include "battery.h"
#include "board_config.h"
#include <Preferences.h>

static const uint32_t SAMPLE_PERIOD_MS = 2000;

static volatile bool  s_enabled = false;
static volatile float s_ratio = 2.0f;
static volatile float s_voltage = 0;
static volatile bool  s_valid = false;
static volatile int   s_percent = -1;
static volatile uint32_t s_version = 1;
static volatile bool  s_config_dirty = false;
static uint32_t s_last = 0;
static float s_filtered = 0;

// Tension a vide d'une cellule Li-ion -> pourcentage (approximation)
static int voltage_to_percent(float v) {
  static const struct { float v; int p; } T[] = {
    {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80}, {3.98f, 75}, {3.95f, 70},
    {3.91f, 65}, {3.87f, 60}, {3.85f, 55}, {3.84f, 50}, {3.82f, 45}, {3.80f, 40}, {3.79f, 35},
    {3.77f, 30}, {3.75f, 25}, {3.73f, 20}, {3.71f, 15}, {3.69f, 10}, {3.61f, 5}, {3.27f, 0},
  };
  const int n = sizeof(T) / sizeof(T[0]);
  if (v >= T[0].v) return 100;
  if (v <= T[n - 1].v) return 0;
  for (int i = 1; i < n; i++) {
    if (v >= T[i].v) {
      float k = (v - T[i].v) / (T[i - 1].v - T[i].v);
      return (int)lroundf(T[i].p + k * (T[i - 1].p - T[i].p));
    }
  }
  return 0;
}

void battery_begin() {
  Preferences p;
  p.begin("battery", true);
  s_enabled = p.getBool("en", false);
  s_ratio = p.getFloat("ratio", 2.0f);
  p.end();
  if (!(s_ratio > 0.5f && s_ratio < 20.0f)) s_ratio = 2.0f;
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);   // Plage 0..~3,1 V
  Serial.printf("[BATT] Mesure %s (GPIO%d, rapport %.2f)\n", s_enabled ? "activee" : "desactivee",
                PIN_BATTERY_ADC, (float)s_ratio);
}

void battery_loop() {
  if (s_config_dirty) {   // Ecriture NVS depuis la boucle principale (comme la luminosite)
    s_config_dirty = false;
    Preferences p;
    p.begin("battery", false);
    p.putBool("en", s_enabled);
    p.putFloat("ratio", s_ratio);
    p.end();
  }
  if (millis() - s_last < SAMPLE_PERIOD_MS) return;
  s_last = millis();

  if (!s_enabled) {
    if (s_valid || s_percent != -1) {
      s_valid = false;
      s_percent = -1;
      s_filtered = 0;
      s_version = s_version + 1;
    }
    return;
  }
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BATTERY_ADC);
  float v = (sum / 16.0f) / 1000.0f * s_ratio;
  s_filtered = (s_filtered <= 0) ? v : s_filtered * 0.8f + v * 0.2f;   // Lissage

  bool valid = s_filtered > 2.5f && s_filtered < 4.6f;   // Plausible pour une cellule Li-ion
  int pct = valid ? voltage_to_percent(s_filtered) : -1;
  if (valid != s_valid || pct != s_percent || fabsf(s_filtered - s_voltage) >= 0.01f) {
    s_valid = valid;
    s_percent = pct;
    s_voltage = s_filtered;
    s_version = s_version + 1;
  }
}

bool battery_enabled() { return s_enabled; }
float battery_ratio() { return s_ratio; }

void battery_configure(bool enabled, float ratio) {
  if (ratio > 0.5f && ratio < 20.0f) s_ratio = ratio;
  s_enabled = enabled;
  s_filtered = 0;
  s_last = 0;   // Nouvelle mesure tout de suite
  s_config_dirty = true;
  s_version = s_version + 1;
}

bool battery_valid() { return s_valid; }
float battery_voltage() { return s_voltage; }
int battery_percent() { return s_percent; }
uint32_t battery_version() { return s_version; }
