// Desk-creature screen, ported from firmware/main/pet_screen.cpp (ESP32).
// "Ember" the blob: rounded body, ember-flame antenna, blinking eyes, deadpan
// quips. No temp/humidity sensor on this box, so the melting/freezing/sticky
// moods are gone -- moods are COZY, SLEEPING (22:00-07:00) and HAPPY (a 4 s
// glow after /pet from the dashboard). Pets-total and adoption day persist in
// EEPROM (the ESP32 used NVS). The body is static; only the eyes, the flame
// flicker, the quip line and the header clock animate, so nothing flickers.
#include "tend.h"

#include <EEPROM.h>

static const unsigned long PETTED_GLOW_MS = 4000UL;
static const int SLEEP_START_HOUR = 22;
static const int SLEEP_END_HOUR = 7;

enum PetMood : uint8_t { PET_COZY, PET_SLEEPING, PET_HAPPY };

static bool g_inited = false;
static unsigned long g_lastPetMs = 0;
static bool g_everPetted = false;
static int g_petsToday = 0;
static uint32_t g_petsTotal = 0;
static uint32_t g_adoptDay = 0;   // local epoch day when first seen; 0 = unknown
static long g_dayMarker = 0;

static uint8_t g_shownMood = 255;
static uint8_t g_shownQuipSlot = 255;
static bool g_eyesClosed = false;
static uint8_t g_flamePhase = 255;
static unsigned long g_lastTickSec = 0;

static const int PET_CX = 120;
static const int BODY_W = 110;
static const int BODY_H = 86;
static const int BODY_TOP = 96;
static const int EYE_DX = 22;
static const int EYE_Y = BODY_TOP + 32;

static long localEpochDay() {
  const unsigned long e = nowEpoch();
  return e ? (long)((e + TZ_OFFSET) / 86400UL) : 0;
}

static int localHour() {
  const unsigned long e = nowEpoch();
  return e ? (int)(((e + TZ_OFFSET) % 86400UL) / 3600UL) : 12;
}

static void initIfNeeded() {
  if (g_inited) return;
  g_inited = true;
  EEPROM.get(EE_PET_TOTAL, g_petsTotal);
  EEPROM.get(EE_PET_ADOPT, g_adoptDay);
  if (g_petsTotal == 0xFFFFFFFFUL) g_petsTotal = 0;   // fresh flash reads as FF
  if (g_adoptDay == 0xFFFFFFFFUL) g_adoptDay = 0;
}

static void updateDayRollover() {
  const long marker = localEpochDay();
  if (marker == 0 || marker == g_dayMarker) return;
  g_dayMarker = marker;
  g_petsToday = 0;
  // Adoption day is recorded the first time the creature sees a valid clock.
  if (g_adoptDay == 0) {
    g_adoptDay = (uint32_t)marker;
    EEPROM.put(EE_PET_ADOPT, g_adoptDay);
    tendEepromCommit();
  }
}

void petPet() {
  initIfNeeded();
  updateDayRollover();
  g_lastPetMs = millis();
  g_everPetted = true;
  g_petsToday++;
  g_petsTotal++;
  EEPROM.put(EE_PET_TOTAL, g_petsTotal);
  tendEepromCommit();
}

int petPetsToday() { return g_petsToday; }
int petPetsTotal() { initIfNeeded(); return (int)g_petsTotal; }

static PetMood resolveMood() {
  if (g_everPetted && millis() - g_lastPetMs < PETTED_GLOW_MS) return PET_HAPPY;
  const int hour = localHour();
  if (nowEpoch() && (hour >= SLEEP_START_HOUR || hour < SLEEP_END_HOUR)) return PET_SLEEPING;
  return PET_COZY;
}

static long ageDays() {
  if (g_adoptDay == 0) return 0;
  const long today = localEpochDay();
  return today > 0 ? today - (long)g_adoptDay + 1 : 0;
}

// Deadpan status line in the Tend voice, rotating every ~7 s in the mood's pool.
static String quipFor(PetMood mood, uint8_t slot) {
  switch (mood) {
    case PET_SLEEPING:
      switch (slot % 3) {
        case 0: return "do not disturb - recharging";
        case 1: return "dreaming in rgb565";
        default: return "night mode - even creatures need sleep";
      }
    case PET_HAPPY:
      switch (slot % 3) {
        case 0: return "pet received - affection buffer full";
        case 1: return String(g_petsToday) + " pets today - a personal best probably";
        default: return "logging warmth - thank you";
      }
    default:
      switch (slot % 4) {
        case 0: return "all conditions nominal - i am thriving";
        case 1: {
          const long age = ageDays();
          if (age > 0) return "day " + String(age) + " of guarding this desk";
          return "guarding this desk since boot";
        }
        case 2: return "i blink so you remember to";
        default: return "powered by wifi and quiet ambition";
      }
  }
}

