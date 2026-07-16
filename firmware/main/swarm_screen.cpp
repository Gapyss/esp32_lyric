#include "swarm_screen.h"

#include <stdio.h>
#include <string.h>

#include "board_peripherals.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

static const char *TAG = "swarm_screen";

// The swarm lives in the area above the footer, same field as sand_screen:
// 400x272 pixels. Positions and velocities are 24.8 fixed point.
#define FIELD_W 400
#define FIELD_H 272
#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)

#define NUM_FLIES 300
#define MAX_TARGETS 400

// Two sim substeps per ~14 fps render tick keeps the motion fluid without
// the flies covering more than a few pixels per drawn frame.
static const int SIM_STEPS_PER_FRAME = 2;

// Spring-and-damper steering toward the assigned target point. k = 1/32 per
// step with 0.9 damping settles in roughly two seconds of gentle overshoot,
// which reads as the swarm "landing" rather than snapping into place.
static const int ATTRACT_SHIFT = 5;
static const int DAMP_NUM = 230;  // of 256

// Per-step random nudge in fixed-point velocity units; night calms it down.
static const int JITTER_DAY = 40;
static const int JITTER_NIGHT = 14;
static const int NIGHT_START_HOUR = 22;
static const int NIGHT_END_HOUR = 7;

// Speed caps in fixed-point px/step.
static const int32_t MAX_SPEED = 3 * FP_ONE;
static const int32_t MAX_SPEED_SCATTER = 7 * FP_ONE;

static const int64_t SCATTER_DURATION_US = 1600000LL;
static const int64_t ENV_REFRESH_INTERVAL_US = 1000000LL;

// 7-segment digit geometry in pixels. Targets are sampled along segment
// centerlines every SAMPLE_SPACING px; with these sizes a worst-case 88:88
// needs just under NUM_FLIES targets, so the digits stay dense.
static const int DIG_W = 56;
static const int DIG_H = 112;
static const int DIG_TOP = 80;
static const int DIG_X[4] = {54, 128, 216, 290};
static const int COLON_CX = 200;
static const int SAMPLE_SPACING = 6;

// Segment bits: A=0x01 top, B=0x02 top-right, C=0x04 bottom-right,
// D=0x08 bottom, E=0x10 bottom-left, F=0x20 top-left, G=0x40 middle.
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

typedef struct {
    Fly flies[NUM_FLIES];
    uint16_t tx[MAX_TARGETS];
    uint16_t ty[MAX_TARGETS];
    int num_targets;
    uint32_t rng;
    uint32_t frame;
    uint32_t assign_salt;  // rotated on each rebuild so flies swap perches
    bool roam;
    int64_t scatter_until_us;
    int64_t last_env_read_us;
    BoardIdleMetrics metrics;
    int target_key;  // hour*60+minute of the targets; -1 = clock not valid, -2 = not built yet
} SwarmCtx;

static SwarmCtx g_swarm;

static uint32_t rnd(void)
{
    uint32_t x = g_swarm.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_swarm.rng = x;
    return x;
}

static void refresh_env_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (g_swarm.last_env_read_us != 0 && now_us - g_swarm.last_env_read_us < ENV_REFRESH_INTERVAL_US) {
        return;
    }
    g_swarm.last_env_read_us = now_us;
    board_peripherals_read(&g_swarm.metrics);
}

static void add_target(int x, int y)
{
    if (g_swarm.num_targets >= MAX_TARGETS) {
        return;
    }
    g_swarm.tx[g_swarm.num_targets] = (uint16_t)x;
    g_swarm.ty[g_swarm.num_targets] = (uint16_t)y;
    g_swarm.num_targets++;
}

static void sample_line(int x0, int y0, int x1, int y1)
{
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int len = (dx > -dx ? dx : -dx) + (dy > -dy ? dy : -dy);
    int n = len / SAMPLE_SPACING;
    if (n < 1) {
        n = 1;
    }
    for (int i = 0; i <= n; i++) {
        add_target(x0 + dx * i / n, y0 + dy * i / n);
    }
}

static void place_digit(int x, uint8_t seg)
{
    const int y = DIG_TOP;
    const int hh = DIG_H / 2;
    if (seg & 0x01) sample_line(x, y, x + DIG_W, y);
    if (seg & 0x02) sample_line(x + DIG_W, y, x + DIG_W, y + hh);
    if (seg & 0x04) sample_line(x + DIG_W, y + hh, x + DIG_W, y + DIG_H);
    if (seg & 0x08) sample_line(x, y + DIG_H, x + DIG_W, y + DIG_H);
    if (seg & 0x10) sample_line(x, y + hh, x, y + DIG_H);
    if (seg & 0x20) sample_line(x, y, x, y + hh);
    if (seg & 0x40) sample_line(x, y + hh, x + DIG_W, y + hh);
}

static void place_colon_dot(int cy)
{
    add_target(COLON_CX - 3, cy - 3);
    add_target(COLON_CX + 3, cy - 3);
    add_target(COLON_CX - 3, cy + 3);
    add_target(COLON_CX + 3, cy + 3);
}

static void rebuild_targets_if_time_changed(void)
{
    const int key = g_swarm.metrics.time_valid
                        ? g_swarm.metrics.hour * 60 + g_swarm.metrics.minute
                        : -1;
    if (key == g_swarm.target_key) {
        return;
    }
    g_swarm.target_key = key;
    g_swarm.num_targets = 0;

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
    place_colon_dot(DIG_TOP + DIG_H / 2 - 24);
    place_colon_dot(DIG_TOP + DIG_H / 2 + 24);

    g_swarm.assign_salt = rnd();
}

static bool is_night(void)
{
    return g_swarm.metrics.time_valid &&
           (g_swarm.metrics.hour >= NIGHT_START_HOUR || g_swarm.metrics.hour < NIGHT_END_HOUR);
}

