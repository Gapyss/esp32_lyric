// Lyrics frame stream — the ESP8266 side of the Mac's lyrics_display_daemon.py
// (the same LYR1 protocol the ESP32 board_client.cpp speaks, at 240x240).
//
// While the MUSIC screen is visible this module keeps a WebSocket to the
// daemon's board port (:8766). The daemon renders 240x240 1-bpp frames with
// Core Text (real Thai shaping, syllable karaoke) and pushes them as binary
// LYR1 envelopes: full frames, dirty-rect deltas, and pre-scheduled frames
// that swap on a millis() deadline (so lyric lines flip on-beat between
// pushes). Set bits are paper, clear bits ink — the daemon's light theme
// matches the Tend look 1:1.
//
// Everything is millis()-polled from loop() (no timer ISR, no extra library):
// a hand-rolled WS client over WiFiClient with a byte-wise state machine, so
// a 7.2 KB frame arriving across TCP segments never blocks the web server.
// The daemon's address is not configured anywhere — it is the Mac that
// already POSTs /usage and /nowplaying to this box, so the .ino hands us the
// source IP of those pushes (lyricsStreamNoteHost) and we dial back to it.
//
// Memory: one current frame + one pending (scheduled) frame, 7200 B each,
// malloc'd only while streaming and freed on lyricsStreamStop(). The .ino
// frees the on-device music marquee canvases (~17 KB) while a stream owns
// the panel, so the two render paths never hold heap at the same time.

#include <ESP8266WiFi.h>
#include "tend.h"

#define LYR_PORT        8766
#define LYR_W           240
#define LYR_H           240
#define LYR_ROW_BYTES   (LYR_W / 8)
#define LYR_FRAME_BYTES (LYR_ROW_BYTES * LYR_H)
#define LYR_ENV_HDR     28          // "LYR1" + ver/kind + 7x u16 + 2x u32
#define LYR_RETRY_MS    8000UL      // reconnect backoff
#define LYR_CONNECT_TIMEOUT_MS 1200UL  // TCP connect budget (blocking; keep small)
#define LYR_HANDSHAKE_TIMEOUT_MS 3000UL
#define LYR_HOST_FRESH_MS (10UL * 60UL * 1000UL)  // dial only a recently-seen Mac

// Envelope kinds (swap_in_ms > 0 is what actually selects the pending buffer,
// mirroring the ESP32 client; the kinds are validated for shape only).
static const uint8_t LYR_KIND_FULL_NOW = 1;
static const uint8_t LYR_KIND_FULL_SCHED = 2;
static const uint8_t LYR_KIND_RECT_NOW = 3;
static const uint8_t LYR_KIND_RECT_SCHED = 4;

enum LyrState : uint8_t { LYR_OFF = 0, LYR_HANDSHAKE, LYR_OPEN };
enum LyrPhase : uint8_t { PH_HDR2 = 0, PH_EXT, PH_PAY };
enum LyrPayMode : uint8_t { PAY_DISCARD = 0, PAY_TEXT, PAY_CTL, PAY_ENV };

static WiFiClient lyrClient;
static IPAddress lyrHost;
static bool lyrHostKnown = false;
static unsigned long lyrHostSeenMs = 0;
static uint8_t lyrState = LYR_OFF;
static unsigned long lyrNextAttemptMs = 0;
static unsigned long lyrHandshakeDeadlineMs = 0;

// Frame buffers: current (what the panel shows) and pending (a scheduled
// frame waiting for its swap deadline).
static uint8_t *lyrFrame = nullptr;
static uint8_t *lyrPending = nullptr;
static bool lyrFrameValid = false;    // stream owns the MUSIC panel
static bool lyrPendingValid = false;
static unsigned long lyrSwapAtMs = 0;

// Handshake response accumulator (looking for "\r\n\r\n", " 101 " on line 1).
static char lyrHsBuf[256];
static uint16_t lyrHsLen = 0;

