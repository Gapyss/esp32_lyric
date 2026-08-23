#include "clock_screen.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board_peripherals.h"
#include "esp_timer.h"

// Live tile values (temp/humidity/battery) refresh at this cadence; the trend
// history itself accrues independently inside board_peripherals so it keeps
// filling even while another screen is on top. Same contract as stats_screen.
static const int64_t LIVE_REFRESH_INTERVAL_US = 1000000LL;

// System clock is only meaningful once SNTP has set it (mirrors the guard in
// board_peripherals read_system_time). Anything below this is an unset clock.
static const time_t CLOCK_VALID_EPOCH = 1700000000;

// Year grid: 20 columns x 19 rows = 380 cells, enough for a 366-day leap year.
static const int GRID_COLS = 20;
static const int GRID_ROWS = 19;
static const int GRID_SQUARE = 5;  // px side of each day square

static int64_t g_last_read_us;
static BoardIdleMetrics g_metrics;

void clock_screen_init(void)
{
    g_last_read_us = 0;
    memset(&g_metrics, 0, sizeof(g_metrics));
}

static void refresh_if_due(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (g_last_read_us != 0 && now_us - g_last_read_us < LIVE_REFRESH_INTERVAL_US) {
        return;
    }
    g_last_read_us = now_us;
    board_peripherals_read(&g_metrics);
}

// This panel's memory-in-pixel controller renders draw color 1 as the light/
// reflective (paper) state and 0 as dark ink -- see the matching note in
// stats_screen.cpp. Every frame starts by filling the card
// light so it reads correctly regardless of what the previous screen left.
// This is Tend's paper surface; draw color 0 is Tend ink.
static void begin_light_theme(u8g2_t *u8)
{
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int height = (int)u8g2_GetDisplayHeight(u8);
    u8g2_SetDrawColor(u8, 1);
    u8g2_DrawBox(u8, 0, 0, width, height);
    u8g2_SetDrawColor(u8, 0);
    u8g2_SetFontMode(u8, 1);
}

static void draw_centered_in(u8g2_t *u8, const char *text, int x, int y, int w, int h)
{
    const int text_width = (int)u8g2_GetUTF8Width(u8, text);
    const int baseline = y + h / 2 + 3;
    u8g2_DrawUTF8(u8, x + (w - text_width) / 2, baseline, text);
}

// Copied from stats_screen.cpp (per-screen duplication is the convention in
// this firmware -- water/stats each carry their own begin_light_theme/card
// helpers). Draws a labeled card containing a line-graph trend of `values`
// (oldest first, `count` entries) with the current reading called out.
static void draw_trend_card(u8g2_t *u8,
                            int x,
                            int y,
                            int w,
                            int h,
                            const char *label,
                            const char *current_text,
                            const int *values,
                            int count)
{
    u8g2_DrawRFrame(u8, x, y, w, h, 8);

    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    u8g2_DrawUTF8(u8, x + 10, y + 13, label);
    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    const int value_width = (int)u8g2_GetUTF8Width(u8, current_text);
    u8g2_DrawUTF8(u8, x + w - 10 - value_width, y + 17, current_text);
    u8g2_DrawHLine(u8, x + 8, y + 22, w - 16);

    const int plot_x = x + 8;
    const int plot_y = y + 28;
    const int plot_w = w - 16;
    const int plot_h = h - 28 - 8;

    if (count < 2) {
        u8g2_SetFont(u8, u8g2_font_5x7_tf);
        draw_centered_in(u8, "COLLECTING TREND...", plot_x, plot_y, plot_w, plot_h);
        return;
    }

    int min_v = values[0];
    int max_v = values[0];
    for (int i = 1; i < count; i++) {
        if (values[i] < min_v) {
            min_v = values[i];
        }
        if (values[i] > max_v) {
            max_v = values[i];
        }
    }
    if (min_v == max_v) {
        max_v = min_v + 1;
    }

    int prev_px = 0;
    int prev_py = 0;
    for (int i = 0; i < count; i++) {
        const int px = plot_x + (plot_w - 1) * i / (count - 1);
        const int py = plot_y + (plot_h - 1) - (plot_h - 1) * (values[i] - min_v) / (max_v - min_v);
        if (i > 0) {
            u8g2_DrawLine(u8, prev_px, prev_py, px, py);
        }
        prev_px = px;
        prev_py = py;
    }
}

