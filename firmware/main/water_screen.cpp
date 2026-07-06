#include "water_screen.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "audio_chime.h"
#include "board_peripherals.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "water_screen";

typedef enum {
    WATER_IDLE = 0,
    WATER_ALERTING = 1,
} WaterState;

typedef struct {
    WaterState state;
    int64_t last_reminder_us;
    int64_t alert_started_us;
    int interval_min;
    int active_start_min;
    int active_end_min;
    bool clock_valid;
    int hour;
    int minute;
    int64_t last_clock_read_us;
    int drinks_today;
    long drinks_day_marker;
} WaterCtx;

static WaterCtx g_water;
static SemaphoreHandle_t g_water_mutex;

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

static bool active_window_allows(const WaterCtx *ctx)
{
    if (!ctx->clock_valid) {
        return true;
    }
    const int now_min = ctx->hour * 60 + ctx->minute;
    if (ctx->active_start_min == ctx->active_end_min) {
        return false;
    }
    if (ctx->active_start_min < ctx->active_end_min) {
        return now_min >= ctx->active_start_min && now_min < ctx->active_end_min;
    }
    return now_min >= ctx->active_start_min || now_min < ctx->active_end_min;
}

static void load_config_locked(void)
{
    g_water.interval_min = HYDRATE_DEFAULT_INTERVAL_MIN;
    g_water.active_start_min = HYDRATE_DEFAULT_ACTIVE_START_MIN;
    g_water.active_end_min = HYDRATE_DEFAULT_ACTIVE_END_MIN;

    nvs_handle_t handle;
    if (nvs_open(HYDRATE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    int32_t value = 0;
    if (nvs_get_i32(handle, HYDRATE_NVS_INTERVAL_KEY, &value) == ESP_OK) {
        g_water.interval_min = clamp_int(value, 1, 24 * 60);
    }
    if (nvs_get_i32(handle, HYDRATE_NVS_START_KEY, &value) == ESP_OK) {
        g_water.active_start_min = clamp_int(value, 0, 24 * 60 - 1);
    }
    if (nvs_get_i32(handle, HYDRATE_NVS_END_KEY, &value) == ESP_OK) {
        g_water.active_end_min = clamp_int(value, 0, 24 * 60 - 1);
    }
    nvs_close(handle);
}

static esp_err_t save_config_locked(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(HYDRATE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_i32(handle, HYDRATE_NVS_INTERVAL_KEY, g_water.interval_min);
    err |= nvs_set_i32(handle, HYDRATE_NVS_START_KEY, g_water.active_start_min);
    err |= nvs_set_i32(handle, HYDRATE_NVS_END_KEY, g_water.active_end_min);
    err |= nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void update_clock_locked(void)
{
    const int64_t now_us = esp_timer_get_time();
    if (g_water.last_clock_read_us > 0 && now_us - g_water.last_clock_read_us < 1000000LL) {
        return;
    }
    g_water.last_clock_read_us = now_us;

    BoardIdleMetrics metrics = {};
    board_peripherals_read(&metrics);
    g_water.clock_valid = metrics.time_valid;
    if (metrics.time_valid) {
        g_water.hour = metrics.hour;
        g_water.minute = metrics.minute;
    }
}

// Drink counts reset when the wall-clock calendar day rolls over. The RTC
// chip itself only stores hour/minute (see board_peripherals.cpp), so the
// day boundary is derived from the system clock (set once via SNTP and
// free-running from there); marker 0 means "day unknown yet".
static long current_day_marker(void)
{
    time_t now = 0;
    time(&now);
    if (now < 1700000000) {
        return 0;
    }
    struct tm local = {};
    if (localtime_r(&now, &local) == NULL) {
        return 0;
    }
    return (long)(local.tm_year * 10000 + local.tm_mon * 100 + local.tm_mday);
}

static void update_day_rollover_locked(void)
{
    const long marker = current_day_marker();
    if (marker == 0 || marker == g_water.drinks_day_marker) {
        return;
    }
    g_water.drinks_day_marker = marker;
    g_water.drinks_today = 0;
}

static int next_in_sec_locked(int64_t now_us)
{
    const int interval_sec = g_water.interval_min * 60;
    const int elapsed_sec = (int)((now_us - g_water.last_reminder_us) / 1000000LL);
    return clamp_int(interval_sec - elapsed_sec, 0, interval_sec);
}

static void enter_alert_locked(int64_t now_us)
{
    if (g_water.state == WATER_ALERTING) {
        return;
    }
    g_water.state = WATER_ALERTING;
    g_water.alert_started_us = now_us;
    audio_chime_start();
    ESP_LOGI(TAG, "hydration reminder started");
}

void water_screen_init(void)
{
    memset(&g_water, 0, sizeof(g_water));
    g_water.state = WATER_IDLE;
    g_water.last_reminder_us = esp_timer_get_time();
    if (g_water_mutex == NULL) {
        g_water_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(g_water_mutex, portMAX_DELAY);
    load_config_locked();
    xSemaphoreGive(g_water_mutex);
}

void water_tick(void)
{
    if (g_water_mutex == NULL) {
        water_screen_init();
    }
    const int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(g_water_mutex, portMAX_DELAY);
    update_clock_locked();
    update_day_rollover_locked();
    if (g_water.state == WATER_ALERTING) {
        const int elapsed_sec = (int)((now_us - g_water.alert_started_us) / 1000000LL);
        if (elapsed_sec >= HYDRATE_ALERT_TIMEOUT_SEC) {
            g_water.state = WATER_IDLE;
            g_water.last_reminder_us = now_us;
            g_water.alert_started_us = 0;
            audio_chime_stop();
            ESP_LOGI(TAG, "hydration reminder timed out");
        }
    } else if (next_in_sec_locked(now_us) == 0 && active_window_allows(&g_water)) {
        enter_alert_locked(now_us);
    }
    xSemaphoreGive(g_water_mutex);
}

void water_fire_now(void)
{
    const int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(g_water_mutex, portMAX_DELAY);
    enter_alert_locked(now_us);
    xSemaphoreGive(g_water_mutex);
}

void water_log_drink(void)
{
    const int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(g_water_mutex, portMAX_DELAY);
    update_day_rollover_locked();
    g_water.drinks_today++;
    g_water.state = WATER_IDLE;
    g_water.alert_started_us = 0;
    g_water.last_reminder_us = now_us;
    const int drinks_today = g_water.drinks_today;
    xSemaphoreGive(g_water_mutex);
    audio_chime_stop();
    ESP_LOGI(TAG, "drink logged via button, total today=%d", drinks_today);
}

void water_snooze(int minutes)
{
    minutes = clamp_int(minutes, 1, 24 * 60);
    xSemaphoreTake(g_water_mutex, portMAX_DELAY);
    g_water.state = WATER_IDLE;
    g_water.alert_started_us = 0;
    g_water.last_reminder_us = esp_timer_get_time() - ((int64_t)(g_water.interval_min - minutes) * 60LL * 1000000LL);
    audio_chime_stop();
    xSemaphoreGive(g_water_mutex);
}

esp_err_t water_configure(int interval_min, int active_start_min, int active_end_min)
{
    if (interval_min <= 0 || active_start_min < 0 || active_start_min >= 24 * 60 ||
        active_end_min < 0 || active_end_min >= 24 * 60) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(g_water_mutex, portMAX_DELAY);
    g_water.interval_min = clamp_int(interval_min, 1, 24 * 60);
    g_water.active_start_min = active_start_min;
    g_water.active_end_min = active_end_min;
    g_water.last_reminder_us = esp_timer_get_time();
    const esp_err_t err = save_config_locked();
    xSemaphoreGive(g_water_mutex);
    return err;
}

void water_get_snapshot(WaterSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    const int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(g_water_mutex, portMAX_DELAY);
    update_day_rollover_locked();
    snapshot->alerting = g_water.state == WATER_ALERTING;
    snapshot->drinks_today = g_water.drinks_today;
    snapshot->clock_valid = g_water.clock_valid;
    snapshot->hour = g_water.hour;
    snapshot->minute = g_water.minute;
    snapshot->next_in_sec = next_in_sec_locked(now_us);
    snapshot->interval_min = g_water.interval_min;
    snapshot->active_start_min = g_water.active_start_min;
    snapshot->active_end_min = g_water.active_end_min;
    if (g_water.state == WATER_ALERTING) {
        const int elapsed_sec = (int)((now_us - g_water.alert_started_us) / 1000000LL);
        snapshot->alert_remaining_sec = clamp_int(HYDRATE_ALERT_TIMEOUT_SEC - elapsed_sec, 0, HYDRATE_ALERT_TIMEOUT_SEC);
    }
    xSemaphoreGive(g_water_mutex);
    snapshot->audio_available = audio_chime_available();
    snapshot->audio_playing = audio_chime_is_playing();
}

static void draw_centered(u8g2_t *u8, const char *text, int y)
{
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int text_width = (int)u8g2_GetUTF8Width(u8, text);
    u8g2_DrawUTF8(u8, (width - text_width) / 2, y, text);
}

// This display's memory-in-pixel panel renders draw color 1 as the light/
// reflective state and 0 as dark ink (opposite of the usual OLED sense) --
// confirmed on hardware for the idle music screen. Every water screen state
// fills the frame with 1 first so it always reads as a light card, regardless
// of whatever draw color the previous frame (e.g. the music screen) left set.
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
// the full circle, by scanning angle steps and painting each radius between
// the inner and outer ring bounds.
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

static void draw_water_drop(u8g2_t *u8, int cx, int cy, int r)
{
    const int disc_cy = cy + r / 3;
    u8g2_DrawDisc(u8, cx, disc_cy, r, U8G2_DRAW_ALL);
    u8g2_DrawTriangle(u8, cx - r, disc_cy, cx + r, disc_cy, cx, cy - r);
}

static void render_idle(u8g2_t *u8, const WaterSnapshot *snapshot)
{
    begin_light_theme(u8);
    const int width = (int)u8g2_GetDisplayWidth(u8);

    char clock[8];
    if (snapshot->clock_valid) {
        snprintf(clock, sizeof(clock), "%02d:%02d", snapshot->hour, snapshot->minute);
    } else {
        snprintf(clock, sizeof(clock), "--:--");
    }

    // Header card: title left, clock right, rule beneath -- same convention
    // as the music screen's idle header.
    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    u8g2_DrawUTF8(u8, 14, 19, "HYDRATION");
    const int clock_width = (int)u8g2_GetUTF8Width(u8, clock);
    u8g2_DrawUTF8(u8, width - 14 - clock_width, 19, clock);
    u8g2_DrawHLine(u8, 12, 27, width - 24);

    // Sits in the open band between the header rule and the ring's top edge
    // (ring top = cy - r_outer = 148 - 86 = 62) so it can't collide with the
    // ring arc drawn below.
    char drinks[24];
    snprintf(drinks, sizeof(drinks), "%d DRINK%s LOGGED TODAY", snapshot->drinks_today, snapshot->drinks_today == 1 ? "" : "S");
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered(u8, drinks, 45);

    // Ring: fills clockwise as time elapses toward the next reminder.
    const int cx = width / 2;
    const int cy = 148;
    const int r_outer = 86;
    const int thickness = 10;
    const int interval_sec = snapshot->interval_min * 60;
    const int elapsed_sec = interval_sec > 0 ? clamp_int(interval_sec - snapshot->next_in_sec, 0, interval_sec) : 0;
    const float fraction = interval_sec > 0 ? (float)elapsed_sec / (float)interval_sec : 0.0f;
    draw_ring_track(u8, cx, cy, r_outer, thickness);
    draw_ring_progress(u8, cx, cy, r_outer, thickness, fraction);

    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered(u8, "NEXT DRINK IN", 138);
    char next[16];
    snprintf(next, sizeof(next), "%02d:%02d", snapshot->next_in_sec / 60, snapshot->next_in_sec % 60);
    u8g2_SetFont(u8, u8g2_font_helvB24_tf);
    draw_centered(u8, next, 176);

    // Stat cards: active window and reminder interval, side by side.
    const int card_y = 240;
    const int card_h = 36;
    const int card_w = 175;
    const int left_x = 20;
    const int right_x = width - 20 - card_w;
    draw_card(u8, left_x, card_y, card_w, card_h);
    draw_card(u8, right_x, card_y, card_w, card_h);

    char active[32];
    snprintf(active,
             sizeof(active),
             "%02d:%02d-%02d:%02d",
             snapshot->active_start_min / 60,
             snapshot->active_start_min % 60,
             snapshot->active_end_min / 60,
             snapshot->active_end_min % 60);
    char interval[16];
    snprintf(interval, sizeof(interval), "%d MIN", snapshot->interval_min);

    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    u8g2_DrawUTF8(u8, left_x + 10, card_y + 13, "ACTIVE");
    u8g2_DrawUTF8(u8, right_x + 10, card_y + 13, "INTERVAL");
    u8g2_SetFont(u8, u8g2_font_helvB10_tf);
    u8g2_DrawUTF8(u8, left_x + 10, card_y + 30, active);
    u8g2_DrawUTF8(u8, right_x + 10, card_y + 30, interval);

    u8g2_SetFont(u8, u8g2_font_5x7_tf);
    draw_centered(u8, "BOOT: MODE  |  g4pys.company", 294);
}

static void render_alert(u8g2_t *u8, const WaterSnapshot *snapshot)
{
    begin_light_theme(u8);
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int height = (int)u8g2_GetDisplayHeight(u8);

    const int card_x = 40;
    const int card_y = 30;
    const int card_w = width - 80;
    const int card_h = height - 60;
    draw_card(u8, card_x, card_y, card_w, card_h);

    draw_water_drop(u8, width / 2, 96, 30);

    u8g2_SetFont(u8, u8g2_font_helvB24_tf);
    draw_centered(u8, "DRINK WATER", 178);

    u8g2_SetFont(u8, u8g2_font_helvB14_tf);
    draw_centered(u8, "PRESS BUTTON TO LOG", 206);

    const int bar_w = width - 120;
    const int bar_x = (width - bar_w) / 2;
    const int bar_y = 222;
    const int bar_h = 14;
    u8g2_DrawFrame(u8, bar_x, bar_y, bar_w, bar_h);
    const int filled = (bar_w - 2) * snapshot->alert_remaining_sec / HYDRATE_ALERT_TIMEOUT_SEC;
    if (filled > 0) {
        u8g2_DrawBox(u8, bar_x + 1, bar_y + 1, filled, bar_h - 2);
    }

    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    draw_centered(u8, "BOOT: MODE  |  g4pys.company", 256);
}

void water_render_current(u8g2_t *u8)
{
    WaterSnapshot snapshot;
    water_get_snapshot(&snapshot);
    if (snapshot.alerting) {
        render_alert(u8, &snapshot);
    } else {
        render_idle(u8, &snapshot);
    }
}
