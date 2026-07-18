// Sand-clock screen, ported from firmware/main/sand_screen.cpp (ESP32).
// Falling-sand simulation on a grid of 2x2-pixel cells; the current HH:MM is
// built as solid 7-segment walls the sand piles onto, and when the minute
// changes the walls are rebuilt and whatever rested on them falls. Scaled from
// the ESP32's 200x136 grid to 120x108 (240x216 px above a footer band), with
// the cell array packed 2 bits per cell to spare RAM.
//
// Unlike the ESP32 (which repainted the full frame at 14 fps into a panel
// framebuffer), this panel has no framebuffer -- so every cell mutation paints
// its 2x2 rect directly. Settled dunes never repaint; only the falling stream
// costs SPI traffic. Tend colors: digit walls in ink, sand in ember (the one
// loud color -- the sand is the show).
#include "tend.h"

#define SAND_W 120
#define SAND_H 108
#define SAND_CELL_PX 2

enum : uint8_t { CELL_EMPTY = 0, CELL_SAND = 1, CELL_WALL = 2 };

static const int SIM_STEPS_PER_TICK = 2;
static const unsigned long SIM_TICK_MS = 45;
static const int TARGET_GRAINS = 1500;
static const int SPAWN_P256_DAY = 20;
static const int SPAWN_P256_NIGHT = 5;
static const int SPAWN_P256_POUR = 230;
static const int NIGHT_START_HOUR = 22;
static const int NIGHT_END_HOUR = 7;
static const unsigned long POUR_DURATION_MS = 1200;

// 7-segment geometry in cells, scaled for the 120-wide grid.
static const int DIG_W = 16;
static const int DIG_H = 30;
static const int DIG_T = 4;
static const int DIG_TOP = 18;
static const int DIG_X[4] = {14, 36, 64, 86};
static const int COLON_X = 57;
static const int COLON_SIZE = 4;

// Segment bits: A=0x01 top, B=0x02 top-right, C=0x04 bottom-right, D=0x08
// bottom, E=0x10 bottom-left, F=0x20 top-left, G=0x40 middle.
static const uint8_t SEG_FOR_DIGIT[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};
static const uint8_t SEG_DASH = 0x40;

static uint8_t g_cells[(SAND_W * SAND_H) / 4];   // 2 bits per cell
static uint32_t g_rng = 1;
static uint32_t g_frame = 0;
static int g_grains = 0;
static int g_emitterX = SAND_W / 2;
static unsigned long g_pourUntilMs = 0;
static int g_obstacleKey = -2;   // hour*60+minute on screen; -1 = no clock; -2 = unbuilt
static unsigned long g_lastSimMs = 0;
static int g_shownGrains = -1;
static bool g_active = false;    // painting only while this screen is visible

static inline uint8_t cellGet(int x, int y) {
  const int i = y * SAND_W + x;
  return (g_cells[i >> 2] >> ((i & 3) * 2)) & 3;
}

static inline void cellSetRaw(int x, int y, uint8_t v) {
  const int i = y * SAND_W + x;
  const int s = (i & 3) * 2;
  g_cells[i >> 2] = (uint8_t)((g_cells[i >> 2] & ~(3 << s)) | (v << s));
}

static uint16_t cellColor(uint8_t v) {
  if (v == CELL_SAND) return C_TND_EMBER;
  if (v == CELL_WALL) return C_TND_INK;
  return C_TND_PAPER;
}

static inline void cellSet(int x, int y, uint8_t v) {
  cellSetRaw(x, y, v);
  if (g_active) {
    gfx->fillRect(x * SAND_CELL_PX, y * SAND_CELL_PX, SAND_CELL_PX, SAND_CELL_PX,
                  cellColor(v));
  }
}

static uint32_t rnd() {
  uint32_t x = g_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_rng = x;
  return x;
}

