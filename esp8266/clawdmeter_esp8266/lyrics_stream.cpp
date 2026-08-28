// Lyrics frame stream — the ESP8266 side of the Mac's lyrics_display_daemon.py.
//
// This is the unauthenticated proto=1 client. The ESP8266 does NOT have the heap
// for the secure proto=2 (SEC2) path the ESP32 uses: SEC2 must buffer a whole
// record to verify its HMAC before the frame is trusted, which means a second
// ~7.2 KB buffer on top of the framebuffer. Two big buffers plus the on-device
// marquee canvases exhaust the heap the WiFi SDK needs and the radio crashes
// (ieee80211_parse_beacon, StoreProhibited on a NULL alloc). So on this chip we
// drop authentication and stream frames straight into ONE framebuffer.
//
// Our daemon is daemon/lyrics_display_esp8266.py, a separate process from the
// ESP32's: it runs --insecure (no identity), so it speaks proto=1, sends a
// plaintext hello, and pushes bare LYR1 envelopes (no SEC2 wrapper). Splitting
// it out is what keeps the ESP32's proto=2 authentication intact — one shared
// daemon in --insecure mode used to downgrade both boards. We dial the Mac whose
// IP arrives on /nowplaying (lyricsStreamNoteHost) — the daemon address is never
// configured here, but LYR_PORT below is, which is why that process takes :8766
// and the ESP32 daemon (found over mDNS) moved to :8767.
//
// This module keeps a hand-rolled WebSocket to the daemon's board port (:8766)
// for as long as the board is up -- there is no other screen to switch away to. The daemon renders 240x240 1-bpp frames with
// Core Text (real Thai shaping, karaoke) — full frames and dirty-rect deltas.
// Set bits are Tend paper; clear bits are colorized by semantic screen region
// (ember label/progress, soft metadata, moss playback state, primary lyric).
// Color-capable clients additionally negotiate ART1: real RGB565 album art is
// streamed directly to the TFT one row at a time after the mono UI mask. This
// preserves the cover's source colors without a 115 KB color framebuffer.
//
// Memory: exactly one framebuffer (7200 B), malloc'd on the first stream and
// freed on lyricsStreamStop(). Incoming frame bytes are applied *incrementally*
// into it as they arrive off the socket (no separate record buffer), so peak
// heap while streaming is ~7.2 KB. The .ino frees the on-device marquee canvases
// (~17 KB) while a stream owns the panel, so the two render paths never hold heap
// at the same time.
//
// Pre-scheduled frames (swap_in_ms > 0, kind FULL_SCHED/RECT_SCHED) would need a
// second "pending" buffer to hold the future frame until its deadline — heap we
// don't have. We drop them: lyric lines flip on the daemon's next now-frame push
// (one tick coarser than on-beat) instead of pre-scheduled to the millisecond.

#include <ESP8266WiFi.h>
#include <esp8266_peri.h>   // RANDOM_REG32 — hardware RNG (valid while RF is on)
#include "tend.h"

#define LYR_PORT        8766
#define LYR_W           240
#define LYR_H           240
#define LYR_ROW_BYTES   (LYR_W / 8)
#define LYR_FRAME_BYTES (LYR_ROW_BYTES * LYR_H)
#define LYR_ENV_HDR     28          // "LYR1" + ver/kind + 7x u16 + 2x u32
#define LYR_COLOR_BYTES (LYR_W * LYR_H * 2UL)
#define LYR_MAX_PAYLOAD (LYR_ENV_HDR + LYR_COLOR_BYTES)   // full-screen ART1 overlay
#define LYR_RETRY_MS    8000UL      // reconnect backoff
#define LYR_CONNECT_TIMEOUT_MS 1200UL  // TCP connect budget (blocking; keep small)
#define LYR_HANDSHAKE_TIMEOUT_MS 3000UL
#define LYR_HELLO_TIMEOUT_MS 4000UL  // budget for the hello/ready exchange
#define LYR_HOST_FRESH_MS (10UL * 60UL * 1000UL)  // dial only a recently-seen Mac

// LYR1 envelope kinds. NOW frames are applied; SCHED frames are dropped (no
// pending buffer on this chip — see file header).
static const uint8_t LYR_KIND_FULL_NOW = 1;
static const uint8_t LYR_KIND_FULL_SCHED = 2;
static const uint8_t LYR_KIND_RECT_NOW = 3;
static const uint8_t LYR_KIND_RECT_SCHED = 4;

