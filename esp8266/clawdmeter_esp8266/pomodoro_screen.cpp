// Pomodoro screen, ported from firmware/main/pomodoro_screen.cpp (ESP32).
// 25-minute countdown drawn as a Tend ring on the 240x240 panel. The ESP32's
// action button becomes dashboard buttons (/pomodoro?action=toggle|reset) and
// the audio chime becomes a screen-only alert: when the countdown fires,
// pomodoroTick() pulls this screen forward via tendShowScreen.
//
// The countdown state machine ticks every loop() pass regardless of the
// visible screen, so a session keeps advancing in the background. Rendering is
// incremental: the ring track is painted once by Begin and the tick only adds
// the newly swept arc and overprints the MM:SS numeral, so nothing flickers.
#include "tend.h"

#include <math.h>

static const int POMODORO_MINUTES = 25;
static const int ALERT_TIMEOUT_SEC = 30;

enum PomoState : uint8_t { POMO_IDLE, POMO_RUNNING, POMO_PAUSED, POMO_DONE };

static PomoState g_state = POMO_IDLE;
static unsigned long g_deadlineMs = 0;       // valid when RUNNING
static int g_pausedRemainingSec = POMODORO_MINUTES * 60;  // valid IDLE/PAUSED
static unsigned long g_alertStartedMs = 0;   // valid when DONE

// Render cache so the tick repaints only what changed.
static int g_shownSec = -1;
static float g_shownFrac = 0.0f;
static int g_shownAlertRemain = -1;
static unsigned long g_lastTickSec = 0;

static const int RING_CX = 120;
static const int RING_CY = 136;
static const int RING_R = 70;
static const int RING_T = 10;

static int totalSeconds() { return POMODORO_MINUTES * 60; }

static int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int pomodoroRemainingSec() {
  if (g_state == POMO_RUNNING) {
    long remain = (long)(g_deadlineMs - millis()) / 1000L;
    return clampInt((int)remain, 0, totalSeconds());
  }
  if (g_state == POMO_DONE) return 0;
  return g_pausedRemainingSec;
}

const char *pomodoroStateName() {
  switch (g_state) {
    case POMO_RUNNING: return "running";
    case POMO_PAUSED:  return "paused";
    case POMO_DONE:    return "done";
    default:           return "idle";
  }
}

void pomodoroToggleStartPause() {
  switch (g_state) {
    case POMO_IDLE:
    case POMO_PAUSED:
      g_deadlineMs = millis() + (unsigned long)g_pausedRemainingSec * 1000UL;
      g_state = POMO_RUNNING;
      break;
    case POMO_RUNNING:
      g_pausedRemainingSec = pomodoroRemainingSec();
      g_state = POMO_PAUSED;
      break;
    case POMO_DONE:
      // Only reset exits the done state (same contract as the ESP32).
      break;
  }
}

void pomodoroReset() {
  g_state = POMO_IDLE;
  g_pausedRemainingSec = totalSeconds();
  g_deadlineMs = 0;
  g_alertStartedMs = 0;
}

void pomodoroTick() {
  if (g_state != POMO_RUNNING) return;
  if ((long)(millis() - g_deadlineMs) >= 0) {
    g_state = POMO_DONE;
    g_alertStartedMs = millis();
    g_pausedRemainingSec = 0;
    // No chime on this box: the alert is the screen itself.
    tendShowScreen(SCREEN_POMODORO);
  }
}

// Clockwise pie-ring sweep from 12 o'clock, fraction range [from, to] of the
// full circle -- the ESP32 helper split so the tick can add just the delta.
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

static const char *statePrompt() {
  switch (g_state) {
    case POMO_RUNNING: return "session running";
    case POMO_PAUSED:  return "paused - resume from dashboard";
    default:           return "start from the dashboard";
  }
}

static float elapsedFraction() {
  const int total = totalSeconds();
  const int elapsed = clampInt(total - pomodoroRemainingSec(), 0, total);
  return total > 0 ? (float)elapsed / (float)total : 0.0f;
}

static void drawRemaining() {
  const int sec = pomodoroRemainingSec();
  gfx->fillRect(RING_CX - 46, 126, 92, 24, C_TND_PAPER);
  String t = pad2(sec / 60) + ":" + pad2(sec % 60);
  printCentered(126, 3, t, C_TND_INK, C_TND_PAPER);
  g_shownSec = sec;
}

static void drawAlertBar(int remain) {
  const int x = 40, y = 168, w = 160, h = 12;
  gfx->fillRect(x + 1, y + 1, w - 2, h - 2, C_TND_PAPER_DEEP);
  gfx->drawRect(x, y, w, h, C_TND_LINE);
  const int filled = (w - 2) * remain / ALERT_TIMEOUT_SEC;
  if (filled > 0) gfx->fillRect(x + 1, y + 1, filled, h - 2, C_TND_EMBER);
  g_shownAlertRemain = remain;
}

static void beginDone() {
  drawTendFlame(120, 74);
  printCentered(100, 3, "TIME'S UP", C_TND_EMBER, C_TND_PAPER);
  printCentered(136, 1, "another pomodoro in the jar", C_TND_INK_SOFT, C_TND_PAPER);
  const int elapsed = (int)((millis() - g_alertStartedMs) / 1000UL);
  drawAlertBar(clampInt(ALERT_TIMEOUT_SEC - elapsed, 0, ALERT_TIMEOUT_SEC));
  printCentered(196, 1, "reset from the dashboard", C_TND_MUTE, C_TND_PAPER);
}

void pomodoroScreenBegin() {
  tendHeader("POMODORO");
  tendHeaderClock();
  g_shownSec = -1;
  g_shownAlertRemain = -1;
  g_lastTickSec = 0;

  if (g_state == POMO_DONE) {
    beginDone();
    return;
  }

  printCentered(40, 1, statePrompt(), C_TND_INK_SOFT, C_TND_PAPER);

  // Ring track as an annulus (two filled circles), then the elapsed arc.
  gfx->fillCircle(RING_CX, RING_CY, RING_R, C_TND_PAPER_DEEP);
  gfx->fillCircle(RING_CX, RING_CY, RING_R - RING_T, C_TND_PAPER);
  g_shownFrac = elapsedFraction();
  drawRingArc(0.0f, g_shownFrac, C_TND_EMBER);

  printCentered(110, 1, "time left", C_TND_MUTE, C_TND_PAPER);
  drawRemaining();

  printCentered(224, 1, "dashboard: start / pause - reset", C_TND_MUTE, C_TND_PAPER);
}

void pomodoroScreenTick() {
  const unsigned long sec = millis() / 1000UL;
  if (sec == g_lastTickSec) return;   // everything below runs once a second
  g_lastTickSec = sec;

  if (g_state == POMO_DONE) {
    const int elapsed = (int)((millis() - g_alertStartedMs) / 1000UL);
    const int remain = clampInt(ALERT_TIMEOUT_SEC - elapsed, 0, ALERT_TIMEOUT_SEC);
    if (remain != g_shownAlertRemain) drawAlertBar(remain);
    return;
  }

  tendHeaderClock();
  if (g_state != POMO_RUNNING) return;

  if (pomodoroRemainingSec() != g_shownSec) drawRemaining();
  const float frac = elapsedFraction();
  if (frac > g_shownFrac) {
    drawRingArc(g_shownFrac, frac, C_TND_EMBER);
    g_shownFrac = frac;
  }
}