static bool clockNow(int *hour, int *minute) {
  const unsigned long e = nowEpoch();
  if (e == 0) return false;
  const unsigned long s = (e + TZ_OFFSET) % 86400UL;
  *hour = (int)(s / 3600UL);
  *minute = (int)((s % 3600UL) / 60UL);
  return true;
}

static void setWallRect(int x0, int y0, int w, int h) {
  for (int y = y0; y < y0 + h; y++) {
    for (int x = x0; x < x0 + w; x++) {
      if (cellGet(x, y) == CELL_SAND) g_grains--;
      cellSet(x, y, CELL_WALL);
    }
  }
}

static void placeDigit(int x, uint8_t seg) {
  if (seg & 0x01) setWallRect(x, DIG_TOP, DIG_W, DIG_T);
  if (seg & 0x02) setWallRect(x + DIG_W - DIG_T, DIG_TOP, DIG_T, DIG_H / 2);
  if (seg & 0x04) setWallRect(x + DIG_W - DIG_T, DIG_TOP + DIG_H / 2, DIG_T, DIG_H / 2);
  if (seg & 0x08) setWallRect(x, DIG_TOP + DIG_H - DIG_T, DIG_W, DIG_T);
  if (seg & 0x10) setWallRect(x, DIG_TOP + DIG_H / 2, DIG_T, DIG_H / 2);
  if (seg & 0x20) setWallRect(x, DIG_TOP, DIG_T, DIG_H / 2);
  if (seg & 0x40) setWallRect(x, DIG_TOP + (DIG_H - DIG_T) / 2, DIG_W, DIG_T);
}

static void rebuildObstaclesIfTimeChanged() {
  int h = 0, m = 0;
  const int key = clockNow(&h, &m) ? h * 60 + m : -1;
  if (key == g_obstacleKey) return;
  g_obstacleKey = key;

  for (int y = 0; y < SAND_H; y++) {
    for (int x = 0; x < SAND_W; x++) {
      if (cellGet(x, y) == CELL_WALL) cellSet(x, y, CELL_EMPTY);
    }
  }

  if (key >= 0) {
    placeDigit(DIG_X[0], SEG_FOR_DIGIT[h / 10]);
    placeDigit(DIG_X[1], SEG_FOR_DIGIT[h % 10]);
    placeDigit(DIG_X[2], SEG_FOR_DIGIT[m / 10]);
    placeDigit(DIG_X[3], SEG_FOR_DIGIT[m % 10]);
  } else {
    for (int i = 0; i < 4; i++) placeDigit(DIG_X[i], SEG_DASH);
  }
  setWallRect(COLON_X, DIG_TOP + 7, COLON_SIZE, COLON_SIZE);
  setWallRect(COLON_X, DIG_TOP + DIG_H - 7 - COLON_SIZE, COLON_SIZE, COLON_SIZE);
}

static bool isNight() {
  int h = 0, m = 0;
  if (!clockNow(&h, &m)) return false;
  return h >= NIGHT_START_HOUR || h < NIGHT_END_HOUR;
}

static void spawnAt(int x) {
  if (x < 0 || x >= SAND_W) return;
  if (cellGet(x, 0) == CELL_EMPTY) {
    cellSet(x, 0, CELL_SAND);
    g_grains++;
  }
}