// Connection lifecycle.
enum LyrState : uint8_t {
  LYR_OFF = 0,
  LYR_HANDSHAKE,   // waiting for the HTTP 101
  LYR_HELLO,       // connected; waiting for the plaintext hello (then send "ready")
  LYR_OPEN,        // frames flow
};

static WiFiClient lyrClient;
static IPAddress lyrHost;
static bool lyrHostKnown = false;
static unsigned long lyrHostSeenMs = 0;
static uint8_t lyrState = LYR_OFF;
static unsigned long lyrNextAttemptMs = 0;
static unsigned long lyrHandshakeDeadlineMs = 0;
static unsigned long lyrHelloDeadlineMs = 0;

// The one framebuffer: what the panel shows. malloc'd on first stream.
static uint8_t *lyrFrame = nullptr;
static bool lyrFrameValid = false;    // stream owns the MUSIC panel

// Small control-frame buffer (hello / clear / ping). Binary frame bodies are NOT
// buffered here — they stream straight into lyrFrame.
static uint8_t lyrCtrl[256];

// Handshake response accumulator (looking for "\r\n\r\n", " 101 " on line 1).
static char lyrHsBuf[256];
static uint16_t lyrHsLen = 0;

// Non-blocking WebSocket frame reader state.
enum WsPhase : uint8_t { WS_HDR = 0, WS_PAY };
static uint8_t lyrWsPhase = WS_HDR;
static uint8_t lyrWsHdr[10];
static uint8_t lyrWsHdrGot = 0;
static uint8_t lyrWsHdrNeed = 2;
static uint8_t lyrWsOpcode = 0;
static uint32_t lyrWsPayLen = 0;
static uint32_t lyrWsPayGot = 0;

// Per-binary-frame incremental-apply state (valid while WS_PAY on opcode 2).
static uint8_t lyrEnvHdr[LYR_ENV_HDR];  // the 28-byte LYR1 header, accumulated first
static uint8_t lyrEnvHdrGot = 0;
static bool lyrEnvApply = false;        // this is a NOW frame we should paint
static bool lyrEnvFull = false;
static uint16_t lyrEnvX = 0, lyrEnvY = 0, lyrEnvRw = 0, lyrEnvRh = 0, lyrEnvRowB = 0;
static uint32_t lyrEnvBodyGot = 0;      // body bytes consumed
static uint32_t lyrEnvRowBase = 0;      // lyrFrame offset of the current row's first byte
static uint16_t lyrEnvCol = 0;          // byte within the current row
static bool lyrEnvColor = false;        // ART1 RGB565 overlay instead of LYR1 mask
static uint8_t lyrColorHigh = 0;        // first byte of a network-order RGB565 pixel
static uint16_t lyrLine[LYR_W];         // shared mono/color scanline buffer

static uint16_t lyrBe16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t lyrBe32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// ---- Panel blit -------------------------------------------------------------

// Tend semantic ink color for the compact daemon layout. The mask stays 1-bpp
// to fit ESP8266 RAM; stable layout bands provide the richer color roles.
static uint16_t lyrInkColorAt(int x, int y) {
  if (y < 25) return C_MUS_EMBER;                         // eyebrow + top rule
  if (y < 54) return C_MUS_INK;                           // title
  if (y < 84) return C_MUS_INK_SOFT;                      // artist / cover frame
  if (y < 100) {                                          // time + progress
    if (x >= 62 && x <= 178) return C_MUS_EMBER;
    return C_MUS_INK_MUTED;
  }
  if (y < 108) return C_MUS_PAPER_LINE;                   // lyric divider
  if (y < 185) return C_MUS_INK;                          // active lyric
  if (y < 224) return y < 208 ? C_MUS_INK_SOFT : C_MUS_INK_MUTED;
  return x < 112 ? C_TND_MOSS : C_MUS_INK_MUTED;          // state + maker footer
}

