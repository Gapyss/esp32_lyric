// Daily-image screens (xkcd comic + NASA APOD), ported from
// firmware/main/comic_screen.cpp (ESP32) with the fetch pipeline redesigned
// for this chip's RAM budget:
//
//   - The HTTPS metadata APIs (xkcd.com / api.nasa.gov) are out of reach --
//     BearSSL wants ~22 KB of contiguous heap this firmware does not have.
//     The Mac daemon resolves them and pushes image URL + title to /daily.
//   - The image itself is fetched over PLAIN HTTP through the wsrv.nl resize
//     proxy, which shrinks any source (multi-MB PNG/JPEG) to a <=240x168
//     baseline JPEG of a few KB, fetched server-side over HTTPS by the proxy.
//   - The JPEG is stream-decoded with the vendored TJpgDec (tjpgd.c, ChaN)
//     straight off the socket -- never buffered whole -- and Bayer-dithered
//     into a shared 1-bit frame (5 KB), drawn as Tend ink on paper exactly
//     like the ESP32's 1-bit panel rendered it.
//
// The frame buffer is shared between the two screens; switching comic<->apod
// refetches (a couple of seconds over plain HTTP). The fetch runs from the
// tick, not the HTTP handler, so mode switches respond instantly.
#include "tend.h"

#include <ESP8266WiFi.h>
extern "C" {
#include "tjpgd.h"
}

static const int IMG_MAX_W = 240;
static const int IMG_MAX_H = 168;
static const int IMG_TOP = 20;
static const char *WSRV_HOST = "wsrv.nl";
static const unsigned long FETCH_TIMEOUT_MS = 20000;

enum DailyState : uint8_t { D_NOMETA, D_PENDING, D_LOADING, D_READY, D_ERROR };

struct DailyMeta {
  String img;
  String title;
  String extra;   // comic: "#1234" number; apod: date
  DailyState state;
  String error;
};

static DailyMeta g_meta[2];   // [0] comic, [1] apod
static uint8_t g_frameBits[(IMG_MAX_W / 8) * IMG_MAX_H];
static int g_frameW = 0;
static int g_frameH = 0;
static int8_t g_frameFor = -1;   // which meta the frame holds; -1 = none

static const uint8_t BAYER[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

const char *dailyStateName(bool apod) {
  switch (g_meta[apod ? 1 : 0].state) {
    case D_PENDING: return "pending";
    case D_LOADING: return "loading";
    case D_READY:   return "ready";
    case D_ERROR:   return "error";
    default:        return "nometa";
  }
}

String dailyTitle(bool apod) { return g_meta[apod ? 1 : 0].title; }
String dailyExtra(bool apod) { return g_meta[apod ? 1 : 0].extra; }

void dailySetMeta(bool apod, const String &imgUrl, const String &title,
                  const String &extra) {
  DailyMeta &m = g_meta[apod ? 1 : 0];
  const bool changed = m.img != imgUrl;
  m.img = imgUrl;
  m.title = title;
  m.extra = extra;
  if (imgUrl.length() == 0) {
    m.state = D_NOMETA;
  } else if (changed || m.state == D_NOMETA || m.state == D_ERROR) {
    m.state = D_PENDING;
    if (changed && g_frameFor == (apod ? 1 : 0)) g_frameFor = -1;
  }
}

void dailyRefresh(bool apod) {
  DailyMeta &m = g_meta[apod ? 1 : 0];
  if (m.img.length()) {
    m.state = D_PENDING;
    g_frameFor = -1;
  }
}

// ---- streaming JPEG fetch --------------------------------------------------

struct FetchCtx {
  WiFiClient *client;
  unsigned long deadline;
  int rowBytes;
  bool overflow;
};

// tjpgd input callback: read (or skip) bytes from the socket, waiting briefly
// for more when the pipe runs dry. Returning 0 aborts the decode.
static size_t jpegIn(JDEC *jd, uint8_t *buf, size_t len) {
  FetchCtx *ctx = (FetchCtx *)jd->device;
  size_t got = 0;
  while (got < len) {
    if (millis() > ctx->deadline) return got == 0 ? 0 : got;
    int avail = ctx->client->available();
    if (avail <= 0) {
      if (!ctx->client->connected()) break;   // stream ended
      delay(1);
      yield();
      continue;
    }
    size_t chunk = len - got;
    if ((size_t)avail < chunk) chunk = (size_t)avail;
    if (buf) {
      int r = ctx->client->read(buf + got, chunk);
      if (r <= 0) break;
      got += (size_t)r;
    } else {
      // Skip: read into a small scratch.
      uint8_t scratch[32];
      size_t want = chunk > sizeof(scratch) ? sizeof(scratch) : chunk;
      int r = ctx->client->read(scratch, want);
      if (r <= 0) break;
      got += (size_t)r;
    }
  }
  return got;
}

// tjpgd output callback: grayscale block -> Bayer dither -> 1-bit frame
// (MSB-first rows, stride rowBytes), Tend ink semantics: bit set = ink.
static int jpegOut(JDEC *jd, void *bitmap, JRECT *rect) {
  FetchCtx *ctx = (FetchCtx *)jd->device;
  const uint8_t *pix = (const uint8_t *)bitmap;
  for (int y = rect->top; y <= rect->bottom; y++) {
    if (y >= IMG_MAX_H) break;
    for (int x = rect->left; x <= rect->right; x++, pix++) {
      if (x >= IMG_MAX_W) continue;
      const int threshold = BAYER[y & 3][x & 3] * 16 + 8;
      uint8_t *bytePtr = &g_frameBits[y * ctx->rowBytes + (x >> 3)];
      const uint8_t mask = (uint8_t)(0x80U >> (x & 7));
      if (*pix < threshold) *bytePtr |= mask;
      else *bytePtr &= (uint8_t)~mask;
    }
  }
  return 1;
}

// Percent-encode a URL for use as the wsrv `url` query value.
static String urlEncode(const String &s) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() + 16);
  for (unsigned int i = 0; i < s.length(); i++) {
    const char c = s[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == ':') {
      out += c;
    } else {
      out += '%';
      out += hex[((uint8_t)c) >> 4];
      out += hex[((uint8_t)c) & 0x0F];
    }
  }
  return out;
}

