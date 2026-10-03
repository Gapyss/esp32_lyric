// Shared declarations for the Clawdmeter ESP8266 firmware. This box runs one
// screen — the lyrics display — so the header is a small contract between the
// .ino (WiFiManager, web server, OTA, EEPROM, the waiting screen) and
// lyrics_stream.cpp: the Tend paper palette, the time source, and the text /
// chrome helpers the .ino defines.
#pragma once

#include <Arduino.h>
#include <IPAddress.h>   // lyricsStreamNoteHost takes the push source address
#include <Arduino_GFX_Library.h>

// ---- Panel + palette -------------------------------------------------------
// General colors (OTA/boot screens).
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GRAY   0x8410
#define C_LINE   0x4208
#define C_BLUE   0x041F
#define C_AMBER  0xFD20

// MUSIC screen -- "Tend" warm-paper palette (claude.ai/design Now Playing card).
#define C_MUS_PAPER      0xFF9C  // #F8F3E1 primary surface (cream)
#define C_MUS_PAPER_DEEP 0xEF39  // #EDE6CB sunken surface / progress track
#define C_MUS_PAPER_SOFT 0xFFDE  // #FDFBF2 raised surface / vinyl label ring
#define C_MUS_PAPER_LINE 0xD654  // #D3C8A2 hairline border on paper
#define C_MUS_INK        0x18E2  // #1F1D11 primary text
#define C_MUS_INK_SOFT   0x5AA6  // #5B5536 secondary text
#define C_MUS_INK_MUTED  0x946D  // #948D6F tertiary
#define C_MUS_INK_FAINT  0xC5F2  // #C5BD96 disabled / placeholder
#define C_MUS_EMBER      0xEAE7  // #E85D3C primary accent
#define C_MUS_EMBER_DEEP 0xC203  // #C2421F pressed ember
#define C_MUS_BARK       0x8B68  // #8B6F47 vinyl disc rings (warm earth)
#define C_MUS_BARK_DEEP  0x6A86  // #6B5236 vinyl disc grooves

// Screen-neutral Tend aliases + the calm pillar/semantic accents. Per Tend,
// EMBER is the one loud color -- reserved for danger/alert; healthy values
// stay quiet (moss / earth / ink).
#define C_TND_PAPER      C_MUS_PAPER
#define C_TND_PAPER_DEEP C_MUS_PAPER_DEEP
#define C_TND_LINE       C_MUS_PAPER_LINE
#define C_TND_INK        C_MUS_INK
#define C_TND_INK_SOFT   C_MUS_INK_SOFT
#define C_TND_MUTE       C_MUS_INK_MUTED
#define C_TND_FAINT      C_MUS_INK_FAINT
#define C_TND_EMBER      C_MUS_EMBER
#define C_TND_MOSS       0x6C6B            // #6B8E5A quiet pillar green
#define C_TND_WARN       0xF504            // #F4A226 marigold
#define C_TND_INFO       0x2C75            // #2E8FAB fresh sky
#define C_TND_BARK       C_MUS_BARK

// Display clock times in Thailand time. Pushed/NTP epochs are UTC; add the
// offset only when formatting wall-clock text.
#define TZ_OFFSET 25200UL   // Asia/Bangkok, UTC+7 (no DST)

extern Arduino_GFX *gfx;

// ---- The one screen ------------------------------------------------------
// This board shows lyrics and nothing else, so there is no screen registry and
// no mode switch. While the Mac's daemon is streaming frames they own the panel
// outright (see lyrics_stream.cpp); the rest of the time the .ino paints its own
// waiting screen -- Tend chrome, a status line, and the device's address, so you
// can reach the dashboard without a serial cable. That screen is private to the
// .ino; nothing here needs to declare it.

// ---- Time ------------------------------------------------------------------
// Current UTC epoch: daemon-pushed server time when available, NTP before
// that. 0 = no time source yet.
unsigned long nowEpoch(void);

// ---- Text helpers (defined in the .ino) ------------------------------------
int textWidth(const String &s, uint8_t size);
void printCentered(int y, uint8_t size, const String &s, uint16_t fg, uint16_t bg);
void printRight(int rightX, int y, uint8_t size, const String &s, uint16_t fg, uint16_t bg);
String pad2(int v);
String hhmmss(unsigned long e);

// Tend chrome: ember hearth mark + eyebrow + hairline rule (the shared screen
// header). The per-second clock at top right is each screen's tick job.
void drawTendFlame(int cx, int cy);
void tendHeader(const char *eyebrow);
// Header clock repaint (right-aligned HH:MM:SS at the eyebrow line).
void tendHeaderClock(void);

// EEPROM commit wrapped in the backlight-PWM flash guard (a timer interrupt
// during a flash write resets the chip -- see the .ino).
void tendEepromCommit(void);

// EEPROM layout v2 (v1 was marker+brightness only; loadBrightness migrates).
// Bytes 2..15 held config for screens this board no longer has; the region stays
// reserved so the addresses below are unchanged and already-flashed
// boards still read their stored brightness across an OTA to this firmware.
static const int EE_MARKER_ADDR = 0;       // 0xC2
static const int EE_BRIGHTNESS_ADDR = 1;   // u8
// WPA2-Enterprise (802.1X) block, appended past the old 16-byte layout so a
// board OTA'd from an older build reads its brightness unchanged. Valid only
// while EE_EAP_FLAG_ADDR holds EEPROM_EAP_MARKER; each string is NUL-terminated.
static const int EE_EAP_FLAG_ADDR = 16;    // EEPROM_EAP_MARKER = 802.1X on
static const int EE_EAP_SSID_ADDR = 17;    // 33 bytes
static const int EE_EAP_USER_ADDR = 50;    // 65 bytes
static const int EE_EAP_PASS_ADDR = 115;   // 65 bytes
static const int EE_SIZE = 180;

// ---- Lyrics frame stream (lyrics_stream.cpp) -------------------------------
// The box dials back to the Mac that pushes /usage & /nowplaying and speaks the
// lyrics_display_daemon.py LYR1 WebSocket protocol (:8766) at 240x240:
// Core Text-rendered frames with real Thai shaping and syllable karaoke. This is
// the only renderer that draws song content; when no frame flows the .ino paints
// the waiting screen (see musicDrawWaiting there), not a fallback now-playing card.
void lyricsStreamNoteHost(const IPAddress &host);  // Mac's IP, from daemon pushes
void lyricsStreamTick(void);   // pump the socket; call once per loop()
void lyricsStreamStop(void);   // close + free the framebuffer (OTA start)
bool lyricsStreamActive(void); // a streamed frame currently owns the panel
bool lyricsStreamConnecting(void); // Mac IP known but frames not flowing yet (dialing)
