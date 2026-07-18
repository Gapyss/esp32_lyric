// Hydration screen, ported from firmware/main/water_screen.cpp (ESP32).
// A reminder ring that fills toward the next drink, an alert card when it
// fires, and a daily drink counter. The interval and active window persist in
// EEPROM (the ESP32 used NVS); the button becomes /hydrate/* dashboard
// endpoints and the chime becomes a screen-only alert that pulls this screen
// forward. waterTick() runs every loop() pass so the reminder fires even
// while another screen is visible.
#include "tend.h"

#include <EEPROM.h>
#include <math.h>

static const int ALERT_TIMEOUT_SEC = 30;

enum WaterState : uint8_t { WATER_IDLE, WATER_ALERTING };

static bool g_inited = false;
static WaterState g_state = WATER_IDLE;
static unsigned long g_lastReminderMs = 0;
static unsigned long g_alertStartedMs = 0;
static int g_intervalMin = 45;
static int g_activeStartMin = 9 * 60;
static int g_activeEndMin = 18 * 60;
static int g_drinksToday = 0;
static long g_dayMarker = 0;   // local epoch day; 0 = unknown yet

// Render cache.
static int g_shownSec = -1;
static float g_shownFrac = 0.0f;
static int g_shownAlertRemain = -1;
static int g_shownDrinks = -1;
static uint8_t g_shownState = 255;
static unsigned long g_lastTickSec = 0;

static const int RING_CX = 120;
static const int RING_CY = 130;
static const int RING_R = 68;
static const int RING_T = 10;

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void initIfNeeded() {
  if (g_inited) return;
  g_inited = true;
  uint16_t v = 0;
  EEPROM.get(EE_WATER_INTERVAL, v);
  g_intervalMin = clampInt(v, 1, 24 * 60);
  EEPROM.get(EE_WATER_START, v);
  g_activeStartMin = clampInt(v, 0, 24 * 60 - 1);
  EEPROM.get(EE_WATER_END, v);
  g_activeEndMin = clampInt(v, 0, 24 * 60 - 1);
  g_lastReminderMs = millis();
}

int waterIntervalMin() { initIfNeeded(); return g_intervalMin; }
int waterStartMin() { initIfNeeded(); return g_activeStartMin; }
int waterEndMin() { initIfNeeded(); return g_activeEndMin; }
int waterDrinksToday() { return g_drinksToday; }
bool waterAlerting() { return g_state == WATER_ALERTING; }

int waterNextInSec() {
  initIfNeeded();
  const int intervalSec = g_intervalMin * 60;
  const int elapsed = (int)((millis() - g_lastReminderMs) / 1000UL);
  return clampInt(intervalSec - elapsed, 0, intervalSec);
}

// Reminders only fire inside the active window; before the clock is known the
// window can't be evaluated and reminders stay allowed (same as the ESP32).
static bool activeWindowAllows() {
  const unsigned long e = nowEpoch();
  if (e == 0) return true;
  const int nowMin = (int)(((e + TZ_OFFSET) % 86400UL) / 60UL);
  if (g_activeStartMin == g_activeEndMin) return false;
  if (g_activeStartMin < g_activeEndMin) {
    return nowMin >= g_activeStartMin && nowMin < g_activeEndMin;
  }
  return nowMin >= g_activeStartMin || nowMin < g_activeEndMin;
}

static void updateDayRollover() {
  const unsigned long e = nowEpoch();
  if (e == 0) return;
  const long marker = (long)((e + TZ_OFFSET) / 86400UL);
  if (marker == g_dayMarker) return;
  g_dayMarker = marker;
  g_drinksToday = 0;
}

void waterTick() {
  initIfNeeded();
  updateDayRollover();
  if (g_state == WATER_ALERTING) {
    if ((int)((millis() - g_alertStartedMs) / 1000UL) >= ALERT_TIMEOUT_SEC) {
      g_state = WATER_IDLE;
      g_lastReminderMs = millis();
    }
  } else if (waterNextInSec() == 0 && activeWindowAllows()) {
    g_state = WATER_ALERTING;
    g_alertStartedMs = millis();
    tendShowScreen(SCREEN_WATER);   // no chime -- the alert is the screen
  }
}