// Which target fly i perches on: an even spread over the target list, with
// the fly order rotated by the per-minute salt so a rebuild sends flies
// criss-crossing to new perches instead of sliding in formation.
static int target_index_for(int i)
{
    const int j = (int)((i + g_swarm.assign_salt) % NUM_FLIES);
    return j * g_swarm.num_targets / NUM_FLIES;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void sim_step(void)
{
    g_swarm.frame++;
    const bool scattering = esp_timer_get_time() < g_swarm.scatter_until_us;
    const bool homing = !scattering && !g_swarm.roam && g_swarm.num_targets > 0;
    const int jitter = is_night() ? JITTER_NIGHT : JITTER_DAY;
    const int32_t max_speed = scattering ? MAX_SPEED_SCATTER : MAX_SPEED;

    for (int i = 0; i < NUM_FLIES; i++) {
        Fly *f = &g_swarm.flies[i];

        if (homing) {
            const int t = target_index_for(i);
            const int32_t tx = ((int32_t)g_swarm.tx[t] + f->ox) << FP_SHIFT;
            const int32_t ty = ((int32_t)g_swarm.ty[t] + f->oy) << FP_SHIFT;
            f->vx += (tx - f->px) >> ATTRACT_SHIFT;
            f->vy += (ty - f->py) >> ATTRACT_SHIFT;
        }

        f->vx += (int32_t)(rnd() % (2 * jitter + 1)) - jitter;
        f->vy += (int32_t)(rnd() % (2 * jitter + 1)) - jitter;
        f->vx = f->vx * DAMP_NUM / 256;
        f->vy = f->vy * DAMP_NUM / 256;
        f->vx = clamp32(f->vx, -max_speed, max_speed);
        f->vy = clamp32(f->vy, -max_speed, max_speed);
        f->px += f->vx;
        f->py += f->vy;

        // Bounce off the field edges; roaming and scattering flies hit these
        // constantly, homing flies almost never do.
        if (f->px < 0) {
            f->px = -f->px;
            f->vx = -f->vx;
        } else if (f->px > (FIELD_W - 2) * FP_ONE) {
            f->px = 2 * (FIELD_W - 2) * FP_ONE - f->px;
            f->vx = -f->vx;
        }
        if (f->py < 0) {
            f->py = -f->py;
            f->vy = -f->vy;
        } else if (f->py > (FIELD_H - 2) * FP_ONE) {
            f->py = 2 * (FIELD_H - 2) * FP_ONE - f->py;
            f->vy = -f->vy;
        }
    }
}

void swarm_screen_init(void)
{
    memset(&g_swarm, 0, sizeof(g_swarm));
    g_swarm.rng = esp_random();
    if (g_swarm.rng == 0) {
        g_swarm.rng = 1;
    }
    g_swarm.target_key = -2;
    for (int i = 0; i < NUM_FLIES; i++) {
        Fly *f = &g_swarm.flies[i];
        f->px = (int32_t)(rnd() % FIELD_W) << FP_SHIFT;
        f->py = (int32_t)(rnd() % FIELD_H) << FP_SHIFT;
        f->ox = (int8_t)(rnd() % 5) - 2;
        f->oy = (int8_t)(rnd() % 5) - 2;
        f->phase = (uint8_t)rnd();
    }
}

void swarm_scatter(void)
{
    g_swarm.scatter_until_us = esp_timer_get_time() + SCATTER_DURATION_US;
    for (int i = 0; i < NUM_FLIES; i++) {
        Fly *f = &g_swarm.flies[i];
        f->vx += (int32_t)(rnd() % (2 * MAX_SPEED_SCATTER + 1)) - MAX_SPEED_SCATTER;
        f->vy += (int32_t)(rnd() % (2 * MAX_SPEED_SCATTER + 1)) - MAX_SPEED_SCATTER;
    }
    ESP_LOGI(TAG, "swarm scattered");
}

void swarm_toggle_roam(void)
{
    g_swarm.roam = !g_swarm.roam;
    ESP_LOGI(TAG, "roam %s", g_swarm.roam ? "on" : "off");
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

void swarm_render_current(u8g2_t *u8)
{
    refresh_env_if_due();
    rebuild_targets_if_time_changed();
    for (int s = 0; s < SIM_STEPS_PER_FRAME; s++) {
        sim_step();
    }

    begin_light_theme(u8);
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int m = 14;

    for (int i = 0; i < NUM_FLIES; i++) {
        const Fly *f = &g_swarm.flies[i];
        const uint32_t tick = g_swarm.frame + f->phase;
        // Firefly blink: every so often a fly winks out for a frame or
        // flares to 3x3 for one.
        if (tick % 43 == 0) {
            continue;
        }
        const int x = (int)(f->px >> FP_SHIFT);
        const int y = (int)(f->py >> FP_SHIFT);
        if (tick % 37 < 2) {
            u8g2_DrawBox(u8, x > 0 ? x - 1 : 0, y > 0 ? y - 1 : 0, 3, 3);
        } else {
            u8g2_DrawBox(u8, x, y, 2, 2);
        }
    }

    u8g2_DrawHLine(u8, m, FIELD_H + 6, width - 2 * m);

    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    char left[32];
    snprintf(left, sizeof(left), "FIREFLY CLOCK - %s", g_swarm.roam ? "ROAM" : "TIME");
    u8g2_DrawUTF8(u8, m, 294, left);
    const char *right = "BTN: SCATTER | HOLD: ROAM | BOOT: MODE";
    const int right_w = (int)u8g2_GetUTF8Width(u8, right);
    u8g2_DrawUTF8(u8, width - m - right_w, 294, right);
}