// Blocking fetch + decode of the current meta's image into g_frameBits.
// Returns true on success; on failure fills `err`.
static bool fetchImage(int idx, String &err) {
  const DailyMeta &m = g_meta[idx];
  if (WiFi.status() != WL_CONNECTED) {
    err = "no wifi";
    return false;
  }

  WiFiClient client;
  client.setTimeout(5000);
  if (!client.connect(WSRV_HOST, 80)) {
    err = "proxy connect failed";
    return false;
  }

  // HTTP/1.0 => identity encoding, connection closed at end of body.
  String path = "/?url=" + urlEncode(m.img) +
                "&w=" + String(IMG_MAX_W) + "&h=" + String(IMG_MAX_H) +
                "&fit=inside&output=jpg&q=60&bg=white";
  client.print(String("GET ") + path + " HTTP/1.0\r\nHost: " + WSRV_HOST +
               "\r\nUser-Agent: clawdmeter-esp8266\r\nConnection: close\r\n\r\n");

  // Status line + headers.
  String status = client.readStringUntil('\n');
  if (status.indexOf("200") < 0) {
    err = "proxy http error";
    client.stop();
    return false;
  }
  while (true) {
    String line = client.readStringUntil('\n');
    if (line.length() == 0 || line == "\r") break;
    yield();
  }

  FetchCtx ctx = {};
  ctx.client = &client;
  ctx.deadline = millis() + FETCH_TIMEOUT_MS;

  void *pool = malloc(TJPGD_WORKSPACE_SIZE);
  if (pool == NULL) {
    err = "low memory";
    client.stop();
    return false;
  }

  JDEC jd = {};
  JRESULT res = jd_prepare(&jd, jpegIn, pool, TJPGD_WORKSPACE_SIZE, &ctx);
  if (res != JDR_OK) {
    free(pool);
    client.stop();
    err = "jpeg header err " + String((int)res);
    return false;
  }

  // The proxy already fits inside 240x168; scale down defensively if not.
  uint8_t scale = 0;
  while (scale < 3 && ((jd.width >> scale) > IMG_MAX_W || (jd.height >> scale) > IMG_MAX_H)) {
    scale++;
  }
  const int outW = jd.width >> scale;
  const int outH = jd.height >> scale;
  ctx.rowBytes = (outW + 7) / 8;

  memset(g_frameBits, 0, sizeof(g_frameBits));
  res = jd_decomp(&jd, jpegOut, scale);
  free(pool);
  client.stop();
  if (res != JDR_OK) {
    err = "jpeg decode err " + String((int)res);
    return false;
  }

  g_frameW = outW > IMG_MAX_W ? IMG_MAX_W : outW;
  g_frameH = outH > IMG_MAX_H ? IMG_MAX_H : outH;
  g_frameFor = (int8_t)idx;
  return true;
}