// 1-bpp (LSB-first, row-major) -> Tend RGB565 rows on the panel. One shared
// 240-px scanline, row by row; periodic yields keep the WiFi stack fed.
// The panel is unconditionally ours: this board has one screen. The only other
// painter is the OTA status page, and handleFirmwareUpload() calls
// lyricsStreamStop() (freeing lyrFrame) before it draws, which this null check
// is what enforces.
static void lyrBlit(int x, int y, int w, int h) {
  if (lyrFrame == nullptr) return;
  for (int row = 0; row < h; row++) {
    const uint8_t *src = lyrFrame + (unsigned)(y + row) * LYR_ROW_BYTES;
    for (int col = 0; col < w; col++) {
      int px = x + col;
      lyrLine[col] = ((src[px >> 3] >> (px & 7)) & 1)
                         ? C_MUS_PAPER : lyrInkColorAt(px, y + row);
    }
    gfx->draw16bitRGBBitmap(x, y + row, lyrLine, w, 1);
    if ((row & 31) == 31) yield();
  }
}

// ---- WebSocket send (client->server frames must be masked) ------------------

static bool lyrSendWs(uint8_t opcode, const uint8_t *payload, uint32_t len) {
  uint8_t hdr[8];
  size_t hl = 0;
  hdr[hl++] = 0x80 | opcode;
  if (len < 126) {
    hdr[hl++] = 0x80 | (uint8_t)len;
  } else if (len <= 0xFFFF) {
    hdr[hl++] = 0x80 | 126;
    hdr[hl++] = (uint8_t)(len >> 8);
    hdr[hl++] = (uint8_t)(len & 0xFF);
  } else {
    return false;   // we never send anything this large
  }
  uint8_t mask[4];
  uint32_t r = RANDOM_REG32;
  memcpy(mask, &r, 4);
  memcpy(hdr + hl, mask, 4);
  hl += 4;
  if (lyrClient.write(hdr, hl) != hl) return false;

  uint8_t scratch[128];
  uint32_t off = 0;
  while (off < len) {
    const uint32_t chunk = (len - off) < sizeof(scratch) ? (len - off) : sizeof(scratch);
    for (uint32_t i = 0; i < chunk; i++) scratch[i] = payload[off + i] ^ mask[(off + i) & 3];
    if (lyrClient.write(scratch, chunk) != chunk) return false;
    off += chunk;
  }
  return true;
}

// ---- Buffers + teardown -----------------------------------------------------

static void lyrResetWs(void) {
  lyrWsPhase = WS_HDR;
  lyrWsHdrGot = 0;
  lyrWsHdrNeed = 2;
  lyrWsPayGot = 0;
  lyrEnvHdrGot = 0;
}

// Drop the connection and everything the stream painted; the MUSIC screen's tick
// sees lyricsStreamActive() go false and repaints the on-device fallback.
static void lyrDrop(unsigned long retryDelayMs) {
  lyrClient.stop();
  lyrState = LYR_OFF;
  lyrFrameValid = false;
  lyrResetWs();
  lyrNextAttemptMs = millis() + retryDelayMs;
}

void lyricsStreamStop(void) {
  lyrDrop(0);
  free(lyrFrame);
  lyrFrame = nullptr;
}

bool lyricsStreamActive(void) {
  return lyrState == LYR_OPEN && lyrFrameValid;
}

// True while a Mac IP is known (learned from a /nowplaying or /usage push) but
// the stream hasn't reached LYR_OPEN — the "reaching out to the daemon" window
// the MUSIC screen shows before rendered frames take over the panel.
bool lyricsStreamConnecting(void) {
  return lyrHostKnown && lyrState != LYR_OPEN;
}

void lyricsStreamNoteHost(const IPAddress &host) {
  if (!host.isSet()) return;
  if (!lyrHostKnown || lyrHost != host) {
    lyrHost = host;
    lyrNextAttemptMs = millis();   // new Mac -> try promptly
  }
  lyrHostKnown = true;
  lyrHostSeenMs = millis();
}

// ---- Incremental LYR1 envelope application ----------------------------------

