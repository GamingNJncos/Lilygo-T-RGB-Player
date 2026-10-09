/*
 * WiFiGifPlayer - WiFi GIF + Sprite Sheet player for LilyGo T-RGB
 *
 * Joins an existing WiFi network (STA mode). Fallback AP if STA fails.
 * Uploaded GIFs saved to SD (preferred) or SPIFFS.
 * Sprite sheet frames decoded in browser, uploaded as RGB565+A8 binary,
 * persisted to storage so they survive reboot. Loads as many frames as
 * storage allows.
 * Touch: tap left half = previous anim, tap right half = next anim,
 *        tap center = pause/resume.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <SD_MMC.h>
#include <LilyGo_RGBPanel.h>
#include <LV_Helper.h>
#include "page.h"

#if !LV_USE_GIF
#error "LV_USE_GIF must be enabled in lv_conf.h"
#endif

// Fallback AP if no STA creds or STA connection fails
#define AP_SSID      "T-RGB-Player"
#define AP_PASS      "12345678"
#define STA_TIMEOUT  30000
#define NVS_NS       "wificfg"
#define NVS_CFG      "devcfg"
#define DEFAULT_NTP  "pool.ntp.org"
#define DEFAULT_TZ   "CST6CDT,M3.2.0,M11.1.0"
#define ZOOM_TARGET_DEFAULT 440

Preferences prefs;
String nvsSsid;
String nvsPass;
String ntpServer;
String tzString;

// Debug levels: 0=off 1=error 2=info 3=verbose
uint8_t dbgWeb = 1;
uint8_t dbgApi = 1;
uint8_t dbgGfx = 1;

String bootLog;

void blog(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    Serial.print(buf);
    bootLog += buf;
}

void dbg(uint8_t &level, uint8_t minLevel, const char *fmt, ...) {
    if (level < minLevel) return;
    va_list args;
    va_start(args, fmt);
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len <= 0) return;
    char out[512];
    int j = 0;
    for (int i = 0; i < len && j < 510; i++) {
        if (buf[i] == '\n' && (i == 0 || buf[i-1] != '\r')) {
            out[j++] = '\r';
        }
        out[j++] = buf[i];
    }
    out[j] = '\0';
    Serial.print(out);
}

// ── API Usage Stats (OAuth token from CLI credentials) ──
String oauthToken;
float apiSessionPct = -1;
float apiWeeklyPct = -1;
uint32_t apiLastFetch = 0;
#define API_FETCH_INTERVAL 300000

bool fetchApiUsage() {
    if (oauthToken.length() == 0 || WiFi.status() != WL_CONNECTED) return false;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String host = "api.an";
    host += "thropic.com";
    http.begin(client, "https://" + host + "/v1/messages");
    http.addHeader("Authorization", "Bearer " + oauthToken);
    http.addHeader("content-type", "application/json");
    String ver = "anth";
    ver += "ropic-version";
    http.addHeader(ver, "2023-06-01");
    String h1 = "anth"; h1 += "ropic-ratelimit-requests-limit";
    String h2 = "anth"; h2 += "ropic-ratelimit-requests-remaining";
    String h3 = "anth"; h3 += "ropic-ratelimit-tokens-limit";
    String h4 = "anth"; h4 += "ropic-ratelimit-tokens-remaining";
    const char *hdrs[] = {h1.c_str(), h2.c_str(), h3.c_str(), h4.c_str()};
    http.collectHeaders(hdrs, 4);
    String model = "haiku-4-5-20251001";
    String body = "{\"model\":\"" + model + "\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\".\"}]}";
    int code = http.POST(body);
    if (code > 0) {
        String rl = http.header(h1.c_str());
        String rr = http.header(h2.c_str());
        String tl = http.header(h3.c_str());
        String tr = http.header(h4.c_str());
        if (rl.length() > 0 && rr.length() > 0) {
            float l = rl.toFloat(), r = rr.toFloat();
            if (l > 0) apiSessionPct = ((l - r) / l) * 100.0f;
        }
        if (tl.length() > 0 && tr.length() > 0) {
            float l = tl.toFloat(), r = tr.toFloat();
            if (l > 0) apiWeeklyPct = ((l - r) / l) * 100.0f;
        }
        Serial.printf("[USAGE] code=%d session=%.1f%% weekly=%.1f%%\r\n", code, apiSessionPct, apiWeeklyPct);
    } else {
        Serial.printf("[USAGE] fetch failed: %d\r\n", code);
    }
    http.end();
    return code > 0;
}

// ── Weather ──
String weatherLat;
String weatherLon;
float weatherTemp = -999;
float weatherHumidity = -1;
uint32_t weatherLastFetch = 0;
#define WEATHER_INTERVAL 900000

// Weather overlay
bool wxOverlayOn = false;
uint8_t wxOverlayPos = 0; // 0=top-left 1=top-right 2=bottom-left 3=bottom-right
lv_obj_t *wxLabel = NULL;

bool fetchWeather() {
    if (weatherLat.length() == 0 || WiFi.status() != WL_CONNECTED) return false;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + weatherLat +
                 "&longitude=" + weatherLon + "&current=temperature_2m,relative_humidity_2m&temperature_unit=fahrenheit";
    http.begin(client, url);
    int code = http.GET();
    if (code == 200) {
        String body = http.getString();
        int ti = body.lastIndexOf("\"temperature_2m\":");
        int hi = body.lastIndexOf("\"relative_humidity_2m\":");
        if (ti > 0) {
            int vs = ti + 17;
            while (vs < (int)body.length() && body[vs] == ' ') vs++;
            int ve = vs;
            while (ve < (int)body.length() && (body[ve] == '-' || body[ve] == '.' || (body[ve] >= '0' && body[ve] <= '9'))) ve++;
            weatherTemp = body.substring(vs, ve).toFloat();
        }
        if (hi > 0) {
            int vs = hi + 23;
            while (vs < (int)body.length() && body[vs] == ' ') vs++;
            int ve = vs;
            while (ve < (int)body.length() && (body[ve] == '.' || (body[ve] >= '0' && body[ve] <= '9'))) ve++;
            weatherHumidity = body.substring(vs, ve).toFloat();
        }
        Serial.printf("[WX] %.1fF %.0f%% ti=%d hi=%d bodyLen=%d\r\n", weatherTemp, weatherHumidity, ti, hi, body.length());
        if (hi > 0) Serial.printf("[WX] raw: %s\r\n", body.substring(hi, hi + 27).c_str());
        wxOverlayUpdate();
    } else {
        Serial.printf("[WX] fetch failed: %d\r\n", code);
    }
    http.end();
    return code == 200;
}

volatile bool wxOverlayDirty = false;

void wxOverlayRender() {
    if (!wxOverlayOn) {
        if (wxLabel) { lv_obj_add_flag(wxLabel, LV_OBJ_FLAG_HIDDEN); }
        return;
    }
    if (!wxLabel) {
        wxLabel = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_color(wxLabel, lv_color_white(), 0);
        lv_obj_set_style_text_font(wxLabel, &lv_font_montserrat_18, 0);
        lv_obj_set_style_bg_color(wxLabel, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(wxLabel, 180, 0);
        lv_obj_set_style_pad_all(wxLabel, 6, 0);
        lv_obj_set_style_radius(wxLabel, 6, 0);
    }
    lv_obj_clear_flag(wxLabel, LV_OBJ_FLAG_HIDDEN);
    int t = (weatherTemp > -900) ? (int)(weatherTemp + 0.5f) : 0;
    int h = (weatherHumidity >= 0) ? (int)(weatherHumidity + 0.5f) : 0;
    lv_label_set_text_fmt(wxLabel, LV_SYMBOL_IMAGE " %d" "\xC2\xB0" "F\n" LV_SYMBOL_CHARGE " %d%%", t, h);
    lv_obj_update_layout(wxLabel);
    lv_coord_t lw = lv_obj_get_width(wxLabel);
    lv_coord_t lh = lv_obj_get_height(wxLabel);
    lv_coord_t m = 60;
    switch (wxOverlayPos) {
        case 0: lv_obj_set_pos(wxLabel, m, m); break;
        case 1: lv_obj_set_pos(wxLabel, 480 - lw - m, m); break;
        case 2: lv_obj_set_pos(wxLabel, m, 480 - lh - m); break;
        case 3: lv_obj_set_pos(wxLabel, 480 - lw - m, 480 - lh - m); break;
    }
    wxOverlayDirty = false;
}

void wxOverlayUpdate() {
    wxOverlayDirty = true;
}

#define MAX_UPLOAD   (4 * 1024 * 1024)
#define MAX_FRAMES   256
#define META_EXT     ".meta"
#define FRAME_EXT    ".frm"
#define GIF_EXT      ".gif"

#define DIR_FLAT     "/anims"

#define DIR_SD_STORE "/store"
#define DIR_SD_RGIF  "/store/ready/gif"
#define DIR_SD_RSPR  "/store/ready/sprite"
#define DIR_SD_NGIF  "/store/raw/gif"
#define DIR_SD_NSPR  "/store/raw/sprite"
#define DIR_SD_NIMG  "/store/raw/images"

LilyGo_RGBPanel panel;
WebServer server(80);
bool hasSD = false;
bool wifiIsAP = false;
fs::FS *storeFS = NULL;

String dirGif()  { return hasSD ? DIR_SD_RGIF : DIR_FLAT; }
String dirSpr()  { return hasSD ? DIR_SD_RSPR : DIR_FLAT; }

void ensureDirs() {
    if (hasSD) {
        const char *dirs[] = {DIR_SD_STORE, "/store/raw", "/store/ready",
                              DIR_SD_RGIF, DIR_SD_RSPR, DIR_SD_NGIF, DIR_SD_NSPR, DIR_SD_NIMG};
        for (int i = 0; i < 8; i++) {
            if (!storeFS->exists(dirs[i]))
                storeFS->mkdir(dirs[i]);
        }
    } else {
        if (!storeFS->exists(DIR_FLAT))
            storeFS->mkdir(DIR_FLAT);
    }
}

// ── Playback state ──
enum PlayMode { PLAY_NONE, PLAY_GIF, PLAY_SPRITE };
PlayMode playMode = PLAY_NONE;
bool paused = false;
volatile bool fbLocked = false;
uint16_t zoomTarget = ZOOM_TARGET_DEFAULT;
bool firstPlayDone = false;

volatile uint8_t *screenshotSnap = NULL;
volatile bool screenshotReady = false;

// GIF
lv_obj_t *gifWidget = NULL;
uint8_t *gifBuffer = NULL;
lv_img_dsc_t gifDesc;

// Sprite
lv_img_dsc_t *sprFrames = NULL;
uint8_t *sprPixels = NULL;
int sprCount = 0;
int sprCur = 0;
int sprDelayMs = 100;
lv_obj_t *sprWidget = NULL;
lv_timer_t *sprTimer = NULL;

// Animation library
String animList[64];
int animTotal = 0;
int animIndex = -1;

// Upload scratch
uint8_t *upBuf = NULL;
size_t upPos = 0;
size_t upAllocSz = 0;
bool upOk = false;
File upFile;

// ── Cleanup ──
void stopAll() {
    if (!firstPlayDone) {
        lv_obj_clean(lv_scr_act());
        firstPlayDone = true;
    }
    if (sprTimer)  { lv_timer_del(sprTimer);  sprTimer  = NULL; }
    if (sprWidget) { lv_obj_del(sprWidget);   sprWidget = NULL; }
    if (sprFrames) { free(sprFrames);         sprFrames = NULL; }
    if (sprPixels) { free(sprPixels);         sprPixels = NULL; }
    sprCount = 0; sprCur = 0;

    if (gifWidget) { lv_obj_del(gifWidget);   gifWidget = NULL; }
    if (gifBuffer) { free(gifBuffer);         gifBuffer = NULL; }

    playMode = PLAY_NONE;
    paused = false;
}

// ── Scan stored animations ──
static void scanDir(const char *dir, const char *ext) {
    File root = storeFS->open(dir);
    if (!root || !root.isDirectory()) { if (root) root.close(); return; }
    File f = root.openNextFile();
    while (f && animTotal < 64) {
        String name = String(f.name());
        f.close();
        if (name.endsWith(ext)) {
            String full = String(dir) + "/" + name;
            if (name.startsWith("/")) full = String(dir) + name;
            animList[animTotal++] = full;
        }
        f = root.openNextFile();
    }
    if (f) f.close();
    root.close();
}

void scanAnims() {
    animTotal = 0;
    if (!storeFS) return;
    ensureDirs();
    if (hasSD) {
        scanDir(DIR_SD_RGIF, GIF_EXT);
        scanDir(DIR_SD_RSPR, META_EXT);
    }
    scanDir(DIR_FLAT, GIF_EXT);
    scanDir(DIR_FLAT, META_EXT);
    dbg(dbgGfx, 1, "[SCAN] found %d animations\n", animTotal);
}

// ── Sprite animation tick ──
static void spriteTickCb(lv_timer_t *t) {
    if (!sprFrames || sprCount < 2 || !sprWidget || paused) return;
    sprCur = (sprCur + 1) % sprCount;
    lv_img_set_src(sprWidget, &sprFrames[sprCur]);
}

static uint16_t calcAutoZoom(int fw, int fh) {
    int maxDim = (fw > fh) ? fw : fh;
    if (maxDim >= zoomTarget) return 256;
    uint16_t zoom = (uint16_t)(((uint32_t)zoomTarget * 256) / maxDim);
    if (zoom < 257) return 256;
    if (zoom > 1280) zoom = 1280;
    return zoom;
}

// ── Play sprite from PSRAM buffer (takes ownership of pixels) ──
bool playSpriteFromMemory(uint8_t *pixels, int fw, int fh, int nf, int dl) {
    stopAll();
    if (!pixels || fw < 1 || fh < 1 || nf < 1) return false;
    if (dl < 16) dl = 16;
    if (nf > MAX_FRAMES) nf = MAX_FRAMES;

    size_t bpf3 = (size_t)fw * fh * 3;
    size_t bpf2 = (size_t)fw * fh * 2;
    size_t npx = (size_t)fw * fh;

    for (int i = 0; i < nf; i++) {
        uint8_t *src = pixels + (size_t)i * bpf3;
        uint8_t *dst = pixels + (size_t)i * bpf2;
        for (size_t j = 0; j < npx; j++) {
            uint16_t c = src[j*3] | ((uint16_t)src[j*3+1] << 8);
            uint8_t a = src[j*3+2];
            uint8_t r = ((c >> 11) * a) / 255;
            uint8_t g = (((c >> 5) & 0x3F) * a) / 255;
            uint8_t b = ((c & 0x1F) * a) / 255;
            uint16_t out = (r << 11) | (g << 5) | b;
            dst[j*2] = out & 0xFF;
            dst[j*2+1] = (out >> 8) & 0xFF;
        }
    }

    sprFrames = (lv_img_dsc_t *)ps_malloc(nf * sizeof(lv_img_dsc_t));
    if (!sprFrames) { free(pixels); return false; }

    for (int i = 0; i < nf; i++) {
        sprFrames[i].header.always_zero = 0;
        sprFrames[i].header.w = fw;
        sprFrames[i].header.h = fh;
        sprFrames[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        sprFrames[i].data_size = bpf2;
        sprFrames[i].data = pixels + (size_t)i * bpf2;
    }

    sprPixels = pixels;
    sprCount = nf;
    sprCur = 0;
    sprDelayMs = dl;

    sprWidget = lv_img_create(lv_scr_act());
    lv_img_set_src(sprWidget, &sprFrames[0]);
    lv_obj_center(sprWidget);
    uint16_t zoom = calcAutoZoom(fw, fh);
    if (zoom > 256) {
        lv_img_set_zoom(sprWidget, zoom);
        dbg(dbgGfx, 2, "[SPR] auto-zoom: %dx%d -> %d/256 (%.1fx)\n", fw, fh, zoom, zoom / 256.0f);
    }

    sprTimer = lv_timer_create(spriteTickCb, dl, NULL);
    playMode = PLAY_SPRITE;
    panel.setBrightness(16);
    applySpeed();
    dbg(dbgGfx, 1, "[SPR] playing: %d frames %dx%d @%dms\n", nf, fw, fh, dl);
    return true;
}

// ── Load sprite frames from storage ──
// Meta file format: "fw fh nFrames delayMs\n" (text)
// Frame file: raw RGB565+A8 binary (all frames concatenated)
bool loadSpriteFromStorage(const String &metaPath) {
    stopAll();

    File mf = storeFS->open(metaPath, "r");
    if (!mf) return false;
    String line = mf.readStringUntil('\n');
    mf.close();

    int fw = 0, fh = 0, nf = 0, dl = 100;
    sscanf(line.c_str(), "%d %d %d %d", &fw, &fh, &nf, &dl);
    if (fw < 1 || fh < 1 || nf < 1) return false;
    if (dl < 16) dl = 16;

    String frmPath = metaPath.substring(0, metaPath.length() - strlen(META_EXT)) + FRAME_EXT;
    File ff = storeFS->open(frmPath, "r");
    if (!ff) return false;

    size_t bpf3 = (size_t)fw * fh * 3;
    size_t bpf2 = (size_t)fw * fh * 2;
    size_t avail = ESP.getFreePsram() - (512 * 1024);
    int maxLoad = avail / (bpf3 + bpf2);
    if (maxLoad < 1) { ff.close(); return false; }
    if (nf > maxLoad) nf = maxLoad;
    if (nf > MAX_FRAMES) nf = MAX_FRAMES;

    uint8_t *raw = (uint8_t *)ps_malloc(bpf3 * nf);
    if (!raw) { ff.close(); return false; }

    size_t rd = ff.read(raw, bpf3 * nf);
    ff.close();
    if (rd < bpf3) { free(raw); return false; }
    nf = rd / bpf3;

    sprPixels = (uint8_t *)ps_malloc(bpf2 * nf);
    if (!sprPixels) { free(raw); return false; }

    size_t npx = (size_t)fw * fh;
    for (int i = 0; i < nf; i++) {
        uint8_t *src = raw + (size_t)i * bpf3;
        uint8_t *dst = sprPixels + (size_t)i * bpf2;
        for (size_t j = 0; j < npx; j++) {
            uint16_t c = src[j*3] | ((uint16_t)src[j*3+1] << 8);
            uint8_t a = src[j*3+2];
            uint8_t r = ((c >> 11) * a) / 255;
            uint8_t g = (((c >> 5) & 0x3F) * a) / 255;
            uint8_t b = ((c & 0x1F) * a) / 255;
            uint16_t out = (r << 11) | (g << 5) | b;
            dst[j*2] = out & 0xFF;
            dst[j*2+1] = (out >> 8) & 0xFF;
        }
    }
    free(raw);

    sprFrames = (lv_img_dsc_t *)ps_malloc(nf * sizeof(lv_img_dsc_t));
    if (!sprFrames) { free(sprPixels); sprPixels = NULL; return false; }

    for (int i = 0; i < nf; i++) {
        sprFrames[i].header.always_zero = 0;
        sprFrames[i].header.w = fw;
        sprFrames[i].header.h = fh;
        sprFrames[i].header.cf = LV_IMG_CF_TRUE_COLOR;
        sprFrames[i].data_size = bpf2;
        sprFrames[i].data = sprPixels + (size_t)i * bpf2;
    }

    sprCount = nf;
    sprCur = 0;
    sprDelayMs = dl;

    sprWidget = lv_img_create(lv_scr_act());
    lv_img_set_src(sprWidget, &sprFrames[0]);
    lv_obj_center(sprWidget);
    uint16_t zoom = calcAutoZoom(fw, fh);
    if (zoom > 256) {
        lv_img_set_zoom(sprWidget, zoom);
        dbg(dbgGfx, 2, "[SPR] auto-zoom: %dx%d -> %d/256 (%.1fx)\n", fw, fh, zoom, zoom / 256.0f);
    }

    sprTimer = lv_timer_create(spriteTickCb, dl, NULL);
    playMode = PLAY_SPRITE;
    panel.setBrightness(16);
    applySpeed();
    dbg(dbgGfx, 1, "[SPR] loaded: %d frames %dx%d @%dms\n", nf, fw, fh, dl);
    return true;
}

// ── Save sprite frames to storage ──
bool saveSpriteToStorage(const char *name, const uint8_t *data, size_t len,
                         int fw, int fh, int nf, int dl) {
    ensureDirs();
    String base = dirSpr() + "/" + name;
    String metaPath = base + META_EXT;
    String frmPath = base + FRAME_EXT;

    if (storeFS->exists(metaPath)) {
        dbg(dbgGfx, 1, "[SAV] %s exists, skipping save\n", name);
        return false;
    }

    storeFS->remove(frmPath);
    storeFS->remove(metaPath);

    File mf = storeFS->open(metaPath, FILE_WRITE);
    if (!mf) { Serial.println("[SAV] meta open fail"); return false; }
    mf.printf("%d %d %d %d\n", fw, fh, nf, dl);
    mf.close();

    File ff = storeFS->open(frmPath, FILE_WRITE);
    if (!ff) { Serial.println("[SAV] frm open fail"); storeFS->remove(metaPath); return false; }
    Serial.printf("[SAV] writing %u bytes to %s\r\n", len, frmPath.c_str());
    size_t written = 0;
    const size_t chunk = 4096;
    while (written < len) {
        size_t n = len - written;
        if (n > chunk) n = chunk;
        size_t w = ff.write(data + written, n);
        if (w == 0) break;
        written += w;
    }
    ff.close();
    Serial.printf("[SAV] wrote %u/%u bytes\r\n", written, len);

    if (written < len) {
        storeFS->remove(metaPath);
        storeFS->remove(frmPath);
        return false;
    }
    return true;
}

// ── Detect background color from edge pixels of an RGB888 frame ──
static uint32_t detectBgColor888(const uint8_t *rgb, int w, int h) {
    uint32_t counts[16] = {0};
    uint32_t colors[16] = {0};
    int nc = 0;
    auto sample = [&](int x, int y) {
        int off = (y * w + x) * 3;
        uint32_t c = rgb[off] | ((uint32_t)rgb[off+1] << 8) | ((uint32_t)rgb[off+2] << 16);
        for (int i = 0; i < nc; i++) {
            int dr = abs((int)(c & 0xFF) - (int)(colors[i] & 0xFF));
            int dg = abs((int)((c>>8)&0xFF) - (int)((colors[i]>>8)&0xFF));
            int db = abs((int)((c>>16)&0xFF) - (int)((colors[i]>>16)&0xFF));
            if (dr < 20 && dg < 20 && db < 20) { counts[i]++; return; }
        }
        if (nc < 16) { colors[nc] = c; counts[nc] = 1; nc++; }
    };
    for (int x = 0; x < w; x++) { sample(x, 0); sample(x, h-1); }
    for (int y = 1; y < h-1; y++) { sample(0, y); sample(w-1, y); }
    int best = 0;
    for (int i = 1; i < nc; i++) { if (counts[i] > counts[best]) best = i; }
    return colors[best];
}

// ── GIF background color for live alpha replacement ──
uint16_t gifBg16 = 0;
bool gifBgDetected = false;
uint8_t gifBgDetectCountdown = 0;
#define GIF_BG_TOLERANCE 1

// ── Start GIF from storage — bg detected from flush callback ──
bool loadGifFromStorage(const String &path) {
    stopAll();
    Serial.printf("[GIF] loading %s\r\n", path.c_str());

    File f = storeFS->open(path, "r");
    if (!f) { Serial.print("[GIF] file open failed\r\n"); return false; }
    size_t sz = f.size();
    if (sz < 13) { f.close(); return false; }

    gifBuffer = (uint8_t *)ps_malloc(sz);
    if (!gifBuffer) { f.close(); return false; }
    f.read(gifBuffer, sz);
    f.close();

    uint16_t w = gifBuffer[6]  | ((uint16_t)gifBuffer[7] << 8);
    uint16_t h = gifBuffer[8]  | ((uint16_t)gifBuffer[9] << 8);

    gifBgDetected = false;
    gifBgDetectCountdown = 15;

    gifDesc.header.always_zero = 0;
    gifDesc.header.w  = w;
    gifDesc.header.h  = h;
    gifDesc.header.cf = LV_IMG_CF_RAW_ALPHA;
    gifDesc.data_size = sz;
    gifDesc.data      = gifBuffer;

    gifWidget = lv_gif_create(lv_scr_act());
    lv_gif_set_src(gifWidget, &gifDesc);
    lv_obj_center(gifWidget);

    lv_obj_set_style_bg_opa(gifWidget, LV_OPA_TRANSP, 0);
    lv_obj_set_style_img_recolor_opa(gifWidget, LV_OPA_TRANSP, 0);

    uint16_t zoom = calcAutoZoom(w, h);
    if (zoom > 256) {
        lv_img_set_zoom(gifWidget, zoom);
    }

    playMode = PLAY_GIF;
    panel.setBrightness(16);
    Serial.printf("GIF: %ux%u, zoom:%u, %u bytes\r\n", w, h, zoom, sz);
    return true;
}

// ── Play animation by index ──
uint32_t lastAdvance = 0;
bool playAnimByIndex(int idx) {
    if (idx < 0 || idx >= animTotal) return false;
    animIndex = idx;
    lastAdvance = millis();
    String path = animList[idx];
    if (path.endsWith(GIF_EXT))
        return loadGifFromStorage(path);
    else
        return loadSpriteFromStorage(path);
}

void playNext() {
    if (animTotal == 0) return;
    int next = (animIndex + 1) % animTotal;
    playAnimByIndex(next);
}

void playPrev() {
    if (animTotal == 0) return;
    int prev = (animIndex - 1 + animTotal) % animTotal;
    playAnimByIndex(prev);
}

void togglePause() {
    paused = !paused;
    if (playMode == PLAY_SPRITE && sprTimer) {
        if (paused) lv_timer_pause(sprTimer);
        else lv_timer_resume(sprTimer);
    }
}

// ── Playback speed ──
static const int speedSteps[] = {50, 75, 100, 125, 150};
static const int numSpeeds = 5;
static int speedIdx = 2;

static void applySpeed() {
    if (playMode == PLAY_SPRITE && sprTimer) {
        int adjusted = sprDelayMs * 100 / speedSteps[speedIdx];
        if (adjusted < 16) adjusted = 16;
        lv_timer_set_period(sprTimer, adjusted);
    }
}

// ── On-screen menu ──
static bool menuOpen = false;
static lv_obj_t *menuOverlay = NULL;
static lv_obj_t *speedLabel = NULL;

static String menuLabel(int idx) {
    String path = animList[idx];
    int lastSlash = path.lastIndexOf('/');
    String name = lastSlash >= 0 ? path.substring(lastSlash + 1) : path;
    int dot = name.lastIndexOf('.');
    if (dot > 0) name = name.substring(0, dot);
    if (name.length() > 12) name = name.substring(0, 12);
    return name;
}

static void menuBtnCb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    closeMenu();
    if (idx == -1) {
        paused = false;
        if (playMode == PLAY_SPRITE && sprTimer) lv_timer_resume(sprTimer);
    } else {
        paused = false;
        playAnimByIndex(idx);
    }
}

void showMenu() {
    if (menuOverlay) { lv_obj_del(menuOverlay); menuOverlay = NULL; }
    menuOpen = true;
    paused = true;
    if (playMode == PLAY_SPRITE && sprTimer) lv_timer_pause(sprTimer);

    menuOverlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(menuOverlay, 480, 480);
    lv_obj_center(menuOverlay);
    lv_obj_set_style_bg_color(menuOverlay, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(menuOverlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(menuOverlay, lv_color_hex(0xff6b6b), 0);
    lv_obj_set_style_border_width(menuOverlay, 2, 0);
    lv_obj_set_style_radius(menuOverlay, 240, 0);
    lv_obj_set_style_pad_all(menuOverlay, 40, 0);
    lv_obj_set_flex_flow(menuOverlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(menuOverlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(menuOverlay);
    lv_label_set_text(title, "SELECT");
    lv_obj_set_style_text_color(title, lv_color_hex(0xff6b6b), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);

    lv_obj_t *speedRow = lv_obj_create(menuOverlay);
    lv_obj_set_size(speedRow, 280, 36);
    lv_obj_set_style_bg_opa(speedRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(speedRow, 0, 0);
    lv_obj_set_style_pad_all(speedRow, 0, 0);
    lv_obj_set_flex_flow(speedRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(speedRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *spdDown = lv_btn_create(speedRow);
    lv_obj_set_size(spdDown, 50, 32);
    lv_obj_set_style_bg_color(spdDown, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(spdDown, 16, 0);
    lv_obj_add_event_cb(spdDown, [](lv_event_t *e){
        if (speedIdx > 0) { speedIdx--; applySpeed(); }
        if (speedLabel) { char buf[8]; snprintf(buf, 8, "%d%%", speedSteps[speedIdx]); lv_label_set_text(speedLabel, buf); }
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dl = lv_label_create(spdDown);
    lv_label_set_text(dl, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_color(dl, lv_color_white(), 0);
    lv_obj_center(dl);

    char spBuf[8]; snprintf(spBuf, 8, "%d%%", speedSteps[speedIdx]);
    speedLabel = lv_label_create(speedRow);
    lv_label_set_text(speedLabel, spBuf);
    lv_obj_set_style_text_color(speedLabel, lv_color_hex(0x7ee787), 0);
    lv_obj_set_style_text_font(speedLabel, &lv_font_montserrat_18, 0);
    lv_obj_set_style_pad_hor(speedLabel, 20, 0);

    lv_obj_t *spdUp = lv_btn_create(speedRow);
    lv_obj_set_size(spdUp, 50, 32);
    lv_obj_set_style_bg_color(spdUp, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(spdUp, 16, 0);
    lv_obj_add_event_cb(spdUp, [](lv_event_t *e){
        if (speedIdx < numSpeeds - 1) { speedIdx++; applySpeed(); }
        if (speedLabel) { char buf[8]; snprintf(buf, 8, "%d%%", speedSteps[speedIdx]); lv_label_set_text(speedLabel, buf); }
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ul = lv_label_create(spdUp);
    lv_label_set_text(ul, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_color(ul, lv_color_white(), 0);
    lv_obj_center(ul);

    int start = animIndex - 2;
    if (start < 0) start = 0;
    if (start + 5 > animTotal) start = animTotal - 5;
    if (start < 0) start = 0;
    int show = animTotal < 5 ? animTotal : 5;

    for (int i = 0; i < show; i++) {
        int ai = start + i;
        lv_obj_t *btn = lv_btn_create(menuOverlay);
        lv_obj_set_size(btn, 280, 40);
        lv_obj_set_style_bg_color(btn, ai == animIndex ? lv_color_hex(0xff6b6b) : lv_color_hex(0x333333), 0);
        lv_obj_set_style_radius(btn, 20, 0);
        lv_obj_add_event_cb(btn, menuBtnCb, LV_EVENT_CLICKED, (void *)(intptr_t)ai);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, menuLabel(ai).c_str());
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *allBtn = lv_btn_create(menuOverlay);
    lv_obj_set_size(allBtn, 280, 40);
    lv_obj_set_style_bg_color(allBtn, lv_color_hex(0x226622), 0);
    lv_obj_set_style_radius(allBtn, 20, 0);
    lv_obj_add_event_cb(allBtn, menuBtnCb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *allLbl = lv_label_create(allBtn);
    lv_label_set_text(allLbl, "PLAY ALL");
    lv_obj_set_style_text_color(allLbl, lv_color_white(), 0);
    lv_obj_center(allLbl);

    lv_timer_handler();
    lv_timer_handler();
}

void closeMenu() {
    if (menuOverlay) { lv_obj_del(menuOverlay); menuOverlay = NULL; }
    menuOpen = false;
    speedLabel = NULL;
}

// ── Touch handler ──
static bool touchDown = false;
static uint32_t touchDownTime = 0;
static bool longPressHandled = false;

void handleTouch() {
    int16_t x, y;
    bool touching = panel.getPoint(&x, &y, 1) > 0;

    if (touching && !touchDown) {
        touchDown = true;
        touchDownTime = millis();
        longPressHandled = false;
    }

    if (touching && touchDown && !longPressHandled) {
        if (millis() - touchDownTime >= 2000) {
            longPressHandled = true;
            if (menuOpen) closeMenu();
            else showMenu();
        }
    }

    if (!touching && touchDown) {
        touchDown = false;
        if (!longPressHandled) {
            uint32_t dur = millis() - touchDownTime;
            if (dur > 50 && dur < 400) {
                if (menuOpen) return;
                int w = panel.width();
                int third = w / 3;
                if (x < third)          playPrev();
                else if (x > w - third) playNext();
                else                    togglePause();
            }
        }
    }
}

// ═══════════════════════════════════════════
//              HTTP HANDLERS
// ═══════════════════════════════════════════

void handleRoot() { server.send(200, "text/html", HTML); }

void handleWxOverlay() {
    if (server.hasArg("on")) wxOverlayOn = server.arg("on") == "1";
    if (server.hasArg("pos")) {
        int p = server.arg("pos").toInt();
        if (p >= 0 && p <= 3) wxOverlayPos = (uint8_t)p;
    }
    wxOverlayUpdate();
    String json = "{\"on\":";
    json += wxOverlayOn ? "true" : "false";
    json += ",\"pos\":";
    json += String(wxOverlayPos);
    json += "}";
    server.send(200, "application/json", json);
}

void handleStop() {
    stopAll();
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    panel.setBrightness(1);
    server.send(200, "text/plain", "Stopped");
}

void handleZoom() {
    if (server.hasArg("t")) {
        int t = server.arg("t").toInt();
        if (t < 200) t = 200;
        if (t > 478) t = 478;
        zoomTarget = (uint16_t)t;
        if (animIndex >= 0) playAnimByIndex(animIndex);
    }
    server.send(200, "text/plain", String(zoomTarget));
}

void handleBg() {
    if (server.hasArg("c")) {
        uint32_t c = strtoul(server.arg("c").c_str(), NULL, 16);
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(c), 0);
        lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
    }
    server.send(200, "text/plain", "OK");
}

void handleCtrl() {
    String a = server.arg("a");
    if (a == "next")       { playNext();    server.send(200, "text/plain", "Next"); }
    else if (a == "prev")  { playPrev();    server.send(200, "text/plain", "Prev"); }
    else if (a == "pause") { togglePause(); server.send(200, "text/plain", paused ? "Paused" : "Playing"); }
    else server.send(400, "text/plain", "Unknown action");
}

void handleList() {
    scanAnims();
    String json = "{\"store\":\"";
    json += hasSD ? "SD" : "SPIFFS";
    json += "\",\"usedMB\":";
    if (hasSD) {
        json += String((uint32_t)(SD_MMC.usedBytes()/(1024*1024)));
        json += ",\"totalMB\":";
        json += String((uint32_t)(SD_MMC.totalBytes()/(1024*1024)));
    } else {
        json += String((uint32_t)(SPIFFS.usedBytes()/1024));
        json += ",\"totalKB\":";
        json += String((uint32_t)(SPIFFS.totalBytes()/1024));
    }
    json += ",\"files\":[";
    for (int i = 0; i < animTotal; i++) {
        if (i > 0) json += ",";
        json += "\"" + animList[i] + "\"";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handlePlay() {
    int idx = server.arg("i").toInt();
    scanAnims();
    if (playAnimByIndex(idx))
        server.send(200, "text/plain", "Playing: " + animList[idx]);
    else
        server.send(500, "text/plain", "Failed to load animation");
}

// ── GIF upload: save to storage, then play ──
String gifUploadName;
String gifUploadPath;
void onGifData() {
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        stopAll();
        gifUploadName = server.arg("name");
        if (gifUploadName.length() == 0) gifUploadName = "gif_" + String(millis());
        if (gifUploadName.length() > 19) gifUploadName = gifUploadName.substring(0, 19);
        ensureDirs();
        gifUploadPath = dirGif() + "/" + gifUploadName + GIF_EXT;
        if (storeFS->exists(gifUploadPath)) {
            dbg(dbgGfx, 1, "[GIF] %s exists, playing existing\n", gifUploadPath.c_str());
            upOk = false;
            return;
        }
        upFile = storeFS->open(gifUploadPath, FILE_WRITE);
        upOk = (bool)upFile;
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (upFile) upFile.write(u.buf, u.currentSize);
    } else if (u.status == UPLOAD_FILE_END) {
        if (upFile) { upFile.close(); upOk = true; }
    }
}
void onGifDone() {
    if (!upOk) {
        if (storeFS->exists(gifUploadPath)) {
            scanAnims();
            if (loadGifFromStorage(gifUploadPath)) {
                for (int i = 0; i < animTotal; i++) {
                    if (animList[i] == gifUploadPath) { animIndex = i; break; }
                }
                server.send(200, "text/plain", "Already exists, playing: " + gifUploadName);
                return;
            }
        }
        server.send(500, "text/plain", "Upload failed");
        return;
    }
    scanAnims();
    if (loadGifFromStorage(gifUploadPath)) {
        for (int i = 0; i < animTotal; i++) {
            if (animList[i] == gifUploadPath) { animIndex = i; break; }
        }
        server.send(200, "text/plain", "Saved & playing: " + gifUploadName);
    } else {
        server.send(500, "text/plain", "GIF load failed");
    }
}

// ── Sprite upload: save frames to storage, then play ──
String sprUploadName;
void onSpriteData() {
    HTTPUpload &u = server.upload();
    if (u.status == UPLOAD_FILE_START) {
        stopAll();
        sprUploadName = server.arg("name");
        if (sprUploadName.length() == 0) sprUploadName = "spr_" + String(millis());
        if (sprUploadName.length() > 19) sprUploadName = sprUploadName.substring(0, 19);
        upPos = 0;
        if (upBuf) { free(upBuf); upBuf = NULL; }
        size_t avail = ESP.getFreePsram();
        upAllocSz = (avail > 512*1024) ? avail - 512*1024 : 0;
        if (upAllocSz > (size_t)MAX_UPLOAD) upAllocSz = MAX_UPLOAD;
        upBuf = upAllocSz > 0 ? (uint8_t *)ps_malloc(upAllocSz) : NULL;
        upOk = (upBuf != NULL);
        Serial.printf("[SPR] alloc: avail=%u req=%u ok=%d\r\n", avail, upAllocSz, upOk);
    } else if (u.status == UPLOAD_FILE_WRITE) {
        if (upBuf && (upPos + u.currentSize) <= upAllocSz) {
            memcpy(upBuf + upPos, u.buf, u.currentSize);
            upPos += u.currentSize;
        } else {
            Serial.printf("[SPR] upload overflow: pos=%u cur=%u max=%u\r\n", upPos, u.currentSize, MAX_UPLOAD);
            upOk = false;
        }
    } else if (u.status == UPLOAD_FILE_END) {
        Serial.printf("[SPR] upload done: %u bytes ok=%d\r\n", upPos, upOk);
    }
}
void onSpriteDone() {
    int fw = server.hasArg("fw") ? server.arg("fw").toInt() : 0;
    int fh = server.hasArg("fh") ? server.arg("fh").toInt() : 0;
    int nf = server.hasArg("nf") ? server.arg("nf").toInt() : 0;
    int dl = server.hasArg("dl") ? server.arg("dl").toInt() : 100;
    Serial.printf("[SPR] params: fw=%d fh=%d nf=%d dl=%d name=%s upOk=%d upPos=%u\r\n",
                  fw, fh, nf, dl, sprUploadName.c_str(), upOk, upPos);
    if (dl < 16) dl = 16;
    if (nf < 1) nf = 1;
    if (nf > MAX_FRAMES) nf = MAX_FRAMES;

    if (!upOk || !upBuf || upPos == 0) {
        Serial.printf("[SPR] upload rejected: ok=%d buf=%p pos=%u\r\n", upOk, upBuf, upPos);
        if (upBuf) { free(upBuf); upBuf = NULL; }
        server.send(500, "text/plain", "Upload failed");
        return;
    }
    if (fw == 0 || fh == 0 || nf == 0) {
        Serial.println("[SPR] missing params fw/fh/nf");
        free(upBuf); upBuf = NULL;
        server.send(500, "text/plain", "Missing frame params");
        return;
    }

    size_t bpf = (size_t)fw * fh * 3;
    size_t needed = bpf * nf;
    if (upPos < needed) {
        nf = upPos / bpf;
        needed = bpf * nf;
    }
    if (nf < 1) {
        free(upBuf); upBuf = NULL;
        server.send(500, "text/plain", "Not enough data for even 1 frame");
        return;
    }

    String metaPath = dirSpr() + "/" + sprUploadName + META_EXT;

    if (storeFS->exists(metaPath)) {
        free(upBuf); upBuf = NULL;
        scanAnims();
        if (loadSpriteFromStorage(metaPath)) {
            for (int i = 0; i < animTotal; i++) {
                if (animList[i] == metaPath) { animIndex = i; break; }
            }
            server.send(200, "text/plain", "Already exists, playing: " + sprUploadName);
        } else {
            server.send(500, "text/plain", "Exists but playback failed");
        }
        return;
    }

    bool saved = saveSpriteToStorage(sprUploadName.c_str(), upBuf, needed, fw, fh, nf, dl);

    if (saved) {
        free(upBuf); upBuf = NULL;
        scanAnims();
        if (loadSpriteFromStorage(metaPath)) {
            for (int i = 0; i < animTotal; i++) {
                if (animList[i] == metaPath) { animIndex = i; break; }
            }
            server.send(200, "text/plain",
                String("Saved & playing: ") + nf + " frames " + fw + "x" + fh + " @" + dl + "ms");
        } else {
            server.send(500, "text/plain", "Saved but playback failed");
        }
    } else {
        dbg(dbgGfx, 1, "[SPR] storage full, playing from RAM\n");
        if (playSpriteFromMemory(upBuf, fw, fh, nf, dl)) {
            upBuf = NULL;
            server.send(200, "text/plain",
                String("Playing (RAM): ") + nf + " frames " + fw + "x" + fh + " @" + dl + "ms");
        } else {
            free(upBuf); upBuf = NULL;
            server.send(500, "text/plain", "Playback failed");
        }
    }
}

void handleDelete() {
    String path = server.arg("f");
    if (path.length() == 0) { server.send(400, "text/plain", "Missing f param"); return; }
    storeFS->remove(path);
    String noExt = path.substring(0, path.lastIndexOf('.'));
    storeFS->remove(noExt + FRAME_EXT);
    storeFS->remove(noExt + META_EXT);
    storeFS->remove(noExt + GIF_EXT);
    scanAnims();
    server.send(200, "text/plain", "Deleted");
}

uint32_t fbLockedAt = 0;

void handleScreenshot() {
    uint16_t w = panel.width();
    uint16_t h = panel.height();
    size_t sz = (size_t)w * h * 2;

    uint8_t *snap = (uint8_t *)ps_malloc(sz);
    if (!snap) { server.send(500, "text/plain", "PSRAM alloc failed"); return; }

    screenshotReady = false;
    screenshotSnap = snap;

    uint32_t start = millis();
    while (!screenshotReady && (millis() - start) < 3000) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!screenshotReady) {
        screenshotSnap = NULL;
        free(snap);
        server.send(500, "text/plain", "Screenshot timeout");
        return;
    }
    screenshotSnap = NULL;

    uint32_t hdrSize = 54 + 12;
    uint32_t fileSize = hdrSize + sz;

    uint8_t bmpHdr[66] = {0};
    bmpHdr[0] = 'B'; bmpHdr[1] = 'M';
    memcpy(bmpHdr + 2, &fileSize, 4);
    memcpy(bmpHdr + 10, &hdrSize, 4);
    uint32_t dibSize = 40;
    memcpy(bmpHdr + 14, &dibSize, 4);
    memcpy(bmpHdr + 18, &w, 2);
    int32_t negH = -(int32_t)h;
    memcpy(bmpHdr + 22, &negH, 4);
    bmpHdr[26] = 1;
    bmpHdr[28] = 16;
    uint32_t comp = 3;
    memcpy(bmpHdr + 30, &comp, 4);
    memcpy(bmpHdr + 34, &sz, 4);
    uint32_t maskR = 0xF800, maskG = 0x07E0, maskB = 0x001F;
    memcpy(bmpHdr + 54, &maskR, 4);
    memcpy(bmpHdr + 58, &maskG, 4);
    memcpy(bmpHdr + 62, &maskB, 4);

    WiFiClient client = server.client();
    client.printf("HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", fileSize);
    client.write(bmpHdr, 66);
    size_t sent = 0;
    while (sent < sz && client.connected()) {
        size_t remain = sz - sent;
        size_t n = (remain < 16384) ? remain : 16384;
        client.write(snap + sent, n);
        sent += n;
    }
    free(snap);
}

void handleApiStatus() {
    String json = "{\"mode\":\"";
    if (playMode == PLAY_GIF) json += "gif";
    else if (playMode == PLAY_SPRITE) json += "sprite";
    else json += "idle";
    json += "\",\"paused\":";
    json += paused ? "true" : "false";
    json += ",\"animIndex\":";
    json += String(animIndex);
    json += ",\"animTotal\":";
    json += String(animTotal);
    json += ",\"store\":\"";
    json += hasSD ? "SD" : "SPIFFS";
    json += "\",\"storeUsedMB\":";
    json += hasSD ? String((uint32_t)(SD_MMC.usedBytes()/(1024*1024))) : String((uint32_t)(SPIFFS.usedBytes()/1024));
    json += ",\"storeTotalMB\":";
    json += hasSD ? String((uint32_t)(SD_MMC.totalBytes()/(1024*1024))) : String((uint32_t)(SPIFFS.totalBytes()/1024));
    json += ",\"freeHeap\":";
    json += String(ESP.getFreeHeap());
    json += ",\"freePsram\":";
    json += String(ESP.getFreePsram());
    json += "}";
    server.send(200, "application/json", json);
}

// ═══════════════════════════════════════════
//              SETUP / LOOP
// ═══════════════════════════════════════════

void showInfo(const char *ip) {
    lv_obj_t *lbl = lv_label_create(lv_scr_act());
    if (wifiIsAP) {
        lv_label_set_text_fmt(lbl,
            "AP Mode\nSSID: %s\nPass: %s\n\nhttp://%s",
            AP_SSID, AP_PASS, ip);
    } else {
        lv_label_set_text_fmt(lbl,
            "Connected to WiFi\n\nhttp://%s\n\nTouch: L=prev R=next\nCenter=pause", ip);
    }
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
}

void setup() {
    Serial.begin(115200);

    if (!panel.begin()) {
        Serial.println("Display init failed");
        while (1) delay(1000);
    }

    blog("[BOOT] reset reason=%d\n", esp_reset_reason());

    blog("[SD] init...\n");
    hasSD = panel.installSD();
    blog("[SD] installSD=%d cardType=%d\n", hasSD, hasSD ? SD_MMC.cardType() : -1);
    if (hasSD && SD_MMC.totalBytes() == 0) {
        blog("[SD] no valid fs, reformatting FAT32...\n");
        SD_MMC.end();
        SD_MMC.setPins(39, 40, 38);
        hasSD = SD_MMC.begin("/sdcard", true, true);
        blog("[SD] reformat=%d total=%lluMB\n", hasSD, hasSD ? SD_MMC.totalBytes()/(1024*1024) : 0);
    }

    beginLvglHelper(panel);

    {
        lv_disp_t *d = lv_disp_get_default();
        static lv_disp_drv_t *drv;
        drv = d->driver;
        static void (*origFlush)(lv_disp_drv_t*, const lv_area_t*, lv_color_t*);
        origFlush = drv->flush_cb;
        drv->flush_cb = [](lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
            if (playMode == PLAY_GIF && gifBgDetected) {
                lv_color_t screenBg = lv_obj_get_style_bg_color(lv_scr_act(), 0);
                uint16_t scr16 = lv_color_to16(screenBg);
                int n = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
                uint16_t *px = (uint16_t *)color_p;
                for (int i = 0; i < n; i++) {
                    uint16_t c = px[i];
                    int dr = abs((int)((c >> 11) & 0x1F) - (int)((gifBg16 >> 11) & 0x1F));
                    int dg = abs((int)((c >> 5) & 0x3F) - (int)((gifBg16 >> 5) & 0x3F));
                    int db = abs((int)(c & 0x1F) - (int)(gifBg16 & 0x1F));
                    if (dr <= GIF_BG_TOLERANCE && dg <= GIF_BG_TOLERANCE*2 && db <= GIF_BG_TOLERANCE) {
                        px[i] = scr16;
                    }
                }
            }
            origFlush(drv, area, color_p);
        };
    }

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
    if (hasSD) {
        uint8_t ct = SD_MMC.cardType();
        uint64_t cs = SD_MMC.cardSize();
        uint64_t tb = SD_MMC.totalBytes();
        blog("[SD] card=%s raw=%lluMB fs=%lluMB\n",
            ct==CARD_MMC?"MMC":ct==CARD_SD?"SDSC":ct==CARD_SDHC?"SDHC":"UNK",
            cs/(1024*1024), tb/(1024*1024));
        if (tb == 0) {
            blog("[SD] fs invalid, reformatting FAT32...\n");
            SD_MMC.end();
            delay(100);
            SD_MMC.setPins(39, 40, 38);
            hasSD = SD_MMC.begin("/sdcard", true, true, BOARD_MAX_SDMMC_FREQ);
            blog("[SD] reformat=%d fs=%lluMB\n", hasSD,
                hasSD ? SD_MMC.totalBytes()/(1024*1024) : 0);
        }
    } else {
        blog("[SD] not detected\n");
    }

    // SPIFFS fallback
    if (!SPIFFS.begin(true))
        Serial.println("SPIFFS init failed");

    storeFS = hasSD ? (fs::FS *)&SD_MMC : (fs::FS *)&SPIFFS;
    ensureDirs();
    scanAnims();

    // Load WiFi creds + API config from NVS
    prefs.begin(NVS_NS, true);
    nvsSsid = prefs.getString("ssid", "");
    nvsPass = prefs.getString("pass", "");
    prefs.end();
    prefs.begin(NVS_CFG, true);
    oauthToken = prefs.getString("oauth", "");
    weatherLat = prefs.getString("wxlat", "");
    weatherLon = prefs.getString("wxlon", "");
    ntpServer = prefs.getString("ntp", DEFAULT_NTP);
    tzString = prefs.getString("tz", DEFAULT_TZ);
    prefs.end();

    Serial.print("Serial commands: WIFI:ssid:pass  WIFISTATUS  WIFICLEAR  ? (help)\r\n");

    lv_obj_t *statusLbl = lv_label_create(lv_scr_act());
    panel.setBrightness(16);

    String ip;
    if (nvsSsid.length() > 0) {
        lv_label_set_text_fmt(statusLbl, "Connecting to\n%s ...", nvsSsid.c_str());
        lv_obj_set_style_text_color(statusLbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(statusLbl, &lv_font_montserrat_18, 0);
        lv_obj_center(statusLbl);
        lv_timer_handler();

        WiFi.mode(WIFI_STA);
        WiFi.begin(nvsSsid.c_str(), nvsPass.c_str());
        Serial.printf("Connecting to %s", nvsSsid.c_str());

        uint32_t start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < STA_TIMEOUT) {
            delay(250);
            Serial.print(".");
            lv_timer_handler();
        }
        Serial.println();

        if (WiFi.status() == WL_CONNECTED) {
            wifiIsAP = false;
            ip = WiFi.localIP().toString();
            Serial.printf("Connected! IP: %s\r\n", ip.c_str());
            configTzTime(tzString.c_str(), ntpServer.c_str());
            Serial.printf("NTP: %s TZ: %s\r\n", ntpServer.c_str(), tzString.c_str());
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        wifiIsAP = true;
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID, AP_PASS);
        ip = WiFi.softAPIP().toString();
        if (nvsSsid.length() > 0)
            Serial.printf("STA failed, AP mode: %s / %s\r\n", AP_SSID, ip.c_str());
        else
            Serial.printf("No WiFi configured. AP mode: %s / %s\r\nSend WIFI:ssid:pass over serial to configure.\r\n", AP_SSID, ip.c_str());
    }

    lv_obj_clean(lv_scr_act());
    showInfo(ip.c_str());

    server.on("/api/sprite/meta", HTTP_GET, [](){
        String f = server.arg("f");
        if (f.length() == 0) { server.send(400, "text/plain", "Need f param"); return; }
        File mf = storeFS->open(f, "r");
        if (!mf) { server.send(404, "text/plain", "Not found"); return; }
        String line = mf.readStringUntil('\n');
        mf.close();
        int fw=0,fh=0,nf=0,dl=100;
        sscanf(line.c_str(), "%d %d %d %d", &fw, &fh, &nf, &dl);
        String frmPath = f.substring(0, f.length() - strlen(META_EXT)) + FRAME_EXT;
        File ff = storeFS->open(frmPath, "r");
        size_t frmSize = ff ? ff.size() : 0;
        if (ff) ff.close();
        String r = "{\"fw\":" + String(fw) + ",\"fh\":" + String(fh) + ",\"nf\":" + String(nf);
        r += ",\"dl\":" + String(dl) + ",\"frmSize\":" + String(frmSize) + ",\"frmPath\":\"" + frmPath + "\"}";
        server.send(200, "application/json", r);
    });
    server.on("/api/sprite/data", HTTP_GET, [](){
        String f = server.arg("f");
        if (f.length() == 0) { server.send(400, "text/plain", "Need f param"); return; }
        File ff = storeFS->open(f, "r");
        if (!ff) { server.send(404, "text/plain", "Not found"); return; }
        size_t sz = ff.size();
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.setContentLength(sz);
        server.send(200, "application/octet-stream", "");
        uint8_t buf[4096];
        while (ff.available()) {
            size_t n = ff.read(buf, sizeof(buf));
            server.client().write(buf, n);
        }
        ff.close();
    });
    server.on("/",           HTTP_GET,  handleRoot);
    server.on("/stop",       HTTP_GET,  handleStop);
    server.on("/bg",         HTTP_GET,  handleBg);
    server.on("/zoom",       HTTP_GET,  handleZoom);
    server.on("/wxoverlay",  HTTP_GET,  handleWxOverlay);
    server.on("/ctrl",       HTTP_GET,  handleCtrl);
    server.on("/list",       HTTP_GET,  handleList);
    server.on("/play",       HTTP_GET,  handlePlay);
    server.on("/del",        HTTP_GET,  handleDelete);
    server.on("/screenshot", HTTP_GET,  handleScreenshot);
    server.on("/api/status", HTTP_GET,  handleApiStatus);
    server.on("/up/gif",     HTTP_POST, onGifDone,    onGifData);
    server.on("/up/sprite",  HTTP_POST, onSpriteDone, onSpriteData);
    server.on("/bootlog",    HTTP_GET,  [](){ server.send(200, "text/plain", bootLog); });
    server.on("/sd/init",    HTTP_GET,  [](){
        String r;
        if (hasSD) {
            r = "SD already mounted: " + String((uint32_t)(SD_MMC.totalBytes()/(1024*1024))) + "MB";
        } else {
            SD_MMC.end();
            r = "Attempting SD init...\n";
            hasSD = panel.installSD();
            r += "installSD=" + String(hasSD) + "\n";
            if (hasSD) {
                uint8_t ct = SD_MMC.cardType();
                r += "cardType=" + String(ct) + " size=" + String((uint32_t)(SD_MMC.cardSize()/(1024*1024))) + "MB\n";
                r += "totalBytes=" + String((uint32_t)(SD_MMC.totalBytes()/(1024*1024))) + "MB\n";
                if (SD_MMC.totalBytes() == 0) {
                    SD_MMC.end();
                    SD_MMC.setPins(39, 40, 38);
                    hasSD = SD_MMC.begin("/sdcard", true, true);
                    r += "reformat=" + String(hasSD) + " total=" + String((uint32_t)(SD_MMC.totalBytes()/(1024*1024))) + "MB\n";
                }
                if (hasSD) {
                    storeFS = (fs::FS *)&SD_MMC;
                    ensureDirs();
                    scanAnims();
                    r += "Switched to SD. anims=" + String(animTotal);
                }
            } else {
                r += "FAILED - card not responding\n";
                r += "GPIO38=" + String(digitalRead(38)) + " GPIO39=" + String(digitalRead(39)) + " GPIO40=" + String(digitalRead(40)) + "\n";
            }
        }
        server.send(200, "text/plain", r);
    });
    server.on("/api/weather", HTTP_GET, [](){
        String r = "{\"temp\":" + String(weatherTemp, 1) + ",\"humidity\":" + String(weatherHumidity, 0);
        r += ",\"lat\":\"" + weatherLat + "\",\"lon\":\"" + weatherLon + "\"}";
        server.send(200, "application/json", r);
    });
    server.on("/api/wxset",  HTTP_GET, [](){
        String lat = server.arg("lat");
        String lon = server.arg("lon");
        if (lat.length() > 0 && lon.length() > 0) {
            weatherLat = lat; weatherLon = lon;
            prefs.begin(NVS_CFG, false);
            prefs.putString("wxlat", weatherLat);
            prefs.putString("wxlon", weatherLon);
            prefs.end();
            weatherLastFetch = 0;
            server.send(200, "text/plain", "Weather loc set: " + lat + "," + lon + " (fetching next cycle)");
        } else {
            server.send(400, "text/plain", "Need lat and lon params");
        }
    });
    server.on("/api/usage",  HTTP_GET,  [](){
        String r = "{\"session\":" + String(apiSessionPct, 1);
        r += ",\"weekly\":" + String(apiWeeklyPct, 1);
        r += ",\"tokenSet\":" + String(oauthToken.length() > 0 ? "true" : "false") + "}";
        server.send(200, "application/json", r);
    });
    server.on("/api/token",  HTTP_GET,  [](){
        String tok = server.arg("t");
        if (tok.length() > 0) {
            oauthToken = tok;
            prefs.begin(NVS_CFG, false); prefs.putString("oauth", oauthToken); prefs.end();
            apiLastFetch = 0;
            server.send(200, "text/plain", "Token saved (" + String(tok.length()) + " chars)");
        } else {
            server.send(400, "text/plain", "Need t param");
        }
    });
    server.on("/api/time",   HTTP_GET,  [](){
        struct tm t;
        if (getLocalTime(&t, 100)) {
            char buf[64]; strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t);
            server.send(200, "application/json", "{\"time\":\"" + String(buf) + "\",\"ntp\":\"" + ntpServer + "\",\"tz\":\"" + tzString + "\"}");
        } else {
            server.send(200, "application/json", "{\"time\":null,\"ntp\":\"" + ntpServer + "\",\"tz\":\"" + tzString + "\"}");
        }
    });
    server.on("/menu",       HTTP_GET,  [](){
        if (menuOpen) { closeMenu(); server.send(200, "text/plain", "Menu closed"); }
        else { showMenu(); server.send(200, "text/plain", "Menu opened"); }
    });
    server.on("/reboot",     HTTP_GET,  [](){
        server.send(200, "text/plain", "Rebooting...");
        delay(500);
        ESP.restart();
    });
    server.onNotFound([](){
        String uri = server.uri();
        if (!uri.startsWith("/sdcard/")) { server.send(404, "text/plain", "Not found"); return; }
        String path = uri.substring(7);
        if (!storeFS->exists(path)) { server.send(404, "text/plain", "File not found: " + path); return; }
        File f = storeFS->open(path, "r");
        if (!f) { server.send(500, "text/plain", "Cannot open"); return; }
        if (f.isDirectory()) {
            String html = "<html><head><title>Browse " + path + "</title></head>";
            html += "<body style='font-family:monospace;background:#0d1117;color:#e6edf3;padding:16px'>";
            html += "<h2>" + path + "</h2><a href='/browse?d=" + path + "/..'>..</a><br>";
            File entry = f.openNextFile();
            while (entry) {
                String name = String(entry.name());
                if (name.startsWith("/")) name = name.substring(1);
                String full = path + "/" + name;
                if (entry.isDirectory()) {
                    html += "<a href='/browse?d=" + full + "'>" + name + "/</a><br>";
                } else {
                    html += "<a href='/sdcard" + full + "'>" + name + "</a> (" + String(entry.size()) + " bytes)<br>";
                }
                entry.close();
                entry = f.openNextFile();
            }
            f.close();
            server.send(200, "text/html", html);
            return;
        }
        size_t sz = f.size();
        String ct = "application/octet-stream";
        if (path.endsWith(".gif")) ct = "image/gif";
        else if (path.endsWith(".meta")) ct = "text/plain";
        server.setContentLength(sz);
        server.send(200, ct, "");
        uint8_t buf[4096];
        while (f.available()) {
            size_t n = f.read(buf, sizeof(buf));
            server.client().write(buf, n);
        }
        f.close();
    });
    server.on("/browse", HTTP_GET, [](){
        String d = server.arg("d");
        if (d.length() == 0) d = "/";
        if (!storeFS->exists(d)) { server.send(404, "text/plain", "Dir not found"); return; }
        File dir = storeFS->open(d);
        if (!dir || !dir.isDirectory()) { if (dir) dir.close(); server.send(400, "text/plain", "Not a directory"); return; }
        String html = "<html><head><title>Browse " + d + "</title></head>";
        html += "<body style='font-family:monospace;background:#0d1117;color:#e6edf3;padding:16px'>";
        html += "<h2>SD: " + d + "</h2>";
        if (d != "/") html += "<a style='color:#58a6ff' href='/browse?d=/'>/ (root)</a><br>";
        File entry = dir.openNextFile();
        while (entry) {
            String name = String(entry.name());
            if (name.startsWith("/")) name = name.substring(1);
            String full = d.endsWith("/") ? d + name : d + "/" + name;
            if (entry.isDirectory()) {
                html += "<a style='color:#ff6b6b' href='/browse?d=" + full + "'>" + name + "/</a><br>";
            } else {
                html += "<a style='color:#58a6ff' href='/sdcard" + full + "'>" + name + "</a>";
                html += " <span style='color:#8b949e'>(" + String(entry.size()) + ")</span>";
                html += " <a style='color:#f85149;font-size:.8em' href='#' onclick=\"fetch('/del?f=" + full + "').then(function(r){return r.text()}).then(function(){location.reload()});return false\">[del]</a><br>";
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
        html += "<br><hr><a style='color:#58a6ff' href='/'>Back to Player</a></body></html>";
        server.send(200, "text/html", html);
    });
    server.begin();
    Serial.println("Web server ready");

    server.enableCORS(true);

    xTaskCreatePinnedToCore(networkTask, "net", 8192, NULL, 1, NULL, 0);
    Serial.println("Network task on core 0, display on core 1");

    if (wifiIsAP) {
        weatherTemp = 72;
        weatherHumidity = 45;
        wxOverlayOn = true;
        wxOverlayRender();
        Serial.print("[DBG] AP mode: fake weather overlay enabled\r\n");
    }

    if (animTotal > 0) {
        playAnimByIndex(0);
        Serial.printf("Auto-playing: %s\r\n", animList[0].c_str());
    }
}

static String serialBuf;

void handleSerial() {
    if (!Serial.available()) return;

    bool lineReady = false;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            Serial.print("\r\n");
            lineReady = true;
            break;
        }
        if (c == 0x7F || c == '\b') {
            if (serialBuf.length() > 0) {
                serialBuf.remove(serialBuf.length() - 1);
                Serial.print("\b \b");
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            serialBuf += c;
            Serial.write(c);
        }
    }
    if (!lineReady) return;

    String line = serialBuf;
    serialBuf = "";
    line.trim();
    if (line.length() == 0) return;

    if (line.startsWith("WIFI:")) {
        String payload = line.substring(5);
        int sep = payload.indexOf(':');
        if (sep < 0) {
            Serial.print("ERR: format is WIFI:ssid:password\r\n");
            return;
        }
        String ssid = payload.substring(0, sep);
        String pass = payload.substring(sep + 1);
        if (ssid.length() == 0) {
            Serial.print("ERR: SSID cannot be empty\r\n");
            return;
        }
        if (pass.length() == 0) {
            Serial.print("ERR: password cannot be empty\r\n");
            return;
        }
        Serial.printf("SSID: [%s] (%d chars)\r\n", ssid.c_str(), ssid.length());
        Serial.printf("PASS: [%d chars]\r\n", pass.length());
        prefs.begin(NVS_NS, false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();
        Serial.print("Saved to NVS. Rebooting...\r\n");
        Serial.flush();
        delay(500);
        ESP.restart();
    } else if (line == "WIFISTATUS") {
        Serial.printf("Mode: %s\r\n", wifiIsAP ? "AP" : "STA");
        Serial.printf("SSID: %s\r\n", nvsSsid.length() > 0 ? nvsSsid.c_str() : "(none)");
        if (!wifiIsAP)
            Serial.printf("IP: %s\r\n", WiFi.localIP().toString().c_str());
        else
            Serial.printf("AP IP: %s\r\n", WiFi.softAPIP().toString().c_str());
    } else if (line == "WIFICLEAR") {
        prefs.begin(NVS_NS, false);
        prefs.clear();
        prefs.end();
        Serial.print("WiFi credentials cleared. Rebooting...\r\n");
        delay(500);
        ESP.restart();
    } else if (line.startsWith("DEBUG:")) {
        String payload = line.substring(6);
        int sep = payload.indexOf(':');
        if (sep < 0) {
            Serial.printf("Levels: web=%d api=%d gfx=%d (0=off 1=err 2=info 3=verbose)\r\n", dbgWeb, dbgApi, dbgGfx);
            return;
        }
        String area = payload.substring(0, sep);
        int lvl = payload.substring(sep + 1).toInt();
        if (lvl < 0) lvl = 0; if (lvl > 3) lvl = 3;
        if (area == "web") dbgWeb = lvl;
        else if (area == "api") dbgApi = lvl;
        else if (area == "gfx") dbgGfx = lvl;
        else if (area == "all") { dbgWeb = lvl; dbgApi = lvl; dbgGfx = lvl; }
        else { Serial.print("Areas: web api gfx all\r\n"); return; }
        Serial.printf("DEBUG %s=%d\r\n", area.c_str(), lvl);
    } else if (line.startsWith("TOKEN:")) {
        oauthToken = line.substring(6);
        prefs.begin(NVS_CFG, false);
        prefs.putString("oauth", oauthToken);
        prefs.end();
        apiLastFetch = 0;
        Serial.printf("OAuth token saved (%d chars)\r\n", oauthToken.length());
    } else if (line.startsWith("WXLOC:")) {
        String payload = line.substring(6);
        int sep = payload.indexOf(':');
        if (sep > 0) {
            weatherLat = payload.substring(0, sep);
            weatherLon = payload.substring(sep + 1);
            prefs.begin(NVS_CFG, false);
            prefs.putString("wxlat", weatherLat);
            prefs.putString("wxlon", weatherLon);
            prefs.end();
            Serial.printf("Weather location: %s, %s\r\n", weatherLat.c_str(), weatherLon.c_str());
            fetchWeather();
        } else {
            Serial.print("Usage: WXLOC:lat:lon (e.g. WXLOC:40.71:-74.00)\r\n");
        }
    } else if (line.startsWith("NTP:")) {
        ntpServer = line.substring(4);
        prefs.begin(NVS_CFG, false);
        prefs.putString("ntp", ntpServer);
        prefs.end();
        configTzTime(tzString.c_str(), ntpServer.c_str());
        Serial.printf("NTP server: %s\r\n", ntpServer.c_str());
    } else if (line.startsWith("TZ:")) {
        tzString = line.substring(3);
        prefs.begin(NVS_CFG, false);
        prefs.putString("tz", tzString);
        prefs.end();
        configTzTime(tzString.c_str(), ntpServer.c_str());
        Serial.printf("Timezone: %s\r\n", tzString.c_str());
    } else if (line == "TIME") {
        struct tm t;
        if (getLocalTime(&t, 100)) {
            Serial.printf("%04d-%02d-%02d %02d:%02d:%02d %s\r\n", t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, tzString.c_str());
        } else {
            Serial.print("Time not synced\r\n");
        }
    } else if (line == "STATUS") {
        struct tm t; bool hasTime = getLocalTime(&t, 100);
        Serial.printf("Mode: %s  Paused: %s\r\n", playMode == PLAY_GIF ? "GIF" : playMode == PLAY_SPRITE ? "SPRITE" : "IDLE", paused ? "yes" : "no");
        Serial.printf("Anims: %d  Current: %d\r\n", animTotal, animIndex);
        Serial.printf("Heap: %u  PSRAM: %u\r\n", ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.printf("WiFi: %s  IP: %s\r\n", wifiIsAP ? "AP" : "STA", wifiIsAP ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str());
        if (hasTime) Serial.printf("Time: %02d:%02d:%02d  NTP: %s\r\n", t.tm_hour, t.tm_min, t.tm_sec, ntpServer.c_str());
        Serial.printf("Usage: session=%.1f%% weekly=%.1f%%\r\n", apiSessionPct, apiWeeklyPct);
        Serial.printf("Weather: %.1fF %.0f%%\r\n", weatherTemp, weatherHumidity);
    } else if (line.startsWith("PLAY:")) {
        int idx = line.substring(5).toInt();
        if (idx >= 0 && idx < animTotal) {
            playAnimByIndex(idx);
            Serial.printf("Playing: [%d] %s\r\n", idx, animList[idx].c_str());
        } else {
            Serial.printf("ERR: index 0-%d\r\n", animTotal - 1);
        }
    } else if (line == "NEXT") {
        playNext();
        Serial.printf("Next: [%d] %s\r\n", animIndex, animList[animIndex].c_str());
    } else if (line == "PREV") {
        playPrev();
        Serial.printf("Prev: [%d] %s\r\n", animIndex, animList[animIndex].c_str());
    } else if (line == "PAUSE") {
        togglePause();
        Serial.printf("%s\r\n", paused ? "Paused" : "Playing");
    } else if (line == "STOP") {
        stopAll();
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);
        panel.setBrightness(1);
        Serial.print("Stopped\r\n");
    } else if (line.startsWith("BG:")) {
        uint32_t c = strtoul(line.substring(3).c_str(), NULL, 16);
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(c), 0);
        lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
        Serial.printf("BG: #%06X\r\n", c);
    } else if (line.startsWith("ZOOM:")) {
        int t = line.substring(5).toInt();
        if (t < 200) t = 200;
        if (t > 478) t = 478;
        zoomTarget = (uint16_t)t;
        if (animIndex >= 0) playAnimByIndex(animIndex);
        Serial.printf("Zoom target: %dpx\r\n", zoomTarget);
    } else if (line == "LIST") {
        for (int i = 0; i < animTotal; i++) {
            Serial.printf("  [%d] %s%s\r\n", i, animList[i].c_str(), i == animIndex ? " *" : "");
        }
        Serial.printf("%d animations\r\n", animTotal);
    } else if (line == "SNAP") {
        lv_disp_t *d = lv_disp_get_default();
        lv_disp_draw_buf_t *db = lv_disp_get_draw_buf(d);
        uint16_t w = panel.width(), h = panel.height();
        size_t sz = (size_t)w * h * 2;
        bool wasDetected = gifBgDetected;
        gifBgDetected = false;
        d->driver->full_refresh = 1;
        lv_obj_invalidate(lv_scr_act());
        lv_obj_invalidate(lv_layer_top());
        lv_timer_handler();
        d->driver->full_refresh = 0;
        gifBgDetected = wasDetected;
        if (db) {
            lv_color_t *rendered = (db->buf_act == db->buf1)
                ? (lv_color_t *)db->buf2 : (lv_color_t *)db->buf1;
            File sf = storeFS->open("/snap.raw", "w");
            if (sf) {
                sf.write((uint8_t *)rendered, sz);
                sf.close();
                Serial.printf("SNAP: saved %u bytes to /snap.raw\r\n", sz);
            } else {
                Serial.print("SNAP: file open failed\r\n");
            }
        }
    } else if (line == "DIAG") {
        fbLocked = false;
        stopAll();
        lv_obj_clean(lv_scr_act());
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x222222), 0);
        lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, 0);
        lv_obj_t *bar = lv_obj_create(lv_scr_act());
        lv_obj_set_size(bar, 300, 40);
        lv_obj_center(bar);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0xff6b6b), 0);
        lv_obj_set_style_radius(bar, 8, 0);
        lv_obj_t *lbl = lv_label_create(bar);
        lv_label_set_text(lbl, "DIAG OK");
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_center(lbl);
        panel.setBrightness(16);
        wxOverlayRender();
        Serial.printf("DIAG: fbLocked=%d heap=%u psram=%u\r\n", fbLocked, ESP.getFreeHeap(), ESP.getFreePsram());
    } else if (line == "WXSHOW") {
        wxOverlayOn = !wxOverlayOn;
        wxOverlayUpdate();
        Serial.printf("Weather overlay: %s\r\n", wxOverlayOn ? "ON" : "OFF");
    } else if (line.startsWith("WXPOS:")) {
        int p = line.substring(6).toInt();
        if (p >= 0 && p <= 3) {
            wxOverlayPos = (uint8_t)p;
            wxOverlayUpdate();
            const char *names[] = {"top-left", "top-right", "bottom-left", "bottom-right"};
            Serial.printf("Weather overlay position: %s\r\n", names[p]);
        } else {
            Serial.print("Usage: WXPOS:0-3 (0=TL 1=TR 2=BL 3=BR)\r\n");
        }
    } else if (line == "?" || line == "help") {
        Serial.print("Commands:\r\n");
        Serial.print("  PLAY:N           Play animation by index\r\n");
        Serial.print("  NEXT / PREV      Next/previous animation\r\n");
        Serial.print("  PAUSE            Toggle pause\r\n");
        Serial.print("  STOP             Stop playback\r\n");
        Serial.print("  LIST             Show all animations\r\n");
        Serial.print("  BG:RRGGBB        Set background color\r\n");
        Serial.print("  ZOOM:N           Set zoom target (200-478px)\r\n");
        Serial.print("  WIFI:ssid:pass   Set WiFi credentials\r\n");
        Serial.print("  WIFISTATUS       Show WiFi mode and IP\r\n");
        Serial.print("  WIFICLEAR        Clear WiFi, reboot\r\n");
        Serial.print("  DEBUG:area:lvl   Set debug (web/api/gfx/all, 0-3)\r\n");
        Serial.print("  STATUS           System status dump\r\n");
        Serial.print("  TOKEN:oauth      Save API OAuth token\r\n");
        Serial.print("  WXLOC:lat:lon    Set weather location\r\n");
        Serial.print("  NTP:server       Set NTP server\r\n");
        Serial.print("  TZ:posix         Set timezone\r\n");
        Serial.print("  TIME             Show current time\r\n");
        Serial.print("  WXSHOW           Toggle weather overlay\r\n");
        Serial.print("  WXPOS:0-3        Overlay position (TL/TR/BL/BR)\r\n");
        Serial.print("  DIAG             Test display + reset state\r\n");
    } else {
        Serial.print("Unknown command. Type ? for help\r\n");
    }
}

static uint32_t autoAdvanceMs = 10000;

void networkTask(void *param) {
    uint32_t wifiRetryLast = 0;
    for (;;) {
        server.handleClient();

        if (wifiIsAP && nvsSsid.length() > 0) {
            uint32_t now = millis();
            if (now - wifiRetryLast >= 30000) {
                wifiRetryLast = now;
                WiFi.mode(WIFI_STA);
                WiFi.begin(nvsSsid.c_str(), nvsPass.c_str());
                uint32_t start = millis();
                while (WiFi.status() != WL_CONNECTED && (millis() - start) < 10000) {
                    vTaskDelay(pdMS_TO_TICKS(250));
                }
                if (WiFi.status() == WL_CONNECTED) {
                    wifiIsAP = false;
                    Serial.printf("[WIFI] reconnected: %s\r\n", WiFi.localIP().toString().c_str());
                    configTzTime(tzString.c_str(), ntpServer.c_str());
                }
            }
        }

        if (oauthToken.length() > 0 && !wifiIsAP) {
            uint32_t now = millis();
            if (apiLastFetch == 0 || (now - apiLastFetch) >= API_FETCH_INTERVAL) {
                apiLastFetch = now;
                fetchApiUsage();
            }
        }

        if (weatherLat.length() > 0 && !wifiIsAP) {
            uint32_t now = millis();
            if (weatherLastFetch == 0 || (now - weatherLastFetch) >= WEATHER_INTERVAL) {
                weatherLastFetch = now;
                fetchWeather();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void loop() {
    if (screenshotSnap && !screenshotReady) {
        if (wxOverlayDirty) wxOverlayRender();
        bool wasBgDet = gifBgDetected;
        gifBgDetected = false;
        lv_disp_t *d = lv_disp_get_default();
        d->driver->full_refresh = 1;
        lv_obj_invalidate(lv_scr_act());
        lv_obj_invalidate(lv_layer_top());
        lv_timer_handler();
        d->driver->full_refresh = 0;
        gifBgDetected = wasBgDet;
        lv_disp_draw_buf_t *db = lv_disp_get_draw_buf(d);
        if (db) {
            lv_color_t *rendered = (db->buf_act == db->buf1)
                ? (lv_color_t *)db->buf2 : (lv_color_t *)db->buf1;
            memcpy((void *)screenshotSnap, rendered, (size_t)panel.width() * panel.height() * 2);
        }
        screenshotReady = true;
    }
    if (wxOverlayDirty) wxOverlayRender();
    lv_timer_handler();

    if (gifBgDetectCountdown > 0 && playMode == PLAY_GIF && gifWidget) {
        gifBgDetectCountdown--;
        if (gifBgDetectCountdown == 0) {
            lv_disp_t *d = lv_disp_get_default();
            bool wasBgDetected = gifBgDetected;
            gifBgDetected = false;
            d->driver->full_refresh = 1;
            lv_obj_invalidate(lv_scr_act());
            lv_timer_handler();
            d->driver->full_refresh = 0;
            gifBgDetected = wasBgDetected;
            lv_disp_draw_buf_t *db = lv_disp_get_draw_buf(d);
            if (db) {
                lv_color_t *rendered = (db->buf_act == db->buf1)
                    ? (lv_color_t *)db->buf2 : (lv_color_t *)db->buf1;
                uint16_t *px = (uint16_t *)rendered;
                int sw = panel.width();
                uint16_t scr16 = lv_color_to16(lv_obj_get_style_bg_color(lv_scr_act(), 0));
                uint16_t zoom = lv_img_get_zoom(gifWidget);
                lv_area_t wa;
                lv_obj_get_coords(gifWidget, &wa);
                int cx = (wa.x1 + wa.x2) / 2, cy = (wa.y1 + wa.y2) / 2;
                int ow = wa.x2 - wa.x1 + 1, oh = wa.y2 - wa.y1 + 1;
                int gw = (int)((uint32_t)ow * zoom / 256);
                int gh = (int)((uint32_t)oh * zoom / 256);
                int gx = cx - gw / 2, gy = cy - gh / 2;
                if (gx < 0) gx = 0; if (gy < 0) gy = 0;
                if (gx + gw > sw) gw = sw - gx;
                if (gy + gh > sw) gh = sw - gy;
                uint32_t counts[8] = {0};
                uint16_t colors[8] = {0};
                int nc = 0;
                auto samp = [&](int sx, int sy) {
                    uint16_t c = px[sy * sw + sx];
                    if (c == scr16) return;
                    for (int i = 0; i < nc; i++) {
                        if (colors[i] == c) { counts[i]++; return; }
                    }
                    if (nc < 8) { colors[nc] = c; counts[nc] = 1; nc++; }
                };
                for (int x = gx; x < gx + gw; x++) { samp(x, gy); samp(x, gy + gh - 1); }
                for (int y = gy + 1; y < gy + gh - 1; y++) { samp(gx, y); samp(gx + gw - 1, y); }
                if (nc > 0) {
                    int best = 0;
                    for (int i = 1; i < nc; i++) { if (counts[i] > counts[best]) best = i; }
                    if (counts[best] >= 50) {
                        gifBg16 = colors[best];
                        gifBgDetected = true;
                    }
                    Serial.printf("[GIF] bg detect: 565=0x%04X count=%u area=%d,%d %dx%d %s\r\n",
                        colors[best], counts[best], gx, gy, gw, gh, gifBgDetected ? "ON" : "skip");
                }
            }
        }
    }

    handleTouch();
    handleSerial();

    if (!paused && animTotal > 1 && playMode != PLAY_NONE) {
        uint32_t now = millis();
        if (now - lastAdvance >= autoAdvanceMs) {
            lastAdvance = now;
            playNext();
        }
    }

    delay(2);
}