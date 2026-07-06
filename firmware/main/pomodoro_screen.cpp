#include "pomodoro_screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "audio_chime.h"
#include "board_peripherals.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "pomodoro_screen";

typedef enum {
    POMODORO_IDLE = 0,
    POMODORO_RUNNING = 1,
    POMODORO_PAUSED = 2,
    POMODORO_DONE = 3,
} PomodoroState;

typedef struct {
    PomodoroState state;
    int64_t deadline_us;      // valid when RUNNING: esp_timer time the countdown reaches zero
    int paused_remaining_sec; // valid when IDLE/PAUSED
    int64_t alert_started_us; // valid when DONE
    bool chime_silenced;      // true once the DONE-state chime has auto-stopped
} PomodoroCtx;

static PomodoroCtx g_pomodoro;

// Live clock cache for the header, refreshed at this cadence -- same pattern
// as stats_screen, since this screen is also only ever touched by the render
// task and needs no mutex.
static const int64_t CLOCK_REFRESH_INTERVAL_US = 1000000LL;
static int64_t g_last_clock_read_us;
static BoardIdleMetrics g_clock_metrics;

static int total_seconds(void)
{
    return POMODORO_DEFAULT_MINUTES * 60;
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

void pomodoro_screen_init(void)
{
    memset(&g_pomodoro, 0, sizeof(g_pomodoro));
    g_pomodoro.state = POMODORO_IDLE;
    g_pomodoro.paused_remaining_sec = total_seconds();
    g_last_clock_read_us = 0;
    memset(&g_clock_metrics, 0, sizeof(g_clock_metrics));
}

void pomodoro_toggle_start_pause(void)
{
    const int64_t now_us = esp_timer_get_time();
    switch (g_pomodoro.state) {
        case POMODORO_IDLE:
        case POMODORO_PAUSED:
            g_pomodoro.deadline_us = now_us + (int64_t)g_pomodoro.paused_remaining_sec * 1000000LL;
            g_pomodoro.state = POMODORO_RUNNING;
            ESP_LOGI(TAG, "pomodoro started/resumed, %d sec remaining", g_pomodoro.paused_remaining_sec);
            break;
        case POMODORO_RUNNING: {
            const int remaining = (int)((g_pomodoro.deadline_us - now_us) / 1000000LL);
            g_pomodoro.paused_remaining_sec = clamp_int(remaining, 0, total_seconds());
            g_pomodoro.state = POMODORO_PAUSED;
            ESP_LOGI(TAG, "pomodoro paused, %d sec remaining", g_pomodoro.paused_remaining_sec);
            break;
        }
        case POMODORO_DONE:
            // Only reset exits the done state; start/pause presses are ignored here.
            break;
    }
}

void pomodoro_reset(void)
{
    const bool was_alerting = g_pomodoro.state == POMODORO_DONE;
    g_pomodoro.state = POMODORO_IDLE;
    g_pomodoro.paused_remaining_sec = total_seconds();
    g_pomodoro.deadline_us = 0;
    g_pomodoro.alert_started_us = 0;
    g_pomodoro.chime_silenced = false;
    if (was_alerting) {
        audio_chime_stop();
    }
    ESP_LOGI(TAG, "pomodoro reset");
}

void pomodoro_tick(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (g_pomodoro.state == POMODORO_RUNNING) {
        if (now_us >= g_pomodoro.deadline_us) {
            g_pomodoro.state = POMODORO_DONE;
            g_pomodoro.alert_started_us = now_us;
            g_pomodoro.paused_remaining_sec = 0;
            g_pomodoro.chime_silenced = false;
            audio_chime_start();
            ESP_LOGI(TAG, "pomodoro session complete, alert started");
        }
    } else if (g_pomodoro.state == POMODORO_DONE && !g_pomodoro.chime_silenced) {
        const int elapsed_sec = (int)((now_us - g_pomodoro.alert_started_us) / 1000000LL);
        if (elapsed_sec >= POMODORO_ALERT_TIMEOUT_SEC) {
            g_pomodoro.chime_silenced = true;
            audio_chime_stop();
            ESP_LOGI(TAG, "pomodoro alert timed out, chime silenced");
        }
    }
}

static void refresh_clock_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (g_last_clock_read_us != 0 && now_us - g_last_clock_read_us < CLOCK_REFRESH_INTERVAL_US) {
        return;
    }
    g_last_clock_read_us = now_us;
    board_peripherals_read(&g_clock_metrics);
}

static void draw_centered(u8g2_t *u8, const char *text, int y)
{
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int text_width = (int)u8g2_GetUTF8Width(u8, text);
    u8g2_DrawUTF8(u8, (width - text_width) / 2, y, text);
}

// Same inverted-color convention as water_screen/stats_screen: this panel
// renders draw color 1 as light/reflective, 0 as dark ink.
static void begin_light_theme(u8g2_t *u8)
{
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int height = (int)u8g2_GetDisplayHeight(u8);
    u8g2_SetDrawColor(u8, 1);
    u8g2_DrawBox(u8, 0, 0, width, height);
    u8g2_SetDrawColor(u8, 0);
    u8g2_SetFontMode(u8, 1);
}

static void draw_card(u8g2_t *u8, int x, int y, int w, int h)
{
    u8g2_DrawRFrame(u8, x, y, w, h, 8);
}

static void draw_ring_track(u8g2_t *u8, int cx, int cy, int r_outer, int thickness)
{
    u8g2_DrawCircle(u8, cx, cy, r_outer, U8G2_DRAW_ALL);
    u8g2_DrawCircle(u8, cx, cy, r_outer - thickness, U8G2_DRAW_ALL);
}