// Stroke an arc of `r` (2 px thick) between degrees a0..a1, 0 = 3 o'clock,
// counting clockwise -- used for the smile and the closed happy-eyes, since
// Arduino_GFX has no quadrant-circle primitive like u8g2's.
static void arcStroke(int cx, int cy, int r, int a0, int a1, uint16_t color) {
  for (int a = a0; a <= a1; a += 4) {
    const float rad = (float)a * (float)M_PI / 180.0f;
    const int x = cx + (int)(cosf(rad) * (float)r);
    const int y = cy + (int)(sinf(rad) * (float)r);
    gfx->fillRect(x, y, 2, 2, color);
  }
}

static void drawFlame(bool flicker) {
  const int cx = PET_CX, top = BODY_TOP;
  gfx->fillRect(cx - 12, top - 34, 24, 30, C_TND_PAPER);   // clear the flame zone
  gfx->drawFastVLine(cx, top - 12, 12, C_TND_INK);
  gfx->drawFastVLine(cx + 1, top - 12, 12, C_TND_INK);
  const int spread = flicker ? 6 : 5;
  const int rise = flicker ? 30 : 27;
  gfx->fillTriangle(cx - spread, top - 12, cx + spread, top - 12,
                    cx, top - rise, C_TND_EMBER);
  gfx->fillCircle(cx, top - 16, 2, C_TND_WARN);
  g_flamePhase = flicker ? 1 : 0;
}

static void drawEyes(PetMood mood, bool closed) {
  for (int side = -1; side <= 1; side += 2) {
    const int ex = PET_CX + side * EYE_DX;
    gfx->fillRect(ex - 7, EYE_Y - 7, 14, 14, C_TND_PAPER);
    if (mood == PET_SLEEPING) {
      arcStroke(ex, EYE_Y - 3, 6, 20, 160, C_TND_INK);   // closed, curved down
    } else if (mood == PET_HAPPY) {
      arcStroke(ex, EYE_Y + 3, 6, 200, 340, C_TND_INK);  // happy ^^ arcs
    } else if (closed) {
      gfx->drawFastHLine(ex - 5, EYE_Y, 11, C_TND_INK);
    } else {
      gfx->fillCircle(ex, EYE_Y, 5, C_TND_INK);
    }
  }
  g_eyesClosed = closed;
}

static void drawMouth(PetMood mood) {
  const int my = BODY_TOP + 58;
  if (mood == PET_SLEEPING) {
    gfx->drawCircle(PET_CX, my, 3, C_TND_INK);            // small o (snoring)
  } else if (mood == PET_HAPPY) {
    arcStroke(PET_CX, my - 6, 11, 20, 160, C_TND_INK);    // big smile
    arcStroke(PET_CX, my - 6, 10, 20, 160, C_TND_INK);
  } else {
    arcStroke(PET_CX, my - 4, 8, 30, 150, C_TND_INK);     // gentle smile
  }
}

static void drawHeart(int cx, int cy, int r) {
  gfx->fillCircle(cx - r, cy, r, C_TND_EMBER);
  gfx->fillCircle(cx + r, cy, r, C_TND_EMBER);
  gfx->fillTriangle(cx - 2 * r, cy, cx + 2 * r + 1, cy, cx, cy + 2 * r + r / 2, C_TND_EMBER);
}

static void drawQuip(PetMood mood) {
  g_shownQuipSlot = (uint8_t)((millis() / 7000UL) & 0xFF);
  gfx->fillRect(0, 202, 240, 10, C_TND_PAPER);
  printCentered(202, 1, quipFor(mood, g_shownQuipSlot), C_TND_INK_SOFT, C_TND_PAPER);
}

static void drawMoodChip(PetMood mood) {
  const char *label = mood == PET_SLEEPING ? "SLEEPING" : mood == PET_HAPPY ? "HAPPY" : "COZY";
  gfx->fillRect(140, 40, 86, 16, C_TND_PAPER);   // clear (chip width varies)
  const int chipW = textWidth(label, 1) + 14;
  const int chipX = 226 - chipW;
  gfx->fillRoundRect(chipX, 40, chipW, 16, 8, C_TND_EMBER);
  gfx->setTextSize(1);
  gfx->setTextColor(C_TND_PAPER, C_TND_EMBER);
  gfx->setCursor(chipX + 7, 44);
  gfx->print(label);
}

