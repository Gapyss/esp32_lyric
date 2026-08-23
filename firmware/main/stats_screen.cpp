#include "stats_screen.h"

#include <stdio.h>
#include <string.h>

#include "board_peripherals.h"
#include "esp_timer.h"

// Live tile values (temp/humidity/battery/clock) refresh at this cadence;
// the trend history itself accrues independently inside board_peripherals
// (see board_peripherals.cpp) so it keeps filling even while another screen
// is on top.
static const int64_t LIVE_REFRESH_INTERVAL_US = 1000000LL;

static int64_t g_last_read_us;
static BoardIdleMetrics g_metrics;

void stats_screen_init(void)
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

// This panel's memory-in-pixel controller renders draw color 1 as the
// light/reflective state and 0 as dark ink. Every stats frame starts by
// filling the card light so it reads correctly regardless of whatever the
// previous screen left behind.
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

static void draw_centered_in(u8g2_t *u8, const char *text, int x, int y, int w, int h)
{
    const int text_width = (int)u8g2_GetUTF8Width(u8, text);
    const int baseline = y + h / 2 + 3;
    u8g2_DrawUTF8(u8, x + (w - text_width) / 2, baseline, text);
}

// Draws a labeled card containing a line-graph trend of `values` (oldest
// first, `count` entries) with the current reading called out top-right.
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
    draw_card(u8, x, y, w, h);

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

// Header battery pill. `x` is the left edge of the body; the terminal nub sits
// to its right, so the whole glyph occupies BATTERY_GLYPH_WIDTH.
#define BATTERY_GLYPH_WIDTH 29

static void draw_battery_glyph(u8g2_t *u8, int x, int y, int percent, bool valid)
{
    const int w = 26;
    const int h = 13;
    u8g2_DrawFrame(u8, x, y, w, h);
    u8g2_DrawBox(u8, x + w, y + 4, 3, 5);

    if (!valid) {
        u8g2_DrawLine(u8, x + 3, y + h - 3, x + w - 4, y + 2);
        return;
    }
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    const int fill_w = (w - 4) * percent / 100;
    if (fill_w > 0) {
        u8g2_DrawBox(u8, x + 2, y + 2, fill_w, h - 4);
    }
}

// Charging bolt, drawn beside the pill rather than inside it -- a filled pill
// leaves no contrast for an inset glyph on this 1-bit panel.
static void draw_charge_bolt(u8g2_t *u8, int x, int y)
{
    u8g2_DrawTriangle(u8, x + 6, y, x, y + 8, x + 5, y + 8);
    u8g2_DrawTriangle(u8, x + 1, y + 13, x + 7, y + 5, x + 2, y + 5);
}

static void draw_stat_card(u8g2_t *u8, int x, int y, int w, int h, const char *label, const char *value)
{
    draw_card(u8, x, y, w, h);
    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    u8g2_DrawUTF8(u8, x + 10, y + 13, label);
    u8g2_SetFont(u8, u8g2_font_helvB10_tf);
    u8g2_DrawUTF8(u8, x + 10, y + 30, value);
}

