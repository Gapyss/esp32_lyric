#include "sand_screen.h"

#include <stdio.h>
#include <string.h>

#include "board_peripherals.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "sand_screen";

// The simulation runs on a grid of 2x2-pixel cells covering the area above
// the footer: 400x272 pixels -> 200x136 cells.
#define SAND_CELL_PX 2
#define SAND_GRID_W 200
#define SAND_GRID_H 136

enum { CELL_EMPTY = 0, CELL_SAND = 1, CELL_WALL = 2 };

// Render loop ticks at ~14 fps; three sim steps per frame gives a fall speed
// of about 85 px/s, which reads as a gentle sift on the reflective panel.
static const int SIM_STEPS_PER_FRAME = 3;

// Steady-state grain budget. Dunes build freely until the field holds this
// many grains; past it the floor starts letting grains slip away, so the
// scene reaches equilibrium instead of silting up.
static const int TARGET_GRAINS = 5200;

// Spawn probability per sim step, in 1/256 units. Night keeps the scene
// alive but calm.
static const int SPAWN_P256_DAY = 20;
static const int SPAWN_P256_NIGHT = 5;
static const int SPAWN_P256_POUR = 230;
static const int NIGHT_START_HOUR = 22;
static const int NIGHT_END_HOUR = 7;

static const int64_t POUR_DURATION_US = 1200000LL;

// Live sensor values refresh at this cadence, same contract as clock_screen.
static const int64_t ENV_REFRESH_INTERVAL_US = 1000000LL;

// 7-segment digit geometry in cells. The digits are solid walls the sand
// piles onto; when the displayed minute changes the walls are rebuilt and
// whatever was resting on them falls.
static const int DIG_W = 24;
static const int DIG_H = 42;
static const int DIG_T = 5;
static const int DIG_TOP = 30;
static const int DIG_X[4] = {34, 66, 110, 142};
static const int COLON_X = 97;
static const int COLON_SIZE = 6;

// Segment bits: A=0x01 top, B=0x02 top-right, C=0x04 bottom-right,
// D=0x08 bottom, E=0x10 bottom-left, F=0x20 top-left, G=0x40 middle.
static const uint8_t SEG_FOR_DIGIT[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};
static const uint8_t SEG_DASH = 0x40;

typedef struct {
    uint8_t cells[SAND_GRID_H][SAND_GRID_W];
    uint32_t rng;
    uint32_t frame;
    int grains;
    int emitter_x;
    int64_t pour_until_us;
    int64_t last_env_read_us;
    BoardIdleMetrics metrics;
    int obstacle_key;  // hour*60+minute of the walls on screen; -1 = clock not valid, -2 = not built yet
} SandCtx;

static SandCtx g_sand;

static uint32_t rnd(void)
{
    uint32_t x = g_sand.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_sand.rng = x;
    return x;
}

static void refresh_env_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (g_sand.last_env_read_us != 0 && now_us - g_sand.last_env_read_us < ENV_REFRESH_INTERVAL_US) {
        return;
    }
    g_sand.last_env_read_us = now_us;
    board_peripherals_read(&g_sand.metrics);
}

static void set_wall_rect(int x0, int y0, int w, int h)
{
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            if (g_sand.cells[y][x] == CELL_SAND) {
                g_sand.grains--;
            }
            g_sand.cells[y][x] = CELL_WALL;
        }
    }
}

static void place_digit(int x, uint8_t seg)
{
    if (seg & 0x01) set_wall_rect(x, DIG_TOP, DIG_W, DIG_T);
    if (seg & 0x02) set_wall_rect(x + DIG_W - DIG_T, DIG_TOP, DIG_T, DIG_H / 2);
    if (seg & 0x04) set_wall_rect(x + DIG_W - DIG_T, DIG_TOP + DIG_H / 2, DIG_T, DIG_H / 2);
    if (seg & 0x08) set_wall_rect(x, DIG_TOP + DIG_H - DIG_T, DIG_W, DIG_T);
    if (seg & 0x10) set_wall_rect(x, DIG_TOP + DIG_H / 2, DIG_T, DIG_H / 2);
    if (seg & 0x20) set_wall_rect(x, DIG_TOP, DIG_T, DIG_H / 2);
    if (seg & 0x40) set_wall_rect(x, DIG_TOP + (DIG_H - DIG_T) / 2, DIG_W, DIG_T);
}

