// Shared declarations for the Clawdmeter ESP8266 firmware. The .ino keeps the
// infrastructure (WiFiManager, web server, OTA, EEPROM, music screen) and each
// ported ESP32 screen lives in its own .cpp next to it; this header is the
// contract between them: the Tend paper palette, the screen registry, the time
// source, and the small text helpers the .ino defines.
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

// ---- Screen registry -------------------------------------------------------
// Mirrors the ESP32 app_mode list; MUSIC is this firmware's own on-device
// lyrics screen (kept, not ported).
static const uint8_t SCREEN_MUSIC = 0;
static const uint8_t SCREEN_CLOCK = 1;
static const uint8_t SCREEN_POMODORO = 2;
static const uint8_t SCREEN_WATER = 3;
static const uint8_t SCREEN_STATS = 4;
static const uint8_t SCREEN_PET = 5;
static const uint8_t SCREEN_SAND = 6;
static const uint8_t SCREEN_SWARM = 7;
static const uint8_t SCREEN_COMIC = 8;
static const uint8_t SCREEN_APOD = 9;

extern uint8_t lcdScreen;

// Switch to a screen and repaint it from scratch (chrome + content). Safe to
// call from screen modules (water/pomodoro alerts pull their screen forward,
// since there is no chime on this box -- alerts are screen-only).
void tendShowScreen(uint8_t screen);

// ---- Time ------------------------------------------------------------------
// Current UTC epoch: daemon-pushed server time when available, NTP before
// that. 0 = no time source yet.
unsigned long nowEpoch(void);

// ---- Text helpers (defined in the .ino) ------------------------------------
int textWidth(const String &s, uint8_t size);
void printCentered(int y, uint8_t size, const String &s, uint16_t fg, uint16_t bg);
void printRight(int rightX, int y, uint8_t size, const String &s, uint16_t fg, uint16_t bg);
String pad2(int v);
String hhmm(unsigned long e);
String hhmmss(unsigned long e);
String pctText(int pct);

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
static const int EE_MARKER_ADDR = 0;       // 0xC2
static const int EE_BRIGHTNESS_ADDR = 1;   // u8
static const int EE_WATER_INTERVAL = 2;    // u16 minutes
static const int EE_WATER_START = 4;       // u16 minutes-of-day
static const int EE_WATER_END = 6;         // u16 minutes-of-day
static const int EE_PET_TOTAL = 8;         // u32
static const int EE_PET_ADOPT = 12;        // u32 epoch day
static const int EE_SIZE = 16;

// ---- Mac metrics (fed by the daemon's /usage push, read by stats/pet) ------
extern int macCpuPct;
extern int macMemPct;
extern int macDiskPct;
extern int macBatteryPct;

// ---- Screen modules --------------------------------------------------------
// Each Begin paints the full screen; each Tick animates in place. The global
// pomodoroTick()/waterTick() run every loop() pass regardless of the visible
// screen (countdowns keep advancing in the background).

void clockScreenBegin(void);
void clockScreenTick(void);

void pomodoroScreenBegin(void);
void pomodoroScreenTick(void);
void pomodoroTick(void);
void pomodoroToggleStartPause(void);
void pomodoroReset(void);
const char *pomodoroStateName(void);
int pomodoroRemainingSec(void);

void waterScreenBegin(void);
void waterScreenTick(void);
void waterTick(void);
void waterLogDrink(void);
void waterFireNow(void);
void waterSnooze(int minutes);
bool waterConfigure(int intervalMin, int startMin, int endMin);
bool waterAlerting(void);
int waterDrinksToday(void);
int waterNextInSec(void);
int waterIntervalMin(void);
int waterStartMin(void);
int waterEndMin(void);

void statsScreenBegin(void);
void statsScreenTick(void);
void statsOnUsagePush(void);

void petScreenBegin(void);
void petScreenTick(void);
void petPet(void);
int petPetsToday(void);
int petPetsTotal(void);

void sandScreenBegin(void);
void sandScreenTick(void);
void sandPour(void);
void sandClear(void);

void swarmScreenBegin(void);
void swarmScreenTick(void);
void swarmScatter(void);
void swarmToggleRoam(void);
bool swarmRoaming(void);

// Lyrics frame stream (lyrics_stream.cpp). While the MUSIC screen is visible
// the box dials back to the Mac that pushes /usage & /nowplaying and speaks
// the lyrics_display_daemon.py LYR1 WebSocket protocol (:8766) at 240x240:
// Core Text-rendered frames with real Thai shaping and syllable karaoke. The
// on-device MUSIC renderer stays as the fallback whenever no frame flows.
void lyricsStreamNoteHost(const IPAddress &host);  // Mac's IP, from daemon pushes
void lyricsStreamTick(void);   // pump the socket; call each loop while MUSIC shows
void lyricsStreamStop(void);   // close + free buffers (leaving MUSIC, OTA start)
bool lyricsStreamActive(void); // a streamed frame currently owns the panel

// Comic (xkcd) + APOD daily images. Metadata (image URL + title) is pushed by
// the daemon over /daily -- this chip cannot afford BearSSL heap for the HTTPS
// APIs -- and the image itself is fetched over plain HTTP through the wsrv.nl
// resize proxy, stream-decoded into a shared 1-bit dithered frame.
void dailyScreenBegin(void);   // uses lcdScreen to pick comic vs apod
void dailyScreenTick(void);
void dailySetMeta(bool apod, const String &imgUrl, const String &title,
                  const String &extra);  // extra: comic number / apod date
void dailyRefresh(bool apod);
const char *dailyStateName(bool apod);
String dailyTitle(bool apod);
String dailyExtra(bool apod);