// WebSocket frame parser.
static LyrPhase lyrPhase = PH_HDR2;
static uint8_t lyrHdr[2];
static uint8_t lyrExtNeed = 0, lyrExtGot = 0;
static uint8_t lyrExtBuf[8];
static uint8_t lyrOpcode = 0;
static uint32_t lyrPayLen = 0, lyrPayOff = 0;
static LyrPayMode lyrPayMode = PAY_DISCARD;
static uint8_t lyrTxtBuf[192];
static uint8_t lyrCtlBuf[125];
static uint8_t lyrCtlLen = 0;

// Parsed LYR1 envelope of the binary message in flight.
static uint8_t lyrEnvBuf[LYR_ENV_HDR];
static uint8_t *lyrEnvTarget = nullptr;
static bool lyrEnvFull = false;
static uint16_t lyrEnvX = 0, lyrEnvY = 0, lyrEnvRW = 0, lyrEnvRH = 0, lyrEnvRowB = 0;
static uint32_t lyrEnvSwapMs = 0;

static uint16_t lyrBe16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t lyrBe32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// 1-bpp (LSB-first, row-major) -> RGB565 rows on the panel. Set bit = paper,
// clear = ink (the daemon's light theme). One 240-px line buffer, row by row;
// a yield every 32 rows keeps the WiFi stack fed during a full-frame blit.
static void lyrBlit(int x, int y, int w, int h) {
  if (lcdScreen != SCREEN_MUSIC || lyrFrame == nullptr) return;
  static uint16_t line[LYR_W];
  for (int row = 0; row < h; row++) {
    const uint8_t *src = lyrFrame + (unsigned)(y + row) * LYR_ROW_BYTES;
    for (int col = 0; col < w; col++) {
      int px = x + col;
      line[col] = ((src[px >> 3] >> (px & 7)) & 1) ? C_MUS_PAPER : C_MUS_INK;
    }
    gfx->draw16bitRGBBitmap(x, y + row, line, w, 1);
    if ((row & 31) == 31) yield();
  }
}

// Client->server frames must be masked (the daemon rejects unmasked ones).
static bool lyrSendFrame(uint8_t opcode, const uint8_t *payload, uint8_t len) {
  uint8_t buf[6 + 125];
  buf[0] = 0x80 | opcode;
  buf[1] = 0x80 | len;
  uint32_t mask = micros() ^ (millis() << 16) ^ 0xA5C3;
  memcpy(buf + 2, &mask, 4);
  for (uint8_t i = 0; i < len; i++) buf[6 + i] = payload[i] ^ buf[2 + (i & 3)];
  return lyrClient.write(buf, (size_t)6 + len) == (size_t)6 + len;
}

static bool lyrSendReady() {
  static const uint8_t ready[] = { 'r', 'e', 'a', 'd', 'y' };
  return lyrSendFrame(1, ready, sizeof(ready));
}

static void lyrResetParser() {
  lyrPhase = PH_HDR2;
  lyrExtNeed = lyrExtGot = 0;
  lyrPayLen = lyrPayOff = 0;
  lyrPayMode = PAY_DISCARD;
  lyrCtlLen = 0;
}

// Drop the connection and everything the stream painted; the MUSIC screen's
// tick sees lyricsStreamActive() go false and repaints the on-device fallback.
static void lyrDrop(unsigned long retryDelayMs) {
  lyrClient.stop();
  lyrState = LYR_OFF;
  lyrFrameValid = false;
  lyrPendingValid = false;
  lyrResetParser();
  lyrNextAttemptMs = millis() + retryDelayMs;
}

void lyricsStreamStop(void) {
  lyrDrop(0);
  free(lyrFrame);
  free(lyrPending);
  lyrFrame = nullptr;
  lyrPending = nullptr;
}