static void rebuild_obstacles_if_time_changed(void)
{
    const int key = g_sand.metrics.time_valid
                        ? g_sand.metrics.hour * 60 + g_sand.metrics.minute
                        : -1;
    if (key == g_sand.obstacle_key) {
        return;
    }
    g_sand.obstacle_key = key;

    for (int y = 0; y < SAND_GRID_H; y++) {
        for (int x = 0; x < SAND_GRID_W; x++) {
            if (g_sand.cells[y][x] == CELL_WALL) {
                g_sand.cells[y][x] = CELL_EMPTY;
            }
        }
    }

    if (key >= 0) {
        const int h = key / 60;
        const int m = key % 60;
        place_digit(DIG_X[0], SEG_FOR_DIGIT[h / 10]);
        place_digit(DIG_X[1], SEG_FOR_DIGIT[h % 10]);
        place_digit(DIG_X[2], SEG_FOR_DIGIT[m / 10]);
        place_digit(DIG_X[3], SEG_FOR_DIGIT[m % 10]);
    } else {
        for (int i = 0; i < 4; i++) {
            place_digit(DIG_X[i], SEG_DASH);
        }
    }
    set_wall_rect(COLON_X, DIG_TOP + 10, COLON_SIZE, COLON_SIZE);
    set_wall_rect(COLON_X, DIG_TOP + DIG_H - 10 - COLON_SIZE, COLON_SIZE, COLON_SIZE);
}

static bool is_night(void)
{
    return g_sand.metrics.time_valid &&
           (g_sand.metrics.hour >= NIGHT_START_HOUR || g_sand.metrics.hour < NIGHT_END_HOUR);
}

static void spawn_at(int x)
{
    if (x < 0 || x >= SAND_GRID_W) {
        return;
    }
    if (g_sand.cells[0][x] == CELL_EMPTY) {
        g_sand.cells[0][x] = CELL_SAND;
        g_sand.grains++;
    }
}

static void sim_step(void)
{
    g_sand.frame++;

    // The emitter wanders across the top edge; a pour widens it to three
    // streams and opens the tap.
    g_sand.emitter_x += (int)(rnd() % 3) - 1;
    if (g_sand.emitter_x < 4) g_sand.emitter_x = 4;
    if (g_sand.emitter_x > SAND_GRID_W - 5) g_sand.emitter_x = SAND_GRID_W - 5;

    const bool pouring = esp_timer_get_time() < g_sand.pour_until_us;
    const int p256 = pouring ? SPAWN_P256_POUR : (is_night() ? SPAWN_P256_NIGHT : SPAWN_P256_DAY);
    if ((int)(rnd() & 0xFF) < p256) {
        spawn_at(g_sand.emitter_x);
        if (pouring) {
            spawn_at(g_sand.emitter_x - 6);
            spawn_at(g_sand.emitter_x + 6);
        }
    }

    // Gravity pass, bottom row first so each grain moves at most one cell per
    // step; x scan direction alternates to avoid a sideways drift bias.
    for (int y = SAND_GRID_H - 2; y >= 0; y--) {
        const bool ltr = ((y + g_sand.frame) & 1) == 0;
        for (int i = 0; i < SAND_GRID_W; i++) {
            const int x = ltr ? i : SAND_GRID_W - 1 - i;
            if (g_sand.cells[y][x] != CELL_SAND) {
                continue;
            }
            if (g_sand.cells[y + 1][x] == CELL_EMPTY) {
                g_sand.cells[y][x] = CELL_EMPTY;
                g_sand.cells[y + 1][x] = CELL_SAND;
                continue;
            }
            int dir = (rnd() & 1) ? 1 : -1;
            for (int k = 0; k < 2; k++, dir = -dir) {
                const int nx = x + dir;
                if (nx < 0 || nx >= SAND_GRID_W) {
                    continue;
                }
                // The side cell must be empty too, so grains cannot tunnel
                // diagonally through a wall corner.
                if (g_sand.cells[y][nx] == CELL_EMPTY && g_sand.cells[y + 1][nx] == CELL_EMPTY) {
                    g_sand.cells[y][x] = CELL_EMPTY;
                    g_sand.cells[y + 1][nx] = CELL_SAND;
                    break;
                }
            }
        }
    }

    // Over budget, the floor becomes a slow grate.
    if (g_sand.grains > TARGET_GRAINS) {
        for (int t = 0; t < 8; t++) {
            const int x = (int)(rnd() % SAND_GRID_W);
            if (g_sand.cells[SAND_GRID_H - 1][x] == CELL_SAND) {
                g_sand.cells[SAND_GRID_H - 1][x] = CELL_EMPTY;
                g_sand.grains--;
            }
        }
    }
}