void waterFireNow() {
  initIfNeeded();
  if (g_state == WATER_ALERTING) return;
  g_state = WATER_ALERTING;
  g_alertStartedMs = millis();
}

void waterLogDrink() {
  initIfNeeded();
  updateDayRollover();
  g_drinksToday++;
  g_state = WATER_IDLE;
  g_lastReminderMs = millis();
}

void waterSnooze(int minutes) {
  initIfNeeded();
  minutes = clampInt(minutes, 1, 24 * 60);
  g_state = WATER_IDLE;
  // Backdate the last reminder so the next one lands `minutes` from now.
  g_lastReminderMs = millis() - (unsigned long)(g_intervalMin - minutes) * 60000UL;
}

bool waterConfigure(int intervalMin, int startMin, int endMin) {
  initIfNeeded();
  if (intervalMin <= 0 || startMin < 0 || startMin >= 24 * 60 ||
      endMin < 0 || endMin >= 24 * 60) {
    return false;
  }
  g_intervalMin = clampInt(intervalMin, 1, 24 * 60);
  g_activeStartMin = startMin;
  g_activeEndMin = endMin;
  g_lastReminderMs = millis();
  EEPROM.put(EE_WATER_INTERVAL, (uint16_t)g_intervalMin);
  EEPROM.put(EE_WATER_START, (uint16_t)g_activeStartMin);
  EEPROM.put(EE_WATER_END, (uint16_t)g_activeEndMin);
  tendEepromCommit();
  return true;
}

// ---- rendering -------------------------------------------------------------

static void drawRingArc(float fromFrac, float toFrac, uint16_t color) {
  if (toFrac <= fromFrac) return;
  if (toFrac > 1.0f) toFrac = 1.0f;
  const int rInner = RING_R - RING_T;
  const float sweep0 = 360.0f * fromFrac;
  const float sweep1 = 360.0f * toFrac;
  const int steps = (int)((sweep1 - sweep0) * 2.0f) + 1;
  for (int i = 0; i <= steps; i++) {
    const float a = (-90.0f + sweep0 + (sweep1 - sweep0) * i / steps) * (float)M_PI / 180.0f;
    const float dx = cosf(a);
    const float dy = sinf(a);
    for (int r = rInner; r <= RING_R; r++) {
      gfx->drawPixel(RING_CX + (int)(dx * (float)r), RING_CY + (int)(dy * (float)r), color);
    }
  }
}

static float elapsedFraction() {
  const int intervalSec = g_intervalMin * 60;
  const int elapsed = clampInt(intervalSec - waterNextInSec(), 0, intervalSec);
  return intervalSec > 0 ? (float)elapsed / (float)intervalSec : 0.0f;
}

static void drawDrinksLine() {
  gfx->fillRect(0, 38, 240, 10, C_TND_PAPER);
  String line = String(g_drinksToday) + " drink" + (g_drinksToday == 1 ? "" : "s") +
                " logged today";
  printCentered(38, 1, line, C_TND_INK_SOFT, C_TND_PAPER);
  g_shownDrinks = g_drinksToday;
}

static void drawNextIn() {
  const int sec = waterNextInSec();
  gfx->fillRect(RING_CX - 46, 120, 92, 24, C_TND_PAPER);
  String t = pad2(sec / 60) + ":" + pad2(sec % 60);
  printCentered(120, 3, t, C_TND_INK, C_TND_PAPER);
  g_shownSec = sec;
}

static void drawStatCard(int x, int w, const char *label, const String &value) {
  const int y = 192, h = 34;
  gfx->drawRoundRect(x, y, w, h, 8, C_TND_LINE);
  gfx->setTextSize(1);
  gfx->setTextColor(C_TND_MUTE, C_TND_PAPER);
  gfx->setCursor(x + 9, y + 6);
  gfx->print(label);
  gfx->setTextColor(C_TND_INK, C_TND_PAPER);
  gfx->setCursor(x + 9, y + 19);
  gfx->print(value);
}

