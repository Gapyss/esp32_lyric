// Firefly-clock screen, ported from firmware/main/swarm_screen.cpp (ESP32).
// A swarm of fireflies steered by a spring-and-damper toward 7-segment target
// points sampled along the current HH:MM; scatter sends them flying, roam
// frees them entirely. Scaled from the ESP32's 300 flies on a 400x272 field
// to 96 flies on 240x212 (footer band below). Positions are 24.8 fixed point.
//
// No framebuffer on this panel, so each fly erases its previous 3x3 footprint
// and redraws at the new spot every sim step -- ember dots on paper, with the
// occasional warm flare, per the Tend one-loud-color rule.
#include "tend.h"

#define FIELD_W 240
#define FIELD_H 212
#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)

#define NUM_FLIES 96
#define MAX_TARGETS 140

static const unsigned long SIM_TICK_MS = 45;
static const int ATTRACT_SHIFT = 5;
static const int DAMP_NUM = 230;  // of 256
static const int JITTER_DAY = 40;
static const int JITTER_NIGHT = 14;
static const int NIGHT_START_HOUR = 22;
static const int NIGHT_END_HOUR = 7;
static const int32_t MAX_SPEED = 3 * FP_ONE;
static const int32_t MAX_SPEED_SCATTER = 7 * FP_ONE;
static const unsigned long SCATTER_DURATION_MS = 1600;

// 7-segment digit geometry in pixels, scaled for 240 wide. Targets are
// sampled along segment centerlines every SAMPLE_SPACING px; with ~96 flies
// the spacing is tuned so a typical time is fully perched.
static const int DIG_W = 34;
static const int DIG_H = 68;
static const int DIG_TOP = 64;
static const int DIG_X[4] = {22, 72, 134, 184};
static const int COLON_CX = 120;
static const int SAMPLE_SPACING = 10;

static const uint8_t SEG_FOR_DIGIT[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};
static const uint8_t SEG_DASH = 0x40;

typedef struct {
  int32_t px, py;  // 24.8 fixed-point position
  int32_t vx, vy;  // 24.8 fixed-point velocity
  int8_t ox, oy;   // personal perch offset in px, so shared targets cluster
  uint8_t phase;   // desynchronizes the blink
} Fly;

static Fly g_flies[NUM_FLIES];
static uint8_t g_tx[MAX_TARGETS];        // x/2 fits a byte at 240 wide
static uint8_t g_ty[MAX_TARGETS];
static int g_numTargets = 0;
static uint32_t g_rng = 0;
static uint32_t g_frame = 0;
static uint32_t g_assignSalt = 0;
static bool g_roam = false;
static unsigned long g_scatterUntilMs = 0;
static int g_targetKey = -2;
static unsigned long g_lastSimMs = 0;
static bool g_shownRoam = false;

static uint32_t rnd() {
  uint32_t x = g_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_rng = x;
  return x;
}

static void initIfNeeded() {
  if (g_rng != 0) return;
  g_rng = ESP.getCycleCount() | 1;
  for (int i = 0; i < NUM_FLIES; i++) {
    Fly *f = &g_flies[i];
    f->px = (int32_t)(rnd() % FIELD_W) << FP_SHIFT;
    f->py = (int32_t)(rnd() % FIELD_H) << FP_SHIFT;
    f->vx = 0;
    f->vy = 0;
    f->ox = (int8_t)(rnd() % 5) - 2;
    f->oy = (int8_t)(rnd() % 5) - 2;
    f->phase = (uint8_t)rnd();
  }
  g_targetKey = -2;
}

static bool clockNow(int *hour, int *minute) {
  const unsigned long e = nowEpoch();
  if (e == 0) return false;
  const unsigned long s = (e + TZ_OFFSET) % 86400UL;
  *hour = (int)(s / 3600UL);
  *minute = (int)((s % 3600UL) / 60UL);
  return true;
}

static void addTarget(int x, int y) {
  if (g_numTargets >= MAX_TARGETS) return;
  g_tx[g_numTargets] = (uint8_t)(x / 2);
  g_ty[g_numTargets] = (uint8_t)y;
  g_numTargets++;
}