void sand_screen_init(void)
{
    memset(&g_sand, 0, sizeof(g_sand));
    g_sand.rng = esp_random();
    if (g_sand.rng == 0) {
        g_sand.rng = 1;
    }
    g_sand.emitter_x = SAND_GRID_W / 2;
    g_sand.obstacle_key = -2;
}

void sand_pour(void)
{
    g_sand.pour_until_us = esp_timer_get_time() + POUR_DURATION_US;
    ESP_LOGI(TAG, "pour, %d grains in the field", g_sand.grains);
}

void sand_clear(void)
{
    for (int y = 0; y < SAND_GRID_H; y++) {
        for (int x = 0; x < SAND_GRID_W; x++) {
            if (g_sand.cells[y][x] == CELL_SAND) {
                g_sand.cells[y][x] = CELL_EMPTY;
            }
        }
    }
    g_sand.grains = 0;
    ESP_LOGI(TAG, "field cleared");
}

// Same panel-polarity note as the other screens: draw color 1 is Tend paper
// (light/reflective), 0 is Tend ink. Per-screen duplication of this helper is
// the convention in this firmware.
static void begin_light_theme(u8g2_t *u8)
{
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int height = (int)u8g2_GetDisplayHeight(u8);
    u8g2_SetDrawColor(u8, 1);
    u8g2_DrawBox(u8, 0, 0, width, height);
    u8g2_SetDrawColor(u8, 0);
    u8g2_SetFontMode(u8, 1);
}

void sand_render_current(u8g2_t *u8)
{
    refresh_env_if_due();
    rebuild_obstacles_if_time_changed();
    for (int s = 0; s < SIM_STEPS_PER_FRAME; s++) {
        sim_step();
    }

    begin_light_theme(u8);
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int m = 14;

    // Sand and walls are both ink; horizontal runs of occupied cells merge
    // into single boxes to keep the draw-call count down.
    for (int y = 0; y < SAND_GRID_H; y++) {
        int run_start = -1;
        for (int x = 0; x <= SAND_GRID_W; x++) {
            const bool filled = x < SAND_GRID_W && g_sand.cells[y][x] != CELL_EMPTY;
            if (filled && run_start < 0) {
                run_start = x;
            } else if (!filled && run_start >= 0) {
                u8g2_DrawBox(u8,
                             run_start * SAND_CELL_PX,
                             y * SAND_CELL_PX,
                             (x - run_start) * SAND_CELL_PX,
                             SAND_CELL_PX);
                run_start = -1;
            }
        }
    }

    u8g2_DrawHLine(u8, m, SAND_GRID_H * SAND_CELL_PX + 6, width - 2 * m);

    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    char left[32];
    snprintf(left, sizeof(left), "SAND CLOCK - %d GRAINS", g_sand.grains);
    u8g2_DrawUTF8(u8, m, 294, left);
    const char *right = "BTN: POUR | HOLD: CLEAR | BOOT: MODE";
    const int right_w = (int)u8g2_GetUTF8Width(u8, right);
    u8g2_DrawUTF8(u8, width - m - right_w, 294, right);
}