// ---- rendering -------------------------------------------------------------

static int activeIdx() { return lcdScreen == SCREEN_APOD ? 1 : 0; }

static void drawHeaderLine(int idx) {
  const DailyMeta &m = g_meta[idx];
  gfx->fillRect(0, 0, 240, 16, C_TND_PAPER);
  gfx->setTextSize(1);
  gfx->setTextColor(C_TND_MUTE, C_TND_PAPER);
  gfx->setCursor(8, 5);
  if (idx == 0) {
    gfx->print(m.extra.length() ? "DAILY COMIC - XKCD #" + m.extra : String("DAILY COMIC - XKCD"));
  } else {
    gfx->print(m.extra.length() ? "NASA APOD - " + m.extra : String("NASA APOD"));
  }
  if (m.state == D_LOADING) printRight(232, 5, 1, "FETCHING", C_TND_EMBER, C_TND_PAPER);
}

// Bottom title, wrapped to at most two centered lines with an ellipsis.
static void drawTitle(int idx) {
  const DailyMeta &m = g_meta[idx];
  gfx->fillRect(0, 214, 240, 26, C_TND_PAPER);
  String t = m.title;
  if (!t.length()) return;
  const int maxChars = 236 / 6;
  if ((int)t.length() <= maxChars) {
    printCentered(220, 1, t, C_TND_INK, C_TND_PAPER);
    return;
  }
  int breakAt = -1;
  for (int i = maxChars; i >= maxChars / 2; i--) {
    if (t[i] == ' ') { breakAt = i; break; }
  }
  if (breakAt < 0) breakAt = maxChars;
  String l1 = t.substring(0, breakAt);
  String l2 = t.substring(t[breakAt] == ' ' ? breakAt + 1 : breakAt);
  if ((int)l2.length() > maxChars) l2 = l2.substring(0, maxChars - 3) + "...";
  printCentered(215, 1, l1, C_TND_INK, C_TND_PAPER);
  printCentered(227, 1, l2, C_TND_INK, C_TND_PAPER);
}

static void drawFrame() {
  const int x = (240 - g_frameW) / 2;
  const int y = IMG_TOP + (IMG_MAX_H - g_frameH) / 2;
  gfx->fillRect(0, 16, 240, IMG_MAX_H + 8, C_TND_PAPER);
  gfx->drawBitmap(x, y, g_frameBits, g_frameW, g_frameH, C_TND_INK, C_TND_PAPER);
}

static void drawPlaceholder(int idx, const char *line1, const char *line2) {
  gfx->fillRect(0, 16, 240, IMG_MAX_H + 8, C_TND_PAPER);
  gfx->drawRoundRect(14, 30, 212, 140, 8, C_TND_LINE);
  printCentered(90, 1, line1, C_TND_INK_SOFT, C_TND_PAPER);
  if (line2 != NULL) printCentered(104, 1, line2, C_TND_FAINT, C_TND_PAPER);
  (void)idx;
}

void dailyScreenBegin() {
  const int idx = activeIdx();
  DailyMeta &m = g_meta[idx];
  gfx->fillScreen(C_TND_PAPER);

  // A READY meta whose frame belongs to the other screen must refetch (the
  // 1-bit frame is shared to spare RAM).
  if (m.state == D_READY && g_frameFor != idx) m.state = D_PENDING;

  drawHeaderLine(idx);
  drawTitle(idx);

  switch (m.state) {
    case D_READY:
      drawFrame();
      break;
    case D_NOMETA:
      drawPlaceholder(idx, "waiting for the daemon", "it pushes image metadata to /daily");
      break;
    case D_ERROR:
      drawPlaceholder(idx, "image unavailable", m.error.c_str());
      break;
    default:
      // PENDING/LOADING -- the tick starts the fetch so the mode-switch
      // request that brought us here can respond first.
      drawPlaceholder(idx, "fetching image...", NULL);
      break;
  }
}

void dailyScreenTick() {
  const int idx = activeIdx();
  DailyMeta &m = g_meta[idx];
  if (m.state != D_PENDING) return;

  m.state = D_LOADING;
  drawHeaderLine(idx);

  String err;
  if (fetchImage(idx, err)) {
    m.state = D_READY;
    m.error = "";
  } else {
    m.state = D_ERROR;
    m.error = err;
    g_frameFor = -1;
  }
  // Full repaint with the result (also clears the FETCHING flag).
  dailyScreenBegin();
}