static void sampleLine(int x0, int y0, int x1, int y1) {
  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int len = (dx > -dx ? dx : -dx) + (dy > -dy ? dy : -dy);
  int n = len / SAMPLE_SPACING;
  if (n < 1) n = 1;
  for (int i = 0; i <= n; i++) {
    addTarget(x0 + dx * i / n, y0 + dy * i / n);
  }
}

static void placeDigit(int x, uint8_t seg) {
  const int y = DIG_TOP;
  const int hh = DIG_H / 2;
  if (seg & 0x01) sampleLine(x, y, x + DIG_W, y);
  if (seg & 0x02) sampleLine(x + DIG_W, y, x + DIG_W, y + hh);
  if (seg & 0x04) sampleLine(x + DIG_W, y + hh, x + DIG_W, y + DIG_H);
  if (seg & 0x08) sampleLine(x, y + DIG_H, x + DIG_W, y + DIG_H);
  if (seg & 0x10) sampleLine(x, y + hh, x, y + DIG_H);
  if (seg & 0x20) sampleLine(x, y, x, y + hh);
  if (seg & 0x40) sampleLine(x, y + hh, x + DIG_W, y + hh);
}

static void placeColonDot(int cy) {
  addTarget(COLON_CX - 3, cy - 3);
  addTarget(COLON_CX + 3, cy - 3);
  addTarget(COLON_CX - 3, cy + 3);
  addTarget(COLON_CX + 3, cy + 3);
}

static void rebuildTargetsIfTimeChanged() {
  int h = 0, m = 0;
  const int key = clockNow(&h, &m) ? h * 60 + m : -1;
  if (key == g_targetKey) return;
  g_targetKey = key;
  g_numTargets = 0;

  if (key >= 0) {
    placeDigit(DIG_X[0], SEG_FOR_DIGIT[h / 10]);
    placeDigit(DIG_X[1], SEG_FOR_DIGIT[h % 10]);
    placeDigit(DIG_X[2], SEG_FOR_DIGIT[m / 10]);
    placeDigit(DIG_X[3], SEG_FOR_DIGIT[m % 10]);
  } else {
    for (int i = 0; i < 4; i++) placeDigit(DIG_X[i], SEG_DASH);
  }
  placeColonDot(DIG_TOP + DIG_H / 2 - 14);
  placeColonDot(DIG_TOP + DIG_H / 2 + 14);

  // Rotated on each rebuild so flies swap perches (criss-cross, not slide).
  g_assignSalt = rnd();
}

static bool isNight() {
  int h = 0, m = 0;
  if (!clockNow(&h, &m)) return false;
  return h >= NIGHT_START_HOUR || h < NIGHT_END_HOUR;
}