void petScreenBegin() {
  initIfNeeded();
  updateDayRollover();
  const PetMood mood = resolveMood();
  g_shownMood = mood;
  g_lastTickSec = 0;

  tendHeader("DESK CREATURE");
  tendHeaderClock();

  gfx->setTextSize(3);
  gfx->setTextColor(C_TND_INK, C_TND_PAPER);
  gfx->setCursor(14, 38);
  gfx->print("EMBER");
  drawMoodChip(mood);
  gfx->drawFastHLine(14, 66, 212, C_TND_LINE);

  // Ground shadow, then the static body (double outline for weight).
  gfx->drawEllipse(PET_CX, BODY_TOP + BODY_H + 6, 46, 5, C_TND_LINE);
  gfx->drawRoundRect(PET_CX - BODY_W / 2, BODY_TOP, BODY_W, BODY_H, 36, C_TND_INK);
  gfx->drawRoundRect(PET_CX - BODY_W / 2 + 1, BODY_TOP + 1, BODY_W - 2, BODY_H - 2, 35, C_TND_INK);

  // Feet.
  gfx->fillRoundRect(PET_CX - 34, BODY_TOP + BODY_H - 4, 22, 9, 4, C_TND_INK);
  gfx->fillRoundRect(PET_CX + 12, BODY_TOP + BODY_H - 4, 22, 9, 4, C_TND_INK);

  drawFlame(false);
  drawEyes(mood, false);
  drawMouth(mood);

  if (mood == PET_HAPPY) {
    // Blush strokes + hearts beside the body for the glow.
    for (int side = -1; side <= 1; side += 2) {
      const int bx = PET_CX + side * (EYE_DX + 9);
      gfx->drawLine(bx - 3, EYE_Y + 12, bx + 3, EYE_Y + 14, C_TND_EMBER);
      gfx->drawLine(bx - 3, EYE_Y + 16, bx + 3, EYE_Y + 18, C_TND_EMBER);
    }
    drawHeart(PET_CX - BODY_W / 2 - 22, BODY_TOP + 18, 4);
    drawHeart(PET_CX + BODY_W / 2 + 20, BODY_TOP + 26, 3);
    drawHeart(PET_CX + BODY_W / 2 + 34, BODY_TOP + 4, 2);
  } else if (mood == PET_SLEEPING) {
    gfx->setTextSize(1);
    gfx->setTextColor(C_TND_MUTE, C_TND_PAPER);
    gfx->setCursor(PET_CX + BODY_W / 2 + 8, BODY_TOP + 10);
    gfx->print("z");
    gfx->setCursor(PET_CX + BODY_W / 2 + 16, BODY_TOP);
    gfx->print("z");
    gfx->setTextSize(2);
    gfx->setCursor(PET_CX + BODY_W / 2 + 22, BODY_TOP - 16);
    gfx->print("z");
  }

  drawQuip(mood);

  // Footer stat line: age / pets / uptime.
  const long age = ageDays();
  const unsigned long upH = millis() / 3600000UL;
  String stats = "age " + (age > 0 ? String(age) + "d" : String("--")) +
                 "  -  pets " + String(g_petsToday) + "/" + String((unsigned long)g_petsTotal) +
                 "  -  up " + String(upH / 24UL) + "d" + pad2((int)(upH % 24UL)) + "h";
  printCentered(224, 1, stats, C_TND_MUTE, C_TND_PAPER);
}

void petScreenTick() {
  const PetMood mood = resolveMood();
  if (mood != g_shownMood) {   // glow expiring / sleep window edges
    petScreenBegin();
    return;
  }

  // Blink on a fixed cycle (COZY only; the other moods use drawn eye shapes).
  if (mood == PET_COZY) {
    const bool closed = (millis() % 3400UL) < 140UL;
    if (closed != g_eyesClosed) drawEyes(mood, closed);
  }

  // Flame flicker (snuffed while sleeping, like the ESP32).
  if (mood != PET_SLEEPING) {
    const uint8_t phase = (uint8_t)((millis() / 220UL) & 1UL);
    if (phase != g_flamePhase) drawFlame(phase != 0);
  }

  // Quip rotation + header clock, once a second.
  const unsigned long sec = millis() / 1000UL;
  if (sec == g_lastTickSec) return;
  g_lastTickSec = sec;
  tendHeaderClock();
  if ((uint8_t)((millis() / 7000UL) & 0xFF) != g_shownQuipSlot) drawQuip(mood);
}
