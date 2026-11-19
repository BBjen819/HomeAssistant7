#include "i18n.h"
#include "camera.h"
#include "ha_client.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <vector>
#include "esp_heap_caps.h"
#include "jpeg_decoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const int MAX_CAMS = 6;
static const size_t MAX_JPEG = 1536 * 1024;     // Image JPEG maximale acceptee
static const uint32_t MIN_INTERVAL_MS = 100;
static const uint32_t ERROR_RETRY_MS = 3000;
static const uint32_t UNREACHABLE_RETRY_MS = 10000;   // Home Assistant injoignable

static inline bool unreachable(int code) { return code < 0 && code > -100; }   // Erreurs reseau HTTPClient

// Codes d'erreur internes (les codes HTTPClient sont de -1 a -11)
static const int ERR_NOT_CONFIGURED = -100;
static const int ERR_TOO_BIG = -101;
static const int ERR_DECODE = -102;
static const int ERR_MEMORY = -103;

CamFit camera_fit_from(const String &s) {
  if (s == "cover") return CAM_COVER;
  if (s == "stretch") return CAM_STRETCH;
  return CAM_CONTAIN;
}

// ---------------------------------------------------------------------------
// Etat partage
// ---------------------------------------------------------------------------
struct Slot {
  bool used = false;
  bool active = false;
  bool busy = false;     // La tache ecrit dans le tampon arriere
  bool ready = false;    // Nouvelle image dans le tampon arriere
  bool dead = false;     // Fermee pendant un telechargement : la tache libere
  String entity;
  int w = 0, h = 0;
  uint32_t interval = 1000;
  CamFit fit = CAM_CONTAIN;
  uint16_t bg = 0;
  uint16_t *buf[2] = {nullptr, nullptr};
  lv_image_dsc_t dsc[2];
  int front = 0;         // Tampon affiche par LVGL
  uint32_t next_at = 0;
  int err_code = 0;
  bool err_new = false;
  uint32_t frames = 0;
};

static Slot s_slots[MAX_CAMS];
static SemaphoreHandle_t s_mtx = nullptr;
static TaskHandle_t s_task = nullptr;

struct Lock {
  Lock() { xSemaphoreTake(s_mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mtx); }
};

static inline bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void free_slot(Slot &s) {
  heap_caps_free(s.buf[0]);
  heap_caps_free(s.buf[1]);
  s = Slot();
}

static String error_text(int code) {
  switch (code) {
    case 401: return TR("Caméra : jeton refusé", "Camera: token rejected");
    case 404: return TR("Caméra introuvable", "Camera not found");
    case ERR_NOT_CONFIGURED: return TR("Home Assistant non configuré", "Home Assistant not configured");
    case ERR_TOO_BIG: return TR("Image trop grande", "Image too large");
    case ERR_DECODE: return TR("Image illisible", "Unreadable image");
    case ERR_MEMORY: return TR("Mémoire insuffisante", "Not enough memory");
  }
  if (code >= 500) return TR("Caméra indisponible", "Camera unavailable");
  if (unreachable(code)) return TR("Home Assistant injoignable\nNouvel essai dans 10 s", "Home Assistant unreachable\nRetrying in 10 s");
  return TR("Caméra : erreur HTTP ", "Camera: HTTP error ") + String(code);
}

