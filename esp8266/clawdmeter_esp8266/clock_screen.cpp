// Clock / year-grid screen, ported from firmware/main/clock_screen.cpp
// (ESP32). One square per day of the year: elapsed days filled ink, today the
// single ember accent, remaining days sunken paper. The ESP32's temperature /
// humidity trend cards had no sensor to feed them here and are dropped; the
// analog watch face gave way to the header clock (240px is a narrower card).
// Time comes from nowEpoch() (daemon push, NTP before that) in TZ_OFFSET.
#include "tend.h"

#include <time.h>

static const int GRID_COLS = 20;
static const int GRID_ROWS = 19;
static const int GRID_SQUARE = 5;

static long g_shownDayKey = -2;   // yyyy*1000+doy actually shown; -2 = never drawn
static unsigned long g_lastTickSec = 0;

static const char *const DOW_NAMES[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
static const char *const MON_NAMES[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                          "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

static bool localCalendar(struct tm *out) {
  const unsigned long e = nowEpoch();
  if (e == 0) return false;
  const time_t t = (time_t)(e + TZ_OFFSET);
  struct tm *g = gmtime(&t);
  if (g == NULL) return false;
  *out = *g;
  return true;
}

static long dayKeyOf(const struct tm &lt) {
  return (long)(lt.tm_year + 1900) * 1000L + lt.tm_yday;
}

static void drawYearGrid(int gx, int gy, int gw, int gh, int dayOfYear, int total) {
  const float pitchX = (float)gw / (float)GRID_COLS;
  const float pitchY = (float)gh / (float)GRID_ROWS;
  for (int i = 1; i <= total; i++) {
    const int idx = i - 1;
    const int col = idx % GRID_COLS;
    const int row = idx / GRID_COLS;
    const int cx = gx + (int)(col * pitchX + (pitchX - GRID_SQUARE) / 2.0f);
    const int cy = gy + (int)(row * pitchY + (pitchY - GRID_SQUARE) / 2.0f);
    if (i < dayOfYear) {
      gfx->fillRect(cx, cy, GRID_SQUARE, GRID_SQUARE, C_TND_INK);
    } else if (i == dayOfYear) {
      // Today: the one ember accent, with a halo ring like the ESP32.
      gfx->fillRect(cx, cy, GRID_SQUARE, GRID_SQUARE, C_TND_EMBER);
      gfx->drawRect(cx - 2, cy - 2, GRID_SQUARE + 4, GRID_SQUARE + 4, C_TND_EMBER);
    } else {
      gfx->fillRect(cx, cy, GRID_SQUARE, GRID_SQUARE, C_TND_PAPER_DEEP);
    }
  }
}

void clockScreenBegin() {
  tendHeader("DAY OF YEAR");
  tendHeaderClock();
  g_lastTickSec = 0;

  struct tm lt = {};
  if (!localCalendar(&lt)) {
    g_shownDayKey = -1;
    printCentered(120, 1, "waiting for time", C_TND_MUTE, C_TND_PAPER);
    printCentered(134, 1, "(daemon push or ntp)", C_TND_FAINT, C_TND_PAPER);
    return;
  }
  g_shownDayKey = dayKeyOf(lt);

  const int year = lt.tm_year + 1900;
  const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  const int total = leap ? 366 : 365;
  const int dayOfYear = lt.tm_yday + 1;
  const int daysLeft = total - dayOfYear;
  const int pct = dayOfYear * 100 / total;

  // Big day number + "/total" caption.
  String big = String(dayOfYear);
  gfx->setTextSize(4);
  gfx->setTextColor(C_TND_INK, C_TND_PAPER);
  gfx->setCursor(14, 42);
  gfx->print(big);
  gfx->setTextSize(1);
  gfx->setTextColor(C_TND_MUTE, C_TND_PAPER);
  gfx->setCursor(14 + textWidth(big, 4) + 6, 66);
  gfx->print("/ " + String(total));

  // Ember chip with the days-left callout (the ESP32's inverted ink chip).
  String chip = String(daysLeft) + " DAYS LEFT";
  const int chipW = textWidth(chip, 1) + 14;
  const int chipX = 226 - chipW;
  gfx->fillRoundRect(chipX, 40, chipW, 16, 8, C_TND_EMBER);
  gfx->setTextColor(C_TND_PAPER, C_TND_EMBER);
  gfx->setCursor(chipX + 7, 44);
  gfx->print(chip);

  printRight(226, 64, 1, String(pct) + "% ELAPSED", C_TND_MUTE, C_TND_PAPER);

  // Date line, then the grid card.
  String date = String(DOW_NAMES[lt.tm_wday]) + " " + String(lt.tm_mday) + " " +
                MON_NAMES[lt.tm_mon] + " " + String(year);
  gfx->setTextColor(C_TND_INK_SOFT, C_TND_PAPER);
  gfx->setCursor(14, 80);
  gfx->print(date);

  gfx->drawFastHLine(14, 94, 212, C_TND_LINE);
  drawYearGrid(14, 102, 212, 132, dayOfYear, total);
}

void clockScreenTick() {
  const unsigned long sec = millis() / 1000UL;
  if (sec == g_lastTickSec) return;
  g_lastTickSec = sec;

  tendHeaderClock();

  // Redraw the card when the day rolls over or the clock first syncs.
  struct tm lt = {};
  const long key = localCalendar(&lt) ? dayKeyOf(lt) : -1;
  if (key != g_shownDayKey) clockScreenBegin();
}