static void simStep() {
  g_frame++;

  // The emitter wanders across the top edge; a pour widens it to three
  // streams and opens the tap.
  g_emitterX += (int)(rnd() % 3) - 1;
  if (g_emitterX < 4) g_emitterX = 4;
  if (g_emitterX > SAND_W - 5) g_emitterX = SAND_W - 5;

  const bool pouring = millis() < g_pourUntilMs;
  const int p256 = pouring ? SPAWN_P256_POUR : (isNight() ? SPAWN_P256_NIGHT : SPAWN_P256_DAY);
  if ((int)(rnd() & 0xFF) < p256) {
    spawnAt(g_emitterX);
    if (pouring) {
      spawnAt(g_emitterX - 4);
      spawnAt(g_emitterX + 4);
    }
  }

  // Gravity pass, bottom row first so each grain moves at most one cell per
  // step; x scan direction alternates to avoid a sideways drift bias.
  for (int y = SAND_H - 2; y >= 0; y--) {
    const bool ltr = ((y + g_frame) & 1) == 0;
    for (int i = 0; i < SAND_W; i++) {
      const int x = ltr ? i : SAND_W - 1 - i;
      if (cellGet(x, y) != CELL_SAND) continue;
      if (cellGet(x, y + 1) == CELL_EMPTY) {
        cellSet(x, y, CELL_EMPTY);
        cellSet(x, y + 1, CELL_SAND);
        continue;
      }
      int dir = (rnd() & 1) ? 1 : -1;
      for (int k = 0; k < 2; k++, dir = -dir) {
        const int nx = x + dir;
        if (nx < 0 || nx >= SAND_W) continue;
        // The side cell must be empty too, so grains cannot tunnel
        // diagonally through a wall corner.
        if (cellGet(nx, y) == CELL_EMPTY && cellGet(nx, y + 1) == CELL_EMPTY) {
          cellSet(x, y, CELL_EMPTY);
          cellSet(nx, y + 1, CELL_SAND);
          break;
        }
      }
    }
  }

  // Over budget, the floor becomes a slow grate.
  if (g_grains > TARGET_GRAINS) {
    for (int t = 0; t < 8; t++) {
      const int x = (int)(rnd() % SAND_W);
      if (cellGet(x, SAND_H - 1) == CELL_SAND) {
        cellSet(x, SAND_H - 1, CELL_EMPTY);
        g_grains--;
      }
    }
  }
}

void sandPour() {
  g_pourUntilMs = millis() + POUR_DURATION_MS;
}

void sandClear() {
  for (int y = 0; y < SAND_H; y++) {
    for (int x = 0; x < SAND_W; x++) {
      if (cellGet(x, y) == CELL_SAND) cellSet(x, y, CELL_EMPTY);
    }
  }
  g_grains = 0;
}

static void drawGrainsCount() {
  gfx->fillRect(140, 226, 86, 8, C_TND_PAPER);
  printRight(226, 226, 1, String(g_grains) + " grains", C_TND_MUTE, C_TND_PAPER);
  g_shownGrains = g_grains;
}

void sandScreenBegin() {
  if (g_rng == 1) {
    g_rng = ESP.getCycleCount() | 1;
  }
  gfx->fillScreen(C_TND_PAPER);
  g_active = true;

  // Repaint the persisted field (the sim state survives screen switches).
  for (int y = 0; y < SAND_H; y++) {
    for (int x = 0; x < SAND_W; x++) {
      const uint8_t v = cellGet(x, y);
      if (v != CELL_EMPTY) {
        gfx->fillRect(x * SAND_CELL_PX, y * SAND_CELL_PX, SAND_CELL_PX, SAND_CELL_PX,
                      cellColor(v));
      }
    }
  }

  gfx->drawFastHLine(14, 220, 212, C_TND_LINE);
  gfx->setTextSize(1);
  gfx->setTextColor(C_TND_MUTE, C_TND_PAPER);
  gfx->setCursor(14, 226);
  gfx->print("sand clock - pour / clear from dashboard");
  g_shownGrains = -1;
  g_lastSimMs = 0;
}

void sandScreenTick() {
  // The sim (and its direct cell painting) only runs while visible, exactly
  // like the ESP32, whose sim only stepped during render.
  g_active = (lcdScreen == SCREEN_SAND);
  if (!g_active) return;

  const unsigned long now = millis();
  if (now - g_lastSimMs < SIM_TICK_MS) return;
  g_lastSimMs = now;

  rebuildObstaclesIfTimeChanged();
  for (int s = 0; s < SIM_STEPS_PER_TICK; s++) simStep();

  if (g_grains != g_shownGrains) drawGrainsCount();
}