// ---------------------------------------------------------------------------
// Telechargement
// ---------------------------------------------------------------------------
// Flux d'ecriture vers un tampon en PSRAM (gere le transfert "chunked")
class BufStream : public Stream {
 public:
  uint8_t *buf = nullptr;
  size_t len = 0, cap = 0;
  bool overflow = false;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *p, size_t n) override {
    if (len + n > cap) {
      size_t nc = cap ? cap * 2 : 64 * 1024;
      while (nc < len + n) nc *= 2;
      if (nc > MAX_JPEG) nc = MAX_JPEG;
      uint8_t *nb = nc >= len + n ? (uint8_t *)heap_caps_realloc(buf, nc, MALLOC_CAP_SPIRAM) : nullptr;
      if (!nb) {
        overflow = true;
        return 0;
      }
      buf = nb;
      cap = nc;
    }
    memcpy(buf + len, p, n);
    len += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

struct Conn {
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  String base;
};

static int fetch_jpeg(Conn &c, const String &entity, int w, int h, BufStream &out, bool persistent) {
  String url, tok;
  if (!ha_get_credentials(url, tok)) return ERR_NOT_CONFIGURED;
  if (WiFi.status() != WL_CONNECTED) return HTTPC_ERROR_NOT_CONNECTED;
  bool https = url.startsWith("https://");
  if (c.base != url) {
    c.http.end();
    c.plain.stop();
    c.secure.stop();
    c.base = url;
  }
  if (https) c.secure.setInsecure();
  c.http.setReuse(persistent);
  c.http.setConnectTimeout(5000);
  c.http.setTimeout(8000);
  // HA reduit l'image cote serveur (par puissances de 2, au moins la taille demandee)
  String path = "/api/camera_proxy/" + entity + "?width=" + String(w) + "&height=" + String(h);
  if (!c.http.begin(https ? (WiFiClient &)c.secure : c.plain, url + path)) return HTTPC_ERROR_CONNECTION_REFUSED;
  c.http.addHeader("Authorization", "Bearer " + tok);
  int code = c.http.GET();
  if (code == 200) {
    out.len = 0;
    out.overflow = false;
    int n = c.http.writeToStream(&out);
    if (out.overflow) code = ERR_TOO_BIG;
    else if (n < 0) code = n;
  }
  c.http.end();
  if (code < 0 || !persistent) {
    c.plain.stop();
    c.secure.stop();
  }
  return code;
}

// ---------------------------------------------------------------------------
// Decodage + mise a l'echelle
// ---------------------------------------------------------------------------
struct Scratch {
  uint8_t *rgb = nullptr;
  size_t cap = 0;
  std::vector<int32_t> xmap;
};

static bool decode_to(const uint8_t *jpg, size_t len, uint16_t *dst, int W, int H, CamFit fit, uint16_t bg,
                      Scratch &sc, int &src_w, int &src_h, int &shift) {
  esp_jpeg_image_cfg_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.indata = (uint8_t *)jpg;
  cfg.indata_size = len;
  cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;   // Ordre R, G, B : converti en RGB565 ci-dessous
  cfg.out_scale = JPEG_IMAGE_SCALE_0;
  esp_jpeg_image_output_t info;
  if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || !info.width || !info.height) return false;
  src_w = info.width;
  src_h = info.height;

  // Reduction au decodage (1/2, 1/4, 1/8) tant que l'image reste assez grande
  float kx = (float)W / info.width, ky = (float)H / info.height;
  float k = fit == CAM_CONTAIN ? min(kx, ky) : max(kx, ky);
  int s = 0;
  while (s < 3 && k * (float)(1 << (s + 1)) <= 1.0f) s++;
  shift = s;

  size_t need = (size_t)((info.width >> s) + 2) * ((info.height >> s) + 2) * 3;
  if (need > sc.cap) {
    heap_caps_free(sc.rgb);
    sc.rgb = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    sc.cap = sc.rgb ? need : 0;
    if (!sc.rgb) return false;
  }
  cfg.priv.read = 0;
  cfg.outbuf = sc.rgb;
  cfg.outbuf_size = sc.cap;
  cfg.out_scale = (esp_jpeg_image_scale_t)s;
  esp_jpeg_image_output_t img;
  if (esp_jpeg_decode(&cfg, &img) != ESP_OK || !img.width || !img.height) return false;
  int sw = img.width, sh = img.height;

  // Placement dans le cadre W x H
  int dw, dh, ox, oy;
  if (fit == CAM_STRETCH) {
    dw = W; dh = H; ox = 0; oy = 0;
  } else {
    float fx = (float)W / sw, fy = (float)H / sh;
    float f = fit == CAM_CONTAIN ? min(fx, fy) : max(fx, fy);
    dw = max(1, (int)lroundf(sw * f));
    dh = max(1, (int)lroundf(sh * f));
    ox = (W - dw) / 2;
    oy = (H - dh) / 2;
  }

  // Plus proche voisin, ligne par ligne
  sc.xmap.resize(W);
  for (int x = 0; x < W; x++) {
    int dx = x - ox;
    sc.xmap[x] = (dx < 0 || dx >= dw) ? -1 : (int32_t)(((int64_t)dx * sw / dw) * 3);
  }
  for (int y = 0; y < H; y++) {
    uint16_t *row = dst + (size_t)y * W;
    int dy = y - oy;
    if (dy < 0 || dy >= dh) {
      for (int x = 0; x < W; x++) row[x] = bg;
      continue;
    }
    const uint8_t *src = sc.rgb + (size_t)((int64_t)dy * sh / dh) * sw * 3;
    for (int x = 0; x < W; x++) {
      int32_t o = sc.xmap[x];
      row[x] = o < 0 ? bg : rgb565(src[o], src[o + 1], src[o + 2]);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// Tache de fond
// ---------------------------------------------------------------------------
static void cam_task(void *) {
  Conn *conn = new Conn();
  BufStream jpg;
  Scratch sc;
  for (;;) {
    int id = -1;
    String entity;
    int w = 0, h = 0;
    CamFit fit = CAM_CONTAIN;
    uint16_t bg = 0, *dst = nullptr;
    uint32_t now = millis();
    {
      Lock l;
      uint32_t oldest = 0;
      for (int i = 0; i < MAX_CAMS; i++) {
        Slot &s = s_slots[i];
        if (!s.used || s.dead || !s.active || s.busy || s.ready || !due(now, s.next_at)) continue;
        uint32_t late = now - s.next_at;
        if (id < 0 || late > oldest) { id = i; oldest = late; }
      }
      if (id >= 0) {
        Slot &s = s_slots[id];
        s.busy = true;
        s.next_at = now + s.interval;
        entity = s.entity;
        w = s.w;
        h = s.h;
        fit = s.fit;
        bg = s.bg;
        dst = s.buf[1 - s.front];
      }
    }
    if (id < 0) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
      continue;
    }

    uint32_t t0 = millis();
    int code = fetch_jpeg(*conn, entity, w, h, jpg, true);
    uint32_t t1 = millis();
    int src_w = 0, src_h = 0, shift = 0;
    if (code == 200 && !decode_to(jpg.buf, jpg.len, dst, w, h, fit, bg, sc, src_w, src_h, shift))
      code = sc.rgb ? ERR_DECODE : ERR_MEMORY;
    uint32_t t2 = millis();

    {
      Lock l;
      Slot &s = s_slots[id];
      s.busy = false;
      if (s.dead) {
        free_slot(s);
      } else if (code == 200) {
        s.ready = true;
        s.err_code = 0;
        if (s.frames++ % 100 == 0)
          Serial.printf("[CAM] %s : image %dx%d (1/%d) %u Ko -> %dx%d, telechargement %lu ms, decodage %lu ms\n",
                        entity.c_str(), src_w, src_h, 1 << shift, (unsigned)(jpg.len / 1024), w, h,
                        (unsigned long)(t1 - t0), (unsigned long)(t2 - t1));
      } else {
        if (code != s.err_code) {
          s.err_code = code;
          s.err_new = true;
          Serial.printf("[CAM] %s : %s (%d)\n", entity.c_str(), error_text(code).c_str(), code);
        }
        uint32_t retry = millis() + (unreachable(code) ? UNREACHABLE_RETRY_MS : ERROR_RETRY_MS);
        if (!due(s.next_at, retry)) s.next_at = retry;
      }
    }
    // Apres une erreur reseau, le prochain essai repart d'une connexion neuve
    if (unreachable(code)) {
      conn->http.end();
      conn->plain.stop();
      conn->secure.stop();
      conn->base = "";
    }
    // Garde de petits tampons entre deux images, libere les gros (image plein ecran)
    if (jpg.cap > 256 * 1024) {
      heap_caps_free(jpg.buf);
      jpg.buf = nullptr;
      jpg.cap = jpg.len = 0;
    }
    vTaskDelay(1);
  }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void camera_begin() {
  s_mtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(cam_task, "camera", 8192, nullptr, 1, &s_task, 0);
}

int camera_open(const String &entity, int w, int h, uint32_t interval_ms, CamFit fit, uint32_t bg_rgb) {
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return -1;
  size_t bytes = (size_t)w * h * 2;
  uint16_t *b0 = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  uint16_t *b1 = (uint16_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  if (!b0 || !b1) {
    heap_caps_free(b0);
    heap_caps_free(b1);
    Serial.printf("[CAM] Memoire insuffisante pour %dx%d\n", w, h);
    return -1;
  }
  Lock l;
  for (int i = 0; i < MAX_CAMS; i++) {
    Slot &s = s_slots[i];
    if (s.used) continue;
    s = Slot();
    s.used = true;
    s.entity = entity;
    s.w = w;
    s.h = h;
    s.interval = max(MIN_INTERVAL_MS, interval_ms);
    s.fit = fit;
    s.bg = rgb565((bg_rgb >> 16) & 0xFF, (bg_rgb >> 8) & 0xFF, bg_rgb & 0xFF);
    s.buf[0] = b0;
    s.buf[1] = b1;
    for (int k = 0; k < 2; k++) {
      lv_image_dsc_t &d = s.dsc[k];
      memset(&d, 0, sizeof(d));
      d.header.magic = LV_IMAGE_HEADER_MAGIC;
      d.header.cf = LV_COLOR_FORMAT_RGB565;
      d.header.w = w;
      d.header.h = h;
      d.header.stride = w * 2;
      d.data_size = bytes;
      d.data = (const uint8_t *)s.buf[k];
    }
    s.next_at = millis();
    return i;
  }
  heap_caps_free(b0);
  heap_caps_free(b1);
  return -1;
}

void camera_close(int id) {
  if (id < 0 || id >= MAX_CAMS) return;
  Lock l;
  Slot &s = s_slots[id];
  if (!s.used) return;
  if (s.busy) s.dead = true;
  else free_slot(s);
}

void camera_close_all() {
  for (int i = 0; i < MAX_CAMS; i++) camera_close(i);
}

void camera_set_active(int id, bool active) {
  if (id < 0 || id >= MAX_CAMS) return;
  {
    Lock l;
    Slot &s = s_slots[id];
    if (!s.used || s.dead || s.active == active) return;
    s.active = active;
    if (active) s.next_at = millis();   // Image fraiche des le retour sur la page
  }
  if (active && s_task) xTaskNotifyGive(s_task);
}

int camera_poll(int id, const lv_image_dsc_t **dsc, String &err) {
  if (id < 0 || id >= MAX_CAMS) return 0;
  int r = 0;
  {
    Lock l;
    Slot &s = s_slots[id];
    if (!s.used || s.dead) return 0;
    if (s.ready) {
      s.ready = false;
      s.front = 1 - s.front;
      *dsc = &s.dsc[s.front];
      r = 1;
    } else if (s.err_new) {
      s.err_new = false;
      err = error_text(s.err_code);
      r = -1;
    }
  }
  if (r == 1 && s_task) xTaskNotifyGive(s_task);   // Le tampon arriere est libre
  return r;
}

bool camera_snapshot(const String &entity, int w, int h, uint8_t **jpg, size_t *len, String &err) {
  Conn c;
  BufStream out;
  int code = fetch_jpeg(c, entity, w, h, out, false);
  if (code != 200) {
    heap_caps_free(out.buf);
    err = error_text(code);
    return false;
  }
  *jpg = out.buf;
  *len = out.len;
  return true;
}