// Which target fly i perches on: an even spread over the target list.
static int targetIndexFor(int i) {
  const int j = (int)((i + g_assignSalt) % NUM_FLIES);
  return j * g_numTargets / NUM_FLIES;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static void simStepAndPaint() {
  g_frame++;
  const bool scattering = millis() < g_scatterUntilMs;
  const bool homing = !scattering && !g_roam && g_numTargets > 0;
  const int jitter = isNight() ? JITTER_NIGHT : JITTER_DAY;
  const int32_t maxSpeed = scattering ? MAX_SPEED_SCATTER : MAX_SPEED;

  for (int i = 0; i < NUM_FLIES; i++) {
    Fly *f = &g_flies[i];
    const int oldX = (int)(f->px >> FP_SHIFT);
    const int oldY = (int)(f->py >> FP_SHIFT);

    if (homing) {
      const int t = targetIndexFor(i);
      const int32_t tx = ((int32_t)g_tx[t] * 2 + f->ox) << FP_SHIFT;
      const int32_t ty = ((int32_t)g_ty[t] + f->oy) << FP_SHIFT;
      f->vx += (tx - f->px) >> ATTRACT_SHIFT;
      f->vy += (ty - f->py) >> ATTRACT_SHIFT;
    }

    f->vx += (int32_t)(rnd() % (2 * jitter + 1)) - jitter;
    f->vy += (int32_t)(rnd() % (2 * jitter + 1)) - jitter;
    f->vx = f->vx * DAMP_NUM / 256;
    f->vy = f->vy * DAMP_NUM / 256;
    f->vx = clamp32(f->vx, -maxSpeed, maxSpeed);
    f->vy = clamp32(f->vy, -maxSpeed, maxSpeed);
    f->px += f->vx;
    f->py += f->vy;

    // Bounce off the field edges.
    if (f->px < 0) {
      f->px = -f->px;
      f->vx = -f->vx;
    } else if (f->px > (FIELD_W - 3) * FP_ONE) {
      f->px = 2 * (FIELD_W - 3) * FP_ONE - f->px;
      f->vx = -f->vx;
    }
    if (f->py < 0) {
      f->py = -f->py;
      f->vy = -f->vy;
    } else if (f->py > (FIELD_H - 3) * FP_ONE) {
      f->py = 2 * (FIELD_H - 3) * FP_ONE - f->py;
      f->vy = -f->vy;
    }

    // Erase the old 3x3 footprint (covers dot and flare), then redraw.
    const int x = (int)(f->px >> FP_SHIFT);
    const int y = (int)(f->py >> FP_SHIFT);
    if (x != oldX || y != oldY) {
      gfx->fillRect(oldX > 0 ? oldX - 1 : 0, oldY > 0 ? oldY - 1 : 0, 3, 3, C_TND_PAPER);
    }
    const uint32_t tick = g_frame + f->phase;
    if (tick % 43 == 0) {
      // Firefly blink: wink out for one frame.
      gfx->fillRect(x > 0 ? x - 1 : 0, y > 0 ? y - 1 : 0, 3, 3, C_TND_PAPER);
    } else if (tick % 37 < 2) {
      gfx->fillRect(x > 0 ? x - 1 : 0, y > 0 ? y - 1 : 0, 3, 3, C_TND_WARN);   // flare
    } else {
      if (x == oldX && y == oldY) {
        gfx->fillRect(x > 0 ? x - 1 : 0, y > 0 ? y - 1 : 0, 3, 3, C_TND_PAPER);
      }
      gfx->fillRect(x, y, 2, 2, C_TND_EMBER);
    }
  }
}

void swarmScatter() {
  initIfNeeded();
  g_scatterUntilMs = millis() + SCATTER_DURATION_MS;
  for (int i = 0; i < NUM_FLIES; i++) {
    Fly *f = &g_flies[i];
    f->vx += (int32_t)(rnd() % (2 * MAX_SPEED_SCATTER + 1)) - MAX_SPEED_SCATTER;
    f->vy += (int32_t)(rnd() % (2 * MAX_SPEED_SCATTER + 1)) - MAX_SPEED_SCATTER;
  }
}

void swarmToggleRoam() {
  g_roam = !g_roam;
}

bool swarmRoaming() { return g_roam; }

static void drawFooterMode() {
  gfx->fillRect(160, 226, 66, 8, C_TND_PAPER);
  printRight(226, 226, 1, g_roam ? String("roam") : String("time"), C_TND_MUTE, C_TND_PAPER);
  g_shownRoam = g_roam;
}

void swarmScreenBegin() {
  initIfNeeded();
  gfx->fillScreen(C_TND_PAPER);
  gfx->drawFastHLine(14, 220, 212, C_TND_LINE);
  gfx->setTextSize(1);
  gfx->setTextColor(C_TND_MUTE, C_TND_PAPER);
  gfx->setCursor(14, 226);
  gfx->print("firefly clock - scatter / roam");
  drawFooterMode();
  g_lastSimMs = 0;
}

void swarmScreenTick() {
  if (lcdScreen != SCREEN_SWARM) return;
  const unsigned long now = millis();
  if (now - g_lastSimMs < SIM_TICK_MS) return;
  g_lastSimMs = now;

  rebuildTargetsIfTimeChanged();
  simStepAndPaint();
  if (g_roam != g_shownRoam) drawFooterMode();
}