bool lyricsStreamActive(void) {
  return lyrState == LYR_OPEN && lyrFrameValid;
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

// Ensure a frame buffer exists; nullptr on OOM (the message is then discarded
// and the on-device renderer keeps the screen).
static uint8_t *lyrEnsure(uint8_t **buf, bool zero) {
  if (*buf == nullptr) {
    *buf = (uint8_t *)malloc(LYR_FRAME_BYTES);
    if (*buf != nullptr && zero) memset(*buf, 0, LYR_FRAME_BYTES);
  } else if (zero) {
    memset(*buf, 0, LYR_FRAME_BYTES);
  }
  return *buf;
}

// Parse the 28-byte LYR1 header once buffered; picks the target buffer and
// payload mode for the rest of the message.
static void lyrParseEnvelope() {
  lyrPayMode = PAY_DISCARD;
  if (memcmp(lyrEnvBuf, "LYR1", 4) != 0 || lyrEnvBuf[4] != 1) return;
  const uint8_t kind = lyrEnvBuf[5];
  const uint16_t width = lyrBe16(lyrEnvBuf + 6);
  const uint16_t height = lyrBe16(lyrEnvBuf + 8);
  lyrEnvX = lyrBe16(lyrEnvBuf + 10);
  lyrEnvY = lyrBe16(lyrEnvBuf + 12);
  lyrEnvRW = lyrBe16(lyrEnvBuf + 14);
  lyrEnvRH = lyrBe16(lyrEnvBuf + 16);
  lyrEnvRowB = lyrBe16(lyrEnvBuf + 18);
  lyrEnvSwapMs = lyrBe32(lyrEnvBuf + 20);
  const uint32_t dataLen = lyrBe32(lyrEnvBuf + 24);

  if (width != LYR_W || height != LYR_H) return;
  if (dataLen != lyrPayLen - LYR_ENV_HDR) return;

  if (kind == LYR_KIND_FULL_NOW || kind == LYR_KIND_FULL_SCHED) {
    if (lyrEnvX != 0 || lyrEnvY != 0 || lyrEnvRW != LYR_W || lyrEnvRH != LYR_H ||
        lyrEnvRowB != LYR_ROW_BYTES || dataLen != LYR_FRAME_BYTES) {
      return;
    }
    lyrEnvFull = true;
  } else if (kind == LYR_KIND_RECT_NOW || kind == LYR_KIND_RECT_SCHED) {
    if ((lyrEnvX & 7) != 0 || lyrEnvRW == 0 || lyrEnvRH == 0 || lyrEnvRowB == 0 ||
        lyrEnvX + lyrEnvRW > LYR_W || lyrEnvY + lyrEnvRH > LYR_H ||
        dataLen != (uint32_t)lyrEnvRowB * lyrEnvRH) {
      return;
    }
    lyrEnvFull = false;
  } else {
    return;
  }

  if (lyrEnvSwapMs > 0) {
    // Scheduled frame -> pending buffer. A scheduled rect patches a copy of
    // the current frame (same seeding as the ESP32 client).
    if (lyrEnsure(&lyrPending, false) == nullptr) return;
    if (!lyrEnvFull && !lyrPendingValid) {
      if (lyrFrame != nullptr && lyrFrameValid) memcpy(lyrPending, lyrFrame, LYR_FRAME_BYTES);
      else memset(lyrPending, 0, LYR_FRAME_BYTES);
    }
    lyrEnvTarget = lyrPending;
  } else {
    // Now-frame -> current buffer (zero-seeded for a rect with no base yet).
    if (lyrEnsure(&lyrFrame, !lyrEnvFull && !lyrFrameValid) == nullptr) return;
    lyrEnvTarget = lyrFrame;
  }
  lyrPayMode = PAY_ENV;
}

// A complete, valid envelope arrived: commit it (swap bookkeeping + blit).
static void lyrApplyEnvelope() {
  if (lyrEnvSwapMs > 0) {
    lyrPendingValid = true;
    lyrSwapAtMs = millis() + lyrEnvSwapMs;
    return;
  }
  lyrFrameValid = true;
  lyrPendingValid = false;   // a now-frame supersedes any queued swap
  if (lyrEnvFull) lyrBlit(0, 0, LYR_W, LYR_H);
  else lyrBlit(lyrEnvX, lyrEnvY, lyrEnvRW, lyrEnvRH);
}

static void lyrHandleText() {
  lyrTxtBuf[lyrPayLen < sizeof(lyrTxtBuf) ? lyrPayLen : sizeof(lyrTxtBuf) - 1] = '\0';
  const char *text = (const char *)lyrTxtBuf;
  if (strstr(text, "\"type\":\"hello\"") != nullptr) {
    lyrSendReady();
  } else if (strstr(text, "\"type\":\"clear\"") != nullptr) {
    // No track: hand the panel back to the on-device renderer.
    lyrFrameValid = false;
    lyrPendingValid = false;
  }
}

static void lyrDispatchMessage() {
  switch (lyrOpcode) {
    case 1: lyrHandleText(); break;
    case 2: if (lyrPayMode == PAY_ENV) lyrApplyEnvelope(); break;
    case 8: lyrDrop(LYR_RETRY_MS); break;
    case 9: lyrSendFrame(10, lyrCtlBuf, lyrCtlLen); break;   // ping -> pong
    default: break;                                          // pong/other: ignore
  }
}

// Feed one payload byte to the current message's consumer.
static inline void lyrPayloadByte(uint8_t b) {
  switch (lyrPayMode) {
    case PAY_TEXT:
      if (lyrPayOff < sizeof(lyrTxtBuf) - 1) lyrTxtBuf[lyrPayOff] = b;
      break;
    case PAY_CTL:
      if (lyrCtlLen < sizeof(lyrCtlBuf)) lyrCtlBuf[lyrCtlLen++] = b;
      break;
    case PAY_ENV:
      if (lyrEnvFull) {
        lyrEnvTarget[lyrPayOff - LYR_ENV_HDR] = b;
      } else {
        const uint32_t idx = lyrPayOff - LYR_ENV_HDR;
        const uint32_t row = idx / lyrEnvRowB;
        const uint32_t col = idx - row * lyrEnvRowB;
        lyrEnvTarget[(lyrEnvY + row) * LYR_ROW_BYTES + (lyrEnvX >> 3) + col] = b;
      }
      break;
    default:
      break;
  }
}

// Pump every buffered byte through the frame parser. Byte-wise on purpose:
// simple, and even a full 7.2 KB frame costs only a few ms.
static void lyrPump() {
  while (lyrClient.available() > 0) {
    int c = lyrClient.read();
    if (c < 0) break;
    uint8_t b = (uint8_t)c;

    switch (lyrPhase) {
      case PH_HDR2:
        lyrHdr[lyrExtGot++] = b;
        if (lyrExtGot < 2) break;
        lyrExtGot = 0;
        // Server frames must be FIN + unmasked (the daemon never fragments).
        if ((lyrHdr[0] & 0x80) == 0 || (lyrHdr[1] & 0x80) != 0) { lyrDrop(LYR_RETRY_MS); return; }
        lyrOpcode = lyrHdr[0] & 0x0F;
        lyrPayLen = lyrHdr[1] & 0x7F;
        lyrPayOff = 0;
        if (lyrPayLen == 126) { lyrExtNeed = 2; lyrPhase = PH_EXT; break; }
        if (lyrPayLen == 127) { lyrExtNeed = 8; lyrPhase = PH_EXT; break; }
        goto payload_start;

      case PH_EXT:
        lyrExtBuf[lyrExtGot++] = b;
        if (lyrExtGot < lyrExtNeed) break;
        if (lyrExtNeed == 2) {
          lyrPayLen = ((uint32_t)lyrExtBuf[0] << 8) | lyrExtBuf[1];
        } else {
          // 64-bit length: anything past 32 bits (or 16, really) is bogus here.
          for (int i = 0; i < 4; i++) {
            if (lyrExtBuf[i] != 0) { lyrDrop(LYR_RETRY_MS); return; }
          }
          lyrPayLen = lyrBe32(lyrExtBuf + 4);
        }
        lyrExtGot = 0;
        goto payload_start;

      payload_start:
        lyrCtlLen = 0;
        if (lyrOpcode == 8 || lyrOpcode == 9 || lyrOpcode == 10) {
          if (lyrPayLen > 125) { lyrDrop(LYR_RETRY_MS); return; }
          lyrPayMode = PAY_CTL;
        } else if (lyrOpcode == 1) {
          lyrPayMode = lyrPayLen < sizeof(lyrTxtBuf) ? PAY_TEXT : PAY_DISCARD;
        } else if (lyrOpcode == 2) {
          // The LYR1 header is buffered by the PH_PAY special case below;
          // lyrParseEnvelope flips the mode to PAY_ENV once it checks out.
          lyrPayMode = PAY_DISCARD;
          lyrEnvTarget = nullptr;
        } else {
          lyrPayMode = PAY_DISCARD;
        }
        if (lyrPayLen == 0) { lyrDispatchMessage(); lyrResetParser(); break; }
        lyrPhase = PH_PAY;
        break;

      case PH_PAY:
        if (lyrOpcode == 2 && lyrPayOff < LYR_ENV_HDR) {
          // Buffer the LYR1 header; parse it when complete.
          if (lyrPayLen >= LYR_ENV_HDR && lyrPayLen <= LYR_ENV_HDR + LYR_FRAME_BYTES) {
            lyrEnvBuf[lyrPayOff] = b;
            if (lyrPayOff == LYR_ENV_HDR - 1) lyrParseEnvelope();
          }
        } else {
          lyrPayloadByte(b);
        }
        lyrPayOff++;
        if (lyrPayOff >= lyrPayLen) {
          lyrDispatchMessage();
          if (lyrState == LYR_OFF) return;   // dispatch may have dropped us
          lyrResetParser();
        }
        break;
    }
  }
}

// Read the HTTP 101 handshake response without blocking.
static void lyrPumpHandshake() {
  while (lyrClient.available() > 0) {
    int c = lyrClient.read();
    if (c < 0) break;
    if (lyrHsLen < sizeof(lyrHsBuf) - 1) lyrHsBuf[lyrHsLen++] = (char)c;
    lyrHsBuf[lyrHsLen] = '\0';
    if (lyrHsLen >= 4 && memcmp(lyrHsBuf + lyrHsLen - 4, "\r\n\r\n", 4) == 0) {
      if (strstr(lyrHsBuf, " 101 ") != nullptr) {
        lyrState = LYR_OPEN;
        lyrResetParser();
      } else {
        lyrDrop(LYR_RETRY_MS);
      }
      return;
    }
    if (lyrHsLen >= sizeof(lyrHsBuf) - 1) { lyrDrop(LYR_RETRY_MS); return; }
  }
}

static void lyrTryConnect() {
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

  char request[224];
  snprintf(request, sizeof(request),
           "GET /board?w=%d&h=%d HTTP/1.1\r\n"
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
    if (lyrState != LYR_OPEN) return;
  }

  lyrPump();

  // Promote a scheduled frame when its deadline passes (this is how lyric
  // lines flip on-beat between daemon pushes).
  if (lyrState == LYR_OPEN && lyrPendingValid && lyrPending != nullptr &&
      (long)(millis() - lyrSwapAtMs) >= 0) {
    if (lyrEnsure(&lyrFrame, false) != nullptr) {
      memcpy(lyrFrame, lyrPending, LYR_FRAME_BYTES);
      lyrFrameValid = true;
      lyrBlit(0, 0, LYR_W, LYR_H);
    }
    lyrPendingValid = false;
  }
}