// One square per day of the year inside the given rectangle. Elapsed days are
// filled (Tend ink), today is filled with a halo ring (the ember-accent day),
// remaining days are hollow outlines. `day_of_year` is 1-based; `total` is 365
// or 366. When day_of_year <= 0 the whole grid renders hollow (clock not set).
static void draw_year_grid(u8g2_t *u8, int gx, int gy, int gw, int gh, int day_of_year, int total)
{
    const float pitch_x = (float)gw / (float)GRID_COLS;
    const float pitch_y = (float)gh / (float)GRID_ROWS;
    for (int i = 1; i <= total; i++) {
        const int idx = i - 1;
        const int col = idx % GRID_COLS;
        const int row = idx / GRID_COLS;
        const int cx = gx + (int)(col * pitch_x + (pitch_x - GRID_SQUARE) / 2.0f);
        const int cy = gy + (int)(row * pitch_y + (pitch_y - GRID_SQUARE) / 2.0f);
        if (day_of_year > 0 && i < day_of_year) {
            u8g2_DrawBox(u8, cx, cy, GRID_SQUARE, GRID_SQUARE);
        } else if (i == day_of_year) {
            u8g2_DrawBox(u8, cx, cy, GRID_SQUARE, GRID_SQUARE);
            u8g2_DrawFrame(u8, cx - 2, cy - 2, GRID_SQUARE + 4, GRID_SQUARE + 4);
        } else {
            u8g2_DrawFrame(u8, cx, cy, GRID_SQUARE, GRID_SQUARE);
        }
    }
}

// One clock hand from the center at `deg` clockwise from 12 o'clock. `thick`
// fattens it with two extra parallel strokes (u8g2 lines are 1px wide) so the
// hour hand reads heavier than the minute/second hands.
static void draw_clock_hand(u8g2_t *u8, int cx, int cy, float deg, float len, bool thick)
{
    const float a = deg * (float)M_PI / 180.0f;
    const int ex = cx + (int)(len * sinf(a));
    const int ey = cy - (int)(len * cosf(a));
    u8g2_DrawLine(u8, cx, cy, ex, ey);
    if (thick) {
        u8g2_DrawLine(u8, cx + 1, cy, ex, ey);
        u8g2_DrawLine(u8, cx, cy + 1, ex, ey);
    }
}

// A small analog watch face: ring, 12 tick marks (longer at the quarters),
// hour/minute/second hands, and a center hub. Calm and minimal, in keeping
// with the Tend look. Hands are distinguished by length (hour shortest) since
// stroke width barely differs at this size.
static void draw_analog_clock(u8g2_t *u8, int cx, int cy, int r, int hh, int mm, int ss)
{
    u8g2_DrawCircle(u8, cx, cy, r, U8G2_DRAW_ALL);
    for (int t = 0; t < 12; t++) {
        const float a = t * 30.0f * (float)M_PI / 180.0f;
        const int inner = (t % 3 == 0) ? r - 3 : r - 2;
        u8g2_DrawLine(u8,
                      cx + (int)(inner * sinf(a)),
                      cy - (int)(inner * cosf(a)),
                      cx + (int)((r - 1) * sinf(a)),
                      cy - (int)((r - 1) * cosf(a)));
    }
    draw_clock_hand(u8, cx, cy, (hh % 12) * 30.0f + mm * 0.5f, r * 0.50f, true);
    draw_clock_hand(u8, cx, cy, mm * 6.0f + ss * 0.1f, r * 0.78f, false);
    draw_clock_hand(u8, cx, cy, ss * 6.0f, r * 0.85f, false);
    u8g2_DrawDisc(u8, cx, cy, 2, U8G2_DRAW_ALL);
}