void stats_render_current(u8g2_t *u8)
{
    refresh_if_due();

    BoardHistory history;
    board_peripherals_get_history(&history);

    begin_light_theme(u8);
    const int width = (int)u8g2_GetDisplayWidth(u8);

    char clock[8];
    if (g_metrics.time_valid) {
        snprintf(clock, sizeof(clock), "%02d:%02d", g_metrics.hour, g_metrics.minute);
    } else {
        snprintf(clock, sizeof(clock), "--:--");
    }

    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    u8g2_DrawUTF8(u8, 14, 19, "BOARD STATS");

    // Right side of the header, laid out from the edge inwards: battery pill,
    // its percentage, a charge bolt when plugged in, then the clock.
    int cursor = width - 14;
    const int glyph_x = cursor - BATTERY_GLYPH_WIDTH;
    draw_battery_glyph(u8, glyph_x, 5, g_metrics.battery_percent, g_metrics.battery_valid);
    cursor = glyph_x - 7;

    char percent_text[8];
    if (g_metrics.battery_valid) {
        snprintf(percent_text, sizeof(percent_text), "%d%%", g_metrics.battery_percent);
    } else {
        snprintf(percent_text, sizeof(percent_text), "--%%");
    }
    u8g2_SetFont(u8, u8g2_font_helvB10_tf);
    const int percent_width = (int)u8g2_GetUTF8Width(u8, percent_text);
    u8g2_DrawUTF8(u8, cursor - percent_width, 18, percent_text);
    cursor -= percent_width + 6;

    if (g_metrics.battery_valid && g_metrics.battery_trend == BOARD_BATTERY_TREND_CHARGING) {
        draw_charge_bolt(u8, cursor - 8, 5);
        cursor -= 14;
    }

    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    const int clock_width = (int)u8g2_GetUTF8Width(u8, clock);
    u8g2_DrawUTF8(u8, cursor - 14 - clock_width, 19, clock);
    u8g2_DrawHLine(u8, 12, 27, width - 24);

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

    draw_trend_card(u8, 20, 34, width - 40, 96, "TEMPERATURE", temp_text, history.temperature_c_x10, history.count);
    draw_trend_card(u8, 20, 138, width - 40, 96, "HUMIDITY", humidity_text, history.humidity_x10, history.count);

    const int card_y = 240;
    const int card_h = 36;
    const int card_w = 175;
    const int left_x = 20;
    const int right_x = width - 20 - card_w;

    // The card carries the runtime estimate; the voltage rides along in the
    // label, where it stays useful for sanity-checking the charge curve.
    char battery_label[24];
    if (g_metrics.battery_valid) {
        snprintf(battery_label,
                 sizeof(battery_label),
                 "BATTERY %d.%02dV",
                 g_metrics.battery_mv / 1000,
                 (g_metrics.battery_mv / 10) % 100);
    } else {
        snprintf(battery_label, sizeof(battery_label), "BATTERY");
    }

    char battery_text[24];
    if (!g_metrics.battery_valid) {
        snprintf(battery_text, sizeof(battery_text), "N/A");
    } else if (g_metrics.battery_trend == BOARD_BATTERY_TREND_CHARGING) {
        snprintf(battery_text, sizeof(battery_text), "CHARGING");
    } else if (g_metrics.battery_trend == BOARD_BATTERY_TREND_DISCHARGING) {
        const int hours = g_metrics.battery_runtime_minutes / 60;
        const int minutes = g_metrics.battery_runtime_minutes % 60;
        if (g_metrics.battery_runtime_minutes >= BOARD_BATTERY_RUNTIME_CAP_MINUTES) {
            snprintf(battery_text, sizeof(battery_text), ">%dH LEFT", BOARD_BATTERY_RUNTIME_CAP_MINUTES / 60);
        } else if (hours > 0) {
            snprintf(battery_text, sizeof(battery_text), "%dH %02dM LEFT", hours, minutes);
        } else {
            snprintf(battery_text, sizeof(battery_text), "%dM LEFT", minutes);
        }
    } else if (g_metrics.battery_trend == BOARD_BATTERY_TREND_STEADY) {
        snprintf(battery_text, sizeof(battery_text), "STEADY");
    } else {
        snprintf(battery_text, sizeof(battery_text), "ESTIMATING...");
    }

    char history_text[16];
    snprintf(history_text, sizeof(history_text), "%d MIN", history.count);

    draw_stat_card(u8, left_x, card_y, card_w, card_h, battery_label, battery_text);
    draw_stat_card(u8, right_x, card_y, card_w, card_h, "TREND SPAN", history_text);

    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    const char *footer = "BOOT: MODE  |  g4pys.company";
    const int footer_width = (int)u8g2_GetUTF8Width(u8, footer);
    u8g2_DrawUTF8(u8, (width - footer_width) / 2, 294, footer);
}