static void drawAlertBar(int remain) {
  const int x = 40, y = 176, w = 160, h = 12;
  gfx->fillRect(x + 1, y + 1, w - 2, h - 2, C_TND_PAPER_DEEP);
  gfx->drawRect(x, y, w, h, C_TND_LINE);
  const int filled = (w - 2) * remain / ALERT_TIMEOUT_SEC;
  if (filled > 0) gfx->fillRect(x + 1, y + 1, filled, h - 2, C_TND_EMBER);
  g_shownAlertRemain = remain;
}

static void drawWaterDrop(int cx, int cy, int r) {
  const int discCy = cy + r / 3;
  gfx->fillCircle(cx, discCy, r, C_TND_INFO);
  gfx->fillTriangle(cx - r, discCy, cx + r, discCy, cx, cy - r, C_TND_INFO);
}

static void beginAlert() {
  gfx->fillScreen(C_TND_PAPER);
  drawWaterDrop(120, 64, 26);
  printCentered(112, 3, "DRINK WATER", C_TND_INK, C_TND_PAPER);
  printCentered(148, 1, "log it from the dashboard", C_TND_INK_SOFT, C_TND_PAPER);
  const int elapsed = (int)((millis() - g_alertStartedMs) / 1000UL);
  drawAlertBar(clampInt(ALERT_TIMEOUT_SEC - elapsed, 0, ALERT_TIMEOUT_SEC));
  printCentered(212, 1, "hydration reminder", C_TND_MUTE, C_TND_PAPER);
}

void waterScreenBegin() {
  initIfNeeded();
  g_shownState = g_state;
  g_shownAlertRemain = -1;
  g_lastTickSec = 0;

  if (g_state == WATER_ALERTING) {
    beginAlert();
    return;
  }

  tendHeader("HYDRATION");
  tendHeaderClock();
  drawDrinksLine();

  gfx->fillCircle(RING_CX, RING_CY, RING_R, C_TND_PAPER_DEEP);
  gfx->fillCircle(RING_CX, RING_CY, RING_R - RING_T, C_TND_PAPER);
  g_shownFrac = elapsedFraction();
  drawRingArc(0.0f, g_shownFrac, C_TND_INFO);

  printCentered(104, 1, "next drink in", C_TND_MUTE, C_TND_PAPER);
  drawNextIn();

  String active = pad2(g_activeStartMin / 60) + ":" + pad2(g_activeStartMin % 60) +
                  "-" + pad2(g_activeEndMin / 60) + ":" + pad2(g_activeEndMin % 60);
  drawStatCard(14, 104, "active", active);
  drawStatCard(122, 104, "interval", String(g_intervalMin) + " min");

  printCentered(232, 1, "dashboard: log - snooze - remind now", C_TND_MUTE, C_TND_PAPER);
}

void waterScreenTick() {
  // State flips repaint the whole card (both directions run through here:
  // fire-while-visible and timeout-back-to-idle).
  if (g_state != g_shownState) {
    waterScreenBegin();
    return;
  }

  const unsigned long sec = millis() / 1000UL;
  if (sec == g_lastTickSec) return;
  g_lastTickSec = sec;

  if (g_state == WATER_ALERTING) {
    const int elapsed = (int)((millis() - g_alertStartedMs) / 1000UL);
    const int remain = clampInt(ALERT_TIMEOUT_SEC - elapsed, 0, ALERT_TIMEOUT_SEC);
    if (remain != g_shownAlertRemain) drawAlertBar(remain);
    return;
  }

  tendHeaderClock();
  if (g_drinksToday != g_shownDrinks) drawDrinksLine();
  if (waterNextInSec() != g_shownSec) drawNextIn();
  const float frac = elapsedFraction();
  if (frac > g_shownFrac) {
    drawRingArc(g_shownFrac, frac, C_TND_INFO);
    g_shownFrac = frac;
  }
}