void clock_render_current(u8g2_t *u8)
{
    refresh_if_due();

    BoardHistory history;
    board_peripherals_get_history(&history);

    begin_light_theme(u8);
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int height = (int)u8g2_GetDisplayHeight(u8);
    const int m = 14;

    // --- resolve today's date from the SNTP-set system clock ---
    time_t now = 0;
    time(&now);
    bool date_valid = now >= CLOCK_VALID_EPOCH;
    struct tm lt = {};
    int day_of_year = 0;
    int total = 365;
    int days_left = 0;
    int pct = 0;
    if (date_valid) {
        if (localtime_r(&now, &lt) != NULL) {
            const int year = lt.tm_year + 1900;
            const bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
            total = leap ? 366 : 365;
            day_of_year = lt.tm_yday + 1;  // tm_yday is 0-based
            days_left = total - day_of_year;
            pct = day_of_year * 100 / total;
        } else {
            date_valid = false;
        }
    }

    // --- header: eyebrow + big day number, ember chip with days-left ---
    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    u8g2_DrawUTF8(u8, m, 20, "DAY OF YEAR");

    char big[16];
    snprintf(big, sizeof(big), "%s", date_valid ? "" : "--");
    if (date_valid) {
        snprintf(big, sizeof(big), "%d", day_of_year);
    }
    u8g2_SetFont(u8, u8g2_font_helvB24_tf);
    u8g2_DrawUTF8(u8, m, 50, big);
    const int big_w = (int)u8g2_GetUTF8Width(u8, big);

    char total_text[12];
    snprintf(total_text, sizeof(total_text), "/ %d", total);
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    u8g2_DrawUTF8(u8, m + big_w + 8, 48, total_text);

    if (date_valid) {
        // Center top: a small analog watch face over the calendar date. The
        // second hand ticks because the render loop redraws ~14x/sec.
        draw_analog_clock(u8, width / 2, 26, 20, lt.tm_hour, lt.tm_min, lt.tm_sec);

        char date_text[16];
        strftime(date_text, sizeof(date_text), "%a %d %b", &lt);  // e.g. "Tue 13 Jul"
        for (char *p = date_text; *p != '\0'; ++p) {
            *p = (char)toupper((unsigned char)*p);
        }
        u8g2_SetFont(u8, u8g2_font_5x7_tf);
        const int date_w = (int)u8g2_GetUTF8Width(u8, date_text);
        u8g2_DrawUTF8(u8, (width - date_w) / 2, 56, date_text);
    }

    if (date_valid) {
        // Ember accent: on a 1-bit paper panel the "one loud color" becomes an
        // inverted ink chip with paper-colored text.
        char chip[28];
        snprintf(chip, sizeof(chip), "%d DAYS LEFT", days_left);
        u8g2_SetFont(u8, u8g2_font_6x12_tf);
        const int chip_tw = (int)u8g2_GetUTF8Width(u8, chip);
        const int chip_w = chip_tw + 16;
        const int chip_h = 18;
        const int chip_x = width - m - chip_w;
        const int chip_y = 12;
        u8g2_DrawRBox(u8, chip_x, chip_y, chip_w, chip_h, 8);
        u8g2_SetDrawColor(u8, 1);
        u8g2_DrawUTF8(u8, chip_x + 8, chip_y + 13, chip);
        u8g2_SetDrawColor(u8, 0);

        char sub[24];
        snprintf(sub, sizeof(sub), "%d%% ELAPSED", pct);
        u8g2_SetFont(u8, u8g2_font_5x7_tf);
        const int sub_w = (int)u8g2_GetUTF8Width(u8, sub);
        u8g2_DrawUTF8(u8, width - m - sub_w, chip_y + chip_h + 12, sub);
    }

    u8g2_DrawHLine(u8, m, 58, width - 2 * m);

    // --- body: year grid card (left) + two trend cards (right) ---
    const int top = 66;
    const int left_w = 190;
    const int left_h = height - m - top;
    u8g2_DrawRFrame(u8, m, top, left_w, left_h, 8);

    if (date_valid) {
        const int pad = 12;
        draw_year_grid(u8, m + pad, top + pad, left_w - 2 * pad, left_h - 2 * pad, day_of_year, total);
    } else {
        u8g2_SetFont(u8, u8g2_font_5x7_tf);
        draw_centered_in(u8, "WAITING FOR TIME", m, top, left_w, left_h);
    }

    const int rx = m + left_w + 10;
    const int rw = width - m - rx;
    const int gap = 6;
    const int ch = (left_h - gap) / 2;

    char temp_text[16];
    if (g_metrics.env_valid) {
        snprintf(temp_text,
                 sizeof(temp_text),
                 "%d.%d C",
                 g_metrics.temperature_c_x10 / 10,
                 g_metrics.temperature_c_x10 < 0 ? -(g_metrics.temperature_c_x10 % 10)
                                                 : g_metrics.temperature_c_x10 % 10);
    } else {
        snprintf(temp_text, sizeof(temp_text), "-- C");
    }
    char humidity_text[16];
    if (g_metrics.env_valid) {
        snprintf(humidity_text, sizeof(humidity_text), "%d %%RH", g_metrics.humidity_x10 / 10);
    } else {
        snprintf(humidity_text, sizeof(humidity_text), "-- %%RH");
    }

    draw_trend_card(u8, rx, top, rw, ch, "TEMPERATURE", temp_text, history.temperature_c_x10, history.count);
    draw_trend_card(u8, rx, top + ch + gap, rw, ch, "HUMIDITY", humidity_text, history.humidity_x10, history.count);
}