// Fills a clockwise pie-ring from 12 o'clock, sweeping `fraction` (0..1) of
// the full circle -- identical to water_screen's ring helper.
static void draw_ring_progress(u8g2_t *u8, int cx, int cy, int r_outer, int thickness, float fraction)
{
    if (fraction <= 0.0f) {
        return;
    }
    if (fraction > 1.0f) {
        fraction = 1.0f;
    }
    const int r_inner = r_outer - thickness;
    const float sweep_deg = 360.0f * fraction;
    const int steps = (int)(sweep_deg * 2.0f) + 1;
    for (int i = 0; i <= steps; i++) {
        const float t = steps == 0 ? 0.0f : (float)i / (float)steps;
        const float angle_rad = (-90.0f + sweep_deg * t) * (float)M_PI / 180.0f;
        const float dx = cosf(angle_rad);
        const float dy = sinf(angle_rad);
        for (int r = r_inner; r <= r_outer; r++) {
            u8g2_DrawPixel(u8, cx + (int)(dx * (float)r), cy + (int)(dy * (float)r));
        }
    }
}

static const char *state_prompt(PomodoroState state)
{
    switch (state) {
        case POMODORO_RUNNING:
            return "SESSION RUNNING";
        case POMODORO_PAUSED:
            return "PAUSED - TAP TO RESUME";
        default:
            return "TAP BUTTON TO START";
    }
}

static void render_countdown(u8g2_t *u8, PomodoroState state, int remaining_sec)
{
    begin_light_theme(u8);
    const int width = (int)u8g2_GetDisplayWidth(u8);

    char clock[8];
    if (g_clock_metrics.time_valid) {
        snprintf(clock, sizeof(clock), "%02d:%02d", g_clock_metrics.hour, g_clock_metrics.minute);
    } else {
        snprintf(clock, sizeof(clock), "--:--");
    }

    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    u8g2_DrawUTF8(u8, 14, 19, "POMODORO");
    const int clock_width = (int)u8g2_GetUTF8Width(u8, clock);
    u8g2_DrawUTF8(u8, width - 14 - clock_width, 19, clock);
    u8g2_DrawHLine(u8, 12, 27, width - 24);

    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered(u8, state_prompt(state), 45);

    const int cx = width / 2;
    const int cy = 148;
    const int r_outer = 86;
    const int thickness = 10;
    const int total_sec = total_seconds();
    const int elapsed_sec = clamp_int(total_sec - remaining_sec, 0, total_sec);
    const float fraction = total_sec > 0 ? (float)elapsed_sec / (float)total_sec : 0.0f;
    draw_ring_track(u8, cx, cy, r_outer, thickness);
    draw_ring_progress(u8, cx, cy, r_outer, thickness, fraction);

    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered(u8, "TIME LEFT", 138);
    char remaining[16];
    snprintf(remaining, sizeof(remaining), "%02d:%02d", remaining_sec / 60, remaining_sec % 60);
    u8g2_SetFont(u8, u8g2_font_helvB24_tf);
    draw_centered(u8, remaining, 176);

    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered(u8, "TAP: START/PAUSE   HOLD: RESET", 260);

    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    draw_centered(u8, "BOOT: MODE  |  g4pys.company", 294);
}

static void render_done(u8g2_t *u8, int alert_remaining_sec)
{
    begin_light_theme(u8);
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int height = (int)u8g2_GetDisplayHeight(u8);

    const int card_x = 40;
    const int card_y = 30;
    const int card_w = width - 80;
    const int card_h = height - 60;
    draw_card(u8, card_x, card_y, card_w, card_h);

    u8g2_SetFont(u8, u8g2_font_helvB24_tf);
    draw_centered(u8, "TIME'S UP", 150);

    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    draw_centered(u8, "HOLD: RESET", 180);

    const int bar_w = width - 120;
    const int bar_x = (width - bar_w) / 2;
    const int bar_y = 200;
    const int bar_h = 14;
    u8g2_DrawFrame(u8, bar_x, bar_y, bar_w, bar_h);
    const int filled = (bar_w - 2) * alert_remaining_sec / POMODORO_ALERT_TIMEOUT_SEC;
    if (filled > 0) {
        u8g2_DrawBox(u8, bar_x + 1, bar_y + 1, filled, bar_h - 2);
    }

    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered(u8, "BOOT: MODE  |  g4pys.company", 256);
}

void pomodoro_render_current(u8g2_t *u8)
{
    refresh_clock_if_due();

    const int64_t now_us = esp_timer_get_time();
    if (g_pomodoro.state == POMODORO_DONE) {
        const int elapsed_sec = (int)((now_us - g_pomodoro.alert_started_us) / 1000000LL);
        const int alert_remaining_sec = clamp_int(POMODORO_ALERT_TIMEOUT_SEC - elapsed_sec, 0, POMODORO_ALERT_TIMEOUT_SEC);
        render_done(u8, alert_remaining_sec);
        return;
    }

    int remaining_sec;
    if (g_pomodoro.state == POMODORO_RUNNING) {
        remaining_sec = clamp_int((int)((g_pomodoro.deadline_us - now_us) / 1000000LL), 0, total_seconds());
    } else {
        remaining_sec = g_pomodoro.paused_remaining_sec;
    }
    render_countdown(u8, g_pomodoro.state, remaining_sec);
}
