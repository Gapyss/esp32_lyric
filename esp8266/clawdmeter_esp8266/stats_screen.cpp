// Stats screen, ported from firmware/main/stats_screen.cpp (ESP32) and
// rethought for this box: there is no AHT20/battery sensor here, so the trend
// cards plot the Mac metrics the daemon already pushes with /usage (CPU and
// memory), with disk and battery as the bottom stat cards. History accrues on
// every push regardless of the visible screen, same contract as the ESP32's
// board_peripherals history.
#include "tend.h"

#define HIST_CAP 120

static int8_t g_cpuHist[HIST_CAP];
static int8_t g_memHist[HIST_CAP];
static int g_histCount = 0;
static unsigned long g_lastSampleMs = 0;
static bool g_dirty = false;
static unsigned long g_lastTickSec = 0;

// Sample on each daemon push, at most every 30 s (pushes arrive ~1/min, so
// the 120-slot ring spans about two hours).
void statsOnUsagePush() {
  if (macCpuPct < 0 && macMemPct < 0) return;
  if (g_histCount > 0 && millis() - g_lastSampleMs < 30000UL) return;
  g_lastSampleMs = millis();
  if (g_histCount == HIST_CAP) {
    memmove(g_cpuHist, g_cpuHist + 1, HIST_CAP - 1);
    memmove(g_memHist, g_memHist + 1, HIST_CAP - 1);
    g_histCount--;
  }
  g_cpuHist[g_histCount] = (int8_t)(macCpuPct < 0 ? 0 : (macCpuPct > 100 ? 100 : macCpuPct));
  g_memHist[g_histCount] = (int8_t)(macMemPct < 0 ? 0 : (macMemPct > 100 ? 100 : macMemPct));
  g_histCount++;
  g_dirty = true;
}

// Labeled Tend trend card: label, current value top right (ember only in the
// danger zone), hairline, then a line-graph of the ring buffer.
static void drawTrendCard(int y, int h, const char *label, int current,
                          const int8_t *values, int count, bool chrome) {
  const int x = 14, w = 212;
  if (chrome) {
    gfx->drawRoundRect(x, y, w, h, 8, C_TND_LINE);
    gfx->setTextSize(1);
    gfx->setTextColor(C_TND_MUTE, C_TND_PAPER);
    gfx->setCursor(x + 9, y + 6);
    gfx->print(label);
  }

  // Current value (clear a fixed zone; width varies).
  const bool danger = current >= 85;
  gfx->fillRect(x + w - 70, y + 4, 62, 16, C_TND_PAPER);
  printRight(x + w - 9, y + 4, 2, pctText(current),
             danger ? C_TND_EMBER : C_TND_INK, C_TND_PAPER);
  gfx->drawFastHLine(x + 8, y + 22, w - 16, C_TND_LINE);

  const int plotX = x + 8;
  const int plotY = y + 27;
  const int plotW = w - 16;
  const int plotH = h - 27 - 6;
  gfx->fillRect(plotX, plotY, plotW, plotH, C_TND_PAPER);

  if (count < 2) {
    printCentered(plotY + plotH / 2 - 4, 1, "collecting trend...", C_TND_FAINT, C_TND_PAPER);
    return;
  }

  int minV = values[0], maxV = values[0];
  for (int i = 1; i < count; i++) {
    if (values[i] < minV) minV = values[i];
    if (values[i] > maxV) maxV = values[i];
  }
  if (minV == maxV) maxV = minV + 1;

  int prevPx = 0, prevPy = 0;
  for (int i = 0; i < count; i++) {
    const int px = plotX + (plotW - 1) * i / (count - 1);
    const int py = plotY + (plotH - 1) - (plotH - 1) * (values[i] - minV) / (maxV - minV);
    if (i > 0) gfx->drawLine(prevPx, prevPy, px, py, C_TND_INK);
    prevPx = px;
    prevPy = py;
  }
}

static void drawBottomCard(int x, int w, const char *label, int pct, bool lowBad) {
  const int y = 206, h = 26;
  const bool danger = pct >= 0 && (lowBad ? pct <= 20 : pct >= 85);
  gfx->drawRoundRect(x, y, w, h, 8, C_TND_LINE);
  gfx->setTextSize(1);
  gfx->setTextColor(C_TND_MUTE, C_TND_PAPER);
  gfx->setCursor(x + 9, y + 9);
  gfx->print(label);
  gfx->fillRect(x + w - 44, y + 9, 36, 8, C_TND_PAPER);
  printRight(x + w - 9, y + 9, 1, pctText(pct),
             danger ? C_TND_EMBER : C_TND_INK, C_TND_PAPER);
}

static void drawValues(bool chrome) {
  drawTrendCard(38, 80, "cpu", macCpuPct, g_cpuHist, g_histCount, chrome);
  drawTrendCard(122, 80, "memory", macMemPct, g_memHist, g_histCount, chrome);
  drawBottomCard(14, 104, "disk", macDiskPct, false);
  drawBottomCard(122, 104, "battery", macBatteryPct, true);
  g_dirty = false;
}

void statsScreenBegin() {
  tendHeader("YOUR MAC");
  tendHeaderClock();
  drawValues(true);
}

void statsScreenTick() {
  if (g_dirty) drawValues(false);
  const unsigned long sec = millis() / 1000UL;
  if (sec == g_lastTickSec) return;
  g_lastTickSec = sec;
  tendHeaderClock();
}