// The 28-byte header of a binary frame is complete in lyrEnvHdr. Parse it, decide
// whether we paint it (a NOW frame) or discard it (SCHED, or malformed), and set
// up the per-byte apply cursor. Body bytes then stream in via lyrEnvBody().
static void lyrEnvBeginBody(void) {
  lyrEnvApply = false;
  lyrEnvColor = false;
  lyrEnvBodyGot = 0;
  lyrEnvCol = 0;

  const uint8_t *d = lyrEnvHdr;
  const bool mono = memcmp(d, "LYR1", 4) == 0;
  const bool color = memcmp(d, "ART1", 4) == 0;
  if ((!mono && !color) || d[4] != 1) return;   // discard body
  const uint8_t kind = d[5];
  const uint16_t width = lyrBe16(d + 6);
  const uint16_t height = lyrBe16(d + 8);
  const uint16_t x = lyrBe16(d + 10);
  const uint16_t y = lyrBe16(d + 12);
  const uint16_t rw = lyrBe16(d + 14);
  const uint16_t rh = lyrBe16(d + 16);
  const uint16_t rowB = lyrBe16(d + 18);
  const uint32_t dataLen = lyrBe32(d + 24);

  if (width != LYR_W || height != LYR_H) return;
  if (dataLen != lyrWsPayLen - LYR_ENV_HDR) return;

  // ART1 is an immediate RGB565 rectangle. It is deliberately streamed direct
  // to the TFT after the matching LYR1 frame, never retained in ESP8266 RAM.
  if (color) {
    if (kind != LYR_KIND_RECT_NOW || rw == 0 || rh == 0 ||
        x + rw > LYR_W || y + rh > LYR_H || rowB != rw * 2U ||
        dataLen != (uint32_t)rowB * rh || !lyrFrameValid) {
      return;
    }
    lyrEnvApply = true;
    lyrEnvColor = true;
    lyrEnvX = x;
    lyrEnvY = y;
    lyrEnvRw = rw;
    lyrEnvRh = rh;
    lyrEnvRowB = rowB;
    return;
  }

  bool full;
  if (kind == LYR_KIND_FULL_NOW || kind == LYR_KIND_FULL_SCHED) {
    if (x != 0 || y != 0 || rw != LYR_W || rh != LYR_H ||
        rowB != LYR_ROW_BYTES || dataLen != LYR_FRAME_BYTES) {
      return;
    }
    full = true;
  } else if (kind == LYR_KIND_RECT_NOW || kind == LYR_KIND_RECT_SCHED) {
    if ((x & 7) != 0 || rw == 0 || rh == 0 || rowB == 0 ||
        x + rw > LYR_W || y + rh > LYR_H || dataLen != (uint32_t)rowB * rh) {
      return;
    }
    full = false;
  } else {
    return;
  }

  // Scheduled frames need a pending buffer we don't have — drop (discard body).
  if (kind == LYR_KIND_FULL_SCHED || kind == LYR_KIND_RECT_SCHED) return;

  // A now-rect must patch an existing frame; if we have none yet, zero-seed it so
  // the untouched region is ink, not garbage.
  if (lyrFrame == nullptr) {
    lyrFrame = (uint8_t *)malloc(LYR_FRAME_BYTES);
    if (lyrFrame == nullptr) return;   // no heap -> discard; blit stays on fallback
    lyrFrameValid = false;
  }
  if (!full && !lyrFrameValid) memset(lyrFrame, 0, LYR_FRAME_BYTES);

  lyrEnvApply = true;
  lyrEnvFull = full;
  lyrEnvX = x;
  lyrEnvY = y;
  lyrEnvRw = rw;
  lyrEnvRh = rh;
  lyrEnvRowB = rowB;
  lyrEnvRowBase = full ? 0 : ((uint32_t)y * LYR_ROW_BYTES + (x >> 3));
}

// Apply one streamed body byte at its position in lyrFrame (now frames only).
static inline void lyrEnvBody(uint8_t b) {
  if (lyrEnvApply) {
    if (lyrEnvColor) {
      const uint16_t rowByte = lyrEnvBodyGot % lyrEnvRowB;
      if ((rowByte & 1) == 0) {
        lyrColorHigh = b;
      } else {
        lyrLine[rowByte >> 1] = ((uint16_t)lyrColorHigh << 8) | b;
        if (rowByte + 1 == lyrEnvRowB) {
          const uint16_t row = lyrEnvBodyGot / lyrEnvRowB;
          gfx->draw16bitRGBBitmap(lyrEnvX, lyrEnvY + row, lyrLine, lyrEnvRw, 1);
          if ((row & 15) == 15) yield();
        }
      }
    } else if (lyrEnvFull) {
      lyrFrame[lyrEnvBodyGot] = b;
    } else {
      lyrFrame[lyrEnvRowBase + lyrEnvCol] = b;
      if (++lyrEnvCol >= lyrEnvRowB) { lyrEnvCol = 0; lyrEnvRowBase += LYR_ROW_BYTES; }
    }
  }
  lyrEnvBodyGot++;
}

// The whole binary frame arrived: paint it if it was a NOW frame we applied.
static void lyrEnvComplete(void) {
  if (!lyrEnvApply) return;
  if (lyrEnvColor) return;  // already painted scanline-by-scanline
  lyrFrameValid = true;
  if (lyrEnvFull) lyrBlit(0, 0, LYR_W, LYR_H);
  else lyrBlit(lyrEnvX, lyrEnvY, lyrEnvRw, lyrEnvRh);
}

// ---- Control frames (text / ping) -------------------------------------------

static void lyrHandleText(const uint8_t *payload, uint32_t len) {
  char text[128];
  const uint32_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
  memcpy(text, payload, n);
  text[n] = '\0';
  if (lyrState == LYR_HELLO && strstr(text, "\"type\":\"hello\"") != nullptr) {
    static const uint8_t ready[] = "ready";
    if (lyrSendWs(1, ready, sizeof(ready) - 1)) lyrState = LYR_OPEN;
    else lyrDrop(LYR_RETRY_MS);
  } else if (strstr(text, "\"type\":\"clear\"") != nullptr) {
    // No track: drop our frame so the MUSIC screen repaints its placeholder.
    lyrFrameValid = false;
  }
}

// ---- Non-blocking WebSocket pump --------------------------------------------

// Called when a full control-frame payload (buffered in lyrCtrl) has arrived.
static void lyrDispatchControl(void) {
  if (lyrWsOpcode == 8) { lyrDrop(LYR_RETRY_MS); return; }                 // close
  if (lyrWsOpcode == 9) { lyrSendWs(10, lyrCtrl, lyrWsPayGot); return; }   // ping -> pong
  if (lyrWsOpcode == 10) return;                                          // pong: ignore
  if (lyrWsOpcode == 1) lyrHandleText(lyrCtrl, lyrWsPayGot);              // text
}

static void lyrPump(void) {
  while (lyrClient.available() > 0) {
    int c = lyrClient.read();
    if (c < 0) break;
    const uint8_t b = (uint8_t)c;

    if (lyrWsPhase == WS_HDR) {
      lyrWsHdr[lyrWsHdrGot++] = b;
      if (lyrWsHdrGot == 2) {
        // Server frames must be FIN + unmasked (the daemon never fragments).
        if ((lyrWsHdr[0] & 0x80) == 0 || (lyrWsHdr[1] & 0x80) != 0) { lyrDrop(LYR_RETRY_MS); return; }
        const uint8_t len7 = lyrWsHdr[1] & 0x7F;
        if (len7 == 126) lyrWsHdrNeed = 4;
        else if (len7 == 127) lyrWsHdrNeed = 10;
        else lyrWsHdrNeed = 2;
      }
      if (lyrWsHdrGot < lyrWsHdrNeed) continue;

      lyrWsOpcode = lyrWsHdr[0] & 0x0F;
      const uint8_t len7 = lyrWsHdr[1] & 0x7F;
      if (len7 == 126) {
        lyrWsPayLen = lyrBe16(lyrWsHdr + 2);
      } else if (len7 == 127) {
        // 64-bit length: anything past 32 bits is bogus for our frames.
        for (int i = 0; i < 4; i++) if (lyrWsHdr[2 + i] != 0) { lyrDrop(LYR_RETRY_MS); return; }
        lyrWsPayLen = lyrBe32(lyrWsHdr + 6);
      } else {
        lyrWsPayLen = len7;
      }
      if (lyrWsPayLen > LYR_MAX_PAYLOAD) { lyrDrop(LYR_RETRY_MS); return; }
      // A control opcode (text/ping/close/pong) must fit the small buffer.
      if (lyrWsOpcode != 2 && lyrWsPayLen > sizeof(lyrCtrl)) { lyrDrop(LYR_RETRY_MS); return; }
      // A binary frame must at least carry a full envelope header.
      if (lyrWsOpcode == 2 && lyrWsPayLen < LYR_ENV_HDR) { lyrDrop(LYR_RETRY_MS); return; }
      lyrWsPayGot = 0;
      lyrEnvHdrGot = 0;
      lyrWsPhase = WS_PAY;
      if (lyrWsPayLen == 0) {   // zero-length control frame (e.g. empty ping)
        lyrDispatchControl();
        if (lyrState == LYR_OFF) return;
        lyrResetWs();
      }
      continue;
    }

    // WS_PAY.
    if (lyrWsOpcode == 2) {
      // Binary LYR1/ART1 frame: header first, then body straight into the mono
      // framebuffer or the TFT scanline buffer.
      if (lyrEnvHdrGot < LYR_ENV_HDR) {
        lyrEnvHdr[lyrEnvHdrGot++] = b;
        if (lyrEnvHdrGot == LYR_ENV_HDR) lyrEnvBeginBody();
      } else {
        lyrEnvBody(b);
      }
      lyrWsPayGot++;
      if (lyrWsPayGot >= lyrWsPayLen) {
        lyrEnvComplete();
        if (lyrState == LYR_OFF) return;
        lyrResetWs();
      }
    } else {
      // Control frame: accumulate into the small buffer, then dispatch.
      if (lyrWsPayGot < sizeof(lyrCtrl)) lyrCtrl[lyrWsPayGot] = b;
      lyrWsPayGot++;
      if (lyrWsPayGot >= lyrWsPayLen) {
        lyrDispatchControl();
        if (lyrState == LYR_OFF) return;
        lyrResetWs();
      }
    }
  }
}

// ---- HTTP 101 handshake -----------------------------------------------------

static void lyrPumpHandshake(void) {
  while (lyrClient.available() > 0) {
    int c = lyrClient.read();
    if (c < 0) break;
    if (lyrHsLen < sizeof(lyrHsBuf) - 1) lyrHsBuf[lyrHsLen++] = (char)c;
    lyrHsBuf[lyrHsLen] = '\0';
    if (lyrHsLen >= 4 && memcmp(lyrHsBuf + lyrHsLen - 4, "\r\n\r\n", 4) == 0) {
      if (strstr(lyrHsBuf, " 101 ") != nullptr) {
        lyrResetWs();
        lyrState = LYR_HELLO;
        lyrHelloDeadlineMs = millis() + LYR_HELLO_TIMEOUT_MS;
      } else {
        lyrDrop(LYR_RETRY_MS);
      }
      return;
    }
    if (lyrHsLen >= sizeof(lyrHsBuf) - 1) { lyrDrop(LYR_RETRY_MS); return; }
  }
}

static void lyrTryConnect(void) {
  unsigned long now = millis();
  if (!lyrHostKnown || now - lyrHostSeenMs > LYR_HOST_FRESH_MS) return;
  if ((long)(now - lyrNextAttemptMs) < 0) return;
  lyrNextAttemptMs = now + LYR_RETRY_MS;

  lyrClient.setTimeout(LYR_CONNECT_TIMEOUT_MS);
  if (!lyrClient.connect(lyrHost, LYR_PORT)) {
    lyrClient.stop();
    return;
  }
  lyrClient.setNoDelay(true);

  char request[256];
  snprintf(request, sizeof(request),
           "GET /board?proto=1&w=%d&h=%d&color=rgb565 HTTP/1.1\r\n"
           "Host: %s:%d\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Version: 13\r\n"
           "Sec-WebSocket-Key: Y2xhd2RtZXRlci1seXJpY3M=\r\n"
           "\r\n",
           LYR_W, LYR_H, lyrHost.toString().c_str(), LYR_PORT);
  if (lyrClient.write(request, strlen(request)) != strlen(request)) {
    lyrDrop(LYR_RETRY_MS);
    return;
  }
  lyrHsLen = 0;
  lyrHandshakeDeadlineMs = millis() + LYR_HANDSHAKE_TIMEOUT_MS;
  lyrState = LYR_HANDSHAKE;
}

void lyricsStreamTick(void) {
  if (WiFi.status() != WL_CONNECTED) return;

  if (lyrState == LYR_OFF) {
    lyrTryConnect();
    if (lyrState == LYR_OFF) return;
  }

  if (!lyrClient.connected() && lyrClient.available() <= 0) {
    lyrDrop(LYR_RETRY_MS);
    return;
  }

  if (lyrState == LYR_HANDSHAKE) {
    lyrPumpHandshake();
    if (lyrState == LYR_HANDSHAKE && (long)(millis() - lyrHandshakeDeadlineMs) >= 0) {
      lyrDrop(LYR_RETRY_MS);
    }
    if (lyrState == LYR_OFF || lyrState == LYR_HANDSHAKE) return;
  }

  lyrPump();
  if (lyrState == LYR_OFF) return;

  // The hello/ready exchange must complete promptly, or drop and retry.
  if (lyrState == LYR_HELLO && (long)(millis() - lyrHelloDeadlineMs) >= 0) {
    lyrDrop(LYR_RETRY_MS);
  }
}
