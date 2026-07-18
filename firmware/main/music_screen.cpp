#include "music_screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "display_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define FONT_LARGE u8g2_font_helvB24_tf
#define FONT_MEDIUM u8g2_font_helvB18_tf
#define FONT_SMALL u8g2_font_6x12_tf

static const int TRANSITION_MS = 280;

typedef struct {
    char title[128];
    char artist[128];
    char lyric[192];
    char lyric2[192];
    char lyric3[192];
    MusicBitmapSlot slots[MUSIC_BITMAP_SLOT_COUNT];
    uint32_t generation[MUSIC_BITMAP_SLOT_COUNT];
    MusicBitmapSlot transition_lyric;
    char transition_lyric_text[192];
    int64_t transition_start_us;
    int pos;
    int dur;
    int paused;
    int lyric_at;
    int lyric2_at;
    int64_t pos_base_us;
    uint8_t *full_frame;
    uint8_t *pending_full_frame;
    int64_t pending_swap_us;
    int idle_hour;
    int idle_minute;
    int idle_temperature_c_x10;
    int idle_humidity_x10;
    bool idle_time_valid;
    bool idle_env_valid;
    char board_ip[24];
    char daemon_status[32];
    bool daemon_connected;
    int64_t daemon_wait_start_us;
} NowPlaying;

static NowPlaying g_now;
static SemaphoreHandle_t g_now_mutex;

static void clear_slot(MusicBitmapSlot *slot)
{
    if (slot == NULL) {
        return;
    }
    slot->width = 0;
    slot->height = 0;
    slot->data_len = 0;
    slot->data = NULL;
}

static void free_slot(MusicBitmapSlot *slot)
{
    if (slot == NULL) {
        return;
    }
    free(slot->data);
    clear_slot(slot);
}

static void move_slot(MusicBitmapSlot *dst, MusicBitmapSlot *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    free_slot(dst);
    *dst = *src;
    clear_slot(src);
}

static bool slot_has_bitmap(const MusicBitmapSlot *slot)
{
    return slot != NULL && slot->data != NULL && slot->width > 0 && slot->height > 0 && slot->data_len > 0;
}

static void copy_text(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }
    snprintf(dst, dst_len, "%s", src == NULL ? "" : src);
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

static int elapsed_for(const NowPlaying *state, int64_t now_us)
{
    int elapsed = state->pos;
    if (!state->paused && state->pos_base_us > 0) {
        elapsed += (int)((now_us - state->pos_base_us) / 1000000LL);
    }
    if (state->dur > 0) {
        return clamp_int(elapsed, 0, state->dur);
    }
    return elapsed < 0 ? 0 : elapsed;
}

static bool slot_or_text_present(const MusicBitmapSlot *slot, const char *text)
{
    return slot_has_bitmap(slot) || (text != NULL && text[0] != '\0');
}

static void finish_transition(NowPlaying *state)
{
    free_slot(&state->transition_lyric);
    state->transition_lyric_text[0] = '\0';
    state->transition_start_us = 0;
}

static void clear_full_frames_locked(NowPlaying *state)
{
    free(state->full_frame);
    free(state->pending_full_frame);
    state->full_frame = NULL;
    state->pending_full_frame = NULL;
    state->pending_swap_us = 0;
}

static void promote_full_frame_if_due(NowPlaying *state, int64_t now_us)
{
    if (state->pending_full_frame == NULL || state->pending_swap_us <= 0 || now_us < state->pending_swap_us) {
        return;
    }
    free(state->full_frame);
    state->full_frame = state->pending_full_frame;
    state->pending_full_frame = NULL;
    state->pending_swap_us = 0;
}

static void promote_if_due(NowPlaying *state, int elapsed, int64_t now_us, bool allow_promote)
{
    if (!allow_promote || state->lyric_at < 0 || elapsed < state->lyric_at) {
        return;
    }
    if (!slot_or_text_present(&state->slots[MUSIC_SLOT_LYRIC2], state->lyric2)) {
        return;
    }

    finish_transition(state);
    move_slot(&state->transition_lyric, &state->slots[MUSIC_SLOT_LYRIC]);
    copy_text(state->transition_lyric_text, sizeof(state->transition_lyric_text), state->lyric);
    state->transition_start_us = now_us;

    move_slot(&state->slots[MUSIC_SLOT_LYRIC], &state->slots[MUSIC_SLOT_LYRIC2]);
    move_slot(&state->slots[MUSIC_SLOT_LYRIC2], &state->slots[MUSIC_SLOT_LYRIC3]);
    clear_slot(&state->slots[MUSIC_SLOT_LYRIC3]);

    copy_text(state->lyric, sizeof(state->lyric), state->lyric2);
    copy_text(state->lyric2, sizeof(state->lyric2), state->lyric3);
    state->lyric3[0] = '\0';

    state->generation[MUSIC_SLOT_LYRIC]++;
    state->generation[MUSIC_SLOT_LYRIC2]++;
    state->generation[MUSIC_SLOT_LYRIC3]++;
    state->lyric_at = state->lyric2_at;
    state->lyric2_at = -1;
}

void music_screen_init(void)
{
    memset(&g_now, 0, sizeof(g_now));
    g_now.lyric_at = -1;
    g_now.lyric2_at = -1;
    copy_text(g_now.board_ip, sizeof(g_now.board_ip), "acquiring");
    copy_text(g_now.daemon_status, sizeof(g_now.daemon_status), "DISCOVERING");
    g_now.daemon_connected = false;
    g_now.daemon_wait_start_us = esp_timer_get_time();
    if (g_now_mutex == NULL) {
        g_now_mutex = xSemaphoreCreateMutex();
    }
}

void music_set_from_args(const char *title,
                         const char *artist,
                         const char *lyric,
                         const char *lyric2,
                         int pos,
                         int dur,
                         int paused,
                         int lyric_at)
{
    if (g_now_mutex == NULL) {
        music_screen_init();
    }

    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    copy_text(g_now.title, sizeof(g_now.title), title);
    copy_text(g_now.artist, sizeof(g_now.artist), artist);
    copy_text(g_now.lyric, sizeof(g_now.lyric), lyric);
    copy_text(g_now.lyric2, sizeof(g_now.lyric2), lyric2);
    g_now.lyric3[0] = '\0';
    for (int i = 0; i < MUSIC_BITMAP_SLOT_COUNT; i++) {
        free_slot(&g_now.slots[i]);
        g_now.generation[i]++;
    }
    finish_transition(&g_now);
    clear_full_frames_locked(&g_now);
    g_now.pos = pos < 0 ? 0 : pos;
    g_now.dur = dur < 0 ? 0 : dur;
    g_now.paused = paused ? 1 : 0;
    g_now.lyric_at = lyric_at;
    g_now.lyric2_at = -1;
    g_now.pos_base_us = esp_timer_get_time();
    xSemaphoreGive(g_now_mutex);
}

void music_set_from_bitmaps(const MusicBitmapSlot slots[MUSIC_BITMAP_SLOT_COUNT],
                            int pos,
                            int dur,
                            int paused,
                            int lyric_at,
                            int lyric2_at)
{
    if (g_now_mutex == NULL) {
        music_screen_init();
    }

    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    for (int i = 0; i < MUSIC_BITMAP_SLOT_COUNT; i++) {
        free_slot(&g_now.slots[i]);
        g_now.slots[i] = slots[i];
        g_now.generation[i]++;
    }
    g_now.title[0] = '\0';
    g_now.artist[0] = '\0';
    g_now.lyric[0] = '\0';
    g_now.lyric2[0] = '\0';
    g_now.lyric3[0] = '\0';
    finish_transition(&g_now);
    clear_full_frames_locked(&g_now);
    g_now.pos = pos < 0 ? 0 : pos;
    g_now.dur = dur < 0 ? 0 : dur;
    g_now.paused = paused ? 1 : 0;
    g_now.lyric_at = lyric_at;
    g_now.lyric2_at = lyric2_at;
    g_now.pos_base_us = esp_timer_get_time();
    xSemaphoreGive(g_now_mutex);
}

bool music_set_full_frame(const uint8_t *frame, uint32_t data_len, int swap_in_ms)
{
    if (frame == NULL || data_len != MUSIC_DISPLAY_BYTES) {
        return false;
    }
    if (g_now_mutex == NULL) {
        music_screen_init();
    }

    uint8_t *copy = (uint8_t *)malloc(MUSIC_DISPLAY_BYTES);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, frame, MUSIC_DISPLAY_BYTES);

    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    if (swap_in_ms > 0) {
        free(g_now.pending_full_frame);
        g_now.pending_full_frame = copy;
        g_now.pending_swap_us = esp_timer_get_time() + ((int64_t)swap_in_ms * 1000LL);
    } else {
        free(g_now.full_frame);
        free(g_now.pending_full_frame);
        g_now.full_frame = copy;
        g_now.pending_full_frame = NULL;
        g_now.pending_swap_us = 0;
    }
    xSemaphoreGive(g_now_mutex);
    return true;
}

static bool patch_frame_rect(uint8_t **frame_ptr,
                             const uint8_t *data,
                             uint32_t data_len,
                             int x,
                             int y,
                             int width,
                             int height,
                             int row_bytes)
{
    if (data == NULL || x < 0 || y < 0 || width <= 0 || height <= 0 || row_bytes <= 0 || (x % 8) != 0) {
        return false;
    }
    if (x + width > (int)MUSIC_DISPLAY_WIDTH || y + height > (int)MUSIC_DISPLAY_HEIGHT) {
        return false;
    }
    if (data_len != (uint32_t)(row_bytes * height)) {
        return false;
    }
    if (*frame_ptr == NULL) {
        *frame_ptr = (uint8_t *)calloc(MUSIC_DISPLAY_BYTES, 1);
        if (*frame_ptr == NULL) {
            return false;
        }
    }

    const int dst_row_bytes = MUSIC_DISPLAY_ROW_BYTES;
    const int dst_x_byte = x / 8;
    for (int row = 0; row < height; row++) {
        uint8_t *dst = *frame_ptr + ((y + row) * dst_row_bytes) + dst_x_byte;
        const uint8_t *src = data + (row * row_bytes);
        memcpy(dst, src, (size_t)row_bytes);
    }
    return true;
}

bool music_set_frame_rect(const uint8_t *data,
                          uint32_t data_len,
                          int x,
                          int y,
                          int width,
                          int height,
                          int row_bytes,
                          int swap_in_ms)
{
    if (g_now_mutex == NULL) {
        music_screen_init();
    }

    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    bool ok = false;
    if (swap_in_ms > 0) {
        if (g_now.pending_full_frame == NULL) {
            g_now.pending_full_frame = (uint8_t *)malloc(MUSIC_DISPLAY_BYTES);
            if (g_now.pending_full_frame != NULL) {
                if (g_now.full_frame != NULL) {
                    memcpy(g_now.pending_full_frame, g_now.full_frame, MUSIC_DISPLAY_BYTES);
                } else {
                    memset(g_now.pending_full_frame, 0, MUSIC_DISPLAY_BYTES);
                }
            }
        }
        ok = patch_frame_rect(&g_now.pending_full_frame, data, data_len, x, y, width, height, row_bytes);
        if (ok) {
            g_now.pending_swap_us = esp_timer_get_time() + ((int64_t)swap_in_ms * 1000LL);
        }
    } else {
        ok = patch_frame_rect(&g_now.full_frame, data, data_len, x, y, width, height, row_bytes);
        if (ok) {
            free(g_now.pending_full_frame);
            g_now.pending_full_frame = NULL;
            g_now.pending_swap_us = 0;
        }
    }
    xSemaphoreGive(g_now_mutex);
    return ok;
}

void music_clear_full_frame(void)
{
    if (g_now_mutex == NULL) {
        music_screen_init();
    }
    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    clear_full_frames_locked(&g_now);
    xSemaphoreGive(g_now_mutex);
}

void music_set_idle_metrics(int hour,
                            int minute,
                            int temperature_c_x10,
                            int humidity_x10,
                            bool time_valid,
                            bool env_valid)
{
    if (g_now_mutex == NULL) {
        music_screen_init();
    }
    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    g_now.idle_hour = hour;
    g_now.idle_minute = minute;
    g_now.idle_temperature_c_x10 = temperature_c_x10;
    g_now.idle_humidity_x10 = humidity_x10;
    g_now.idle_time_valid = time_valid;
    g_now.idle_env_valid = env_valid;
    xSemaphoreGive(g_now_mutex);
}

void music_set_network_status(const char *ip_address)
{
    if (g_now_mutex == NULL) {
        music_screen_init();
    }
    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    copy_text(g_now.board_ip, sizeof(g_now.board_ip), ip_address == NULL || ip_address[0] == '\0' ? "acquiring" : ip_address);
    xSemaphoreGive(g_now_mutex);
}

void music_set_daemon_connected(bool connected)
{
    if (g_now_mutex == NULL) {
        music_screen_init();
    }
    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    if (g_now.daemon_connected != connected) {
        g_now.daemon_connected = connected;
        if (!connected) {
            g_now.daemon_wait_start_us = esp_timer_get_time();
        }
    }
    xSemaphoreGive(g_now_mutex);
}

void music_set_daemon_status(const char *status)
{
    if (g_now_mutex == NULL) {
        music_screen_init();
    }
    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    copy_text(g_now.daemon_status, sizeof(g_now.daemon_status), status);
    xSemaphoreGive(g_now_mutex);
}

void music_get_snapshot(NowPlayingSnapshot *snapshot, bool allow_promote)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->lyric_at = -1;
    snapshot->lyric2_at = -1;

    if (g_now_mutex == NULL) {
        music_screen_init();
    }

    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    const int64_t now_us = esp_timer_get_time();
    const int elapsed = elapsed_for(&g_now, now_us);
    promote_full_frame_if_due(&g_now, now_us);
    promote_if_due(&g_now, elapsed, now_us, allow_promote);

    copy_text(snapshot->title, sizeof(snapshot->title), g_now.title);
    copy_text(snapshot->artist, sizeof(snapshot->artist), g_now.artist);
    copy_text(snapshot->lyric, sizeof(snapshot->lyric), g_now.lyric);
    copy_text(snapshot->lyric2, sizeof(snapshot->lyric2), g_now.lyric2);
    copy_text(snapshot->lyric3, sizeof(snapshot->lyric3), g_now.lyric3);
    for (int i = 0; i < MUSIC_BITMAP_SLOT_COUNT; i++) {
        snapshot->bitmap_width[i] = g_now.slots[i].width;
        snapshot->bitmap_height[i] = g_now.slots[i].height;
    }
    snapshot->pos = g_now.pos;
    snapshot->dur = g_now.dur;
    snapshot->paused = g_now.paused;
    snapshot->lyric_at = g_now.lyric_at;
    snapshot->lyric2_at = g_now.lyric2_at;
    snapshot->elapsed = elapsed_for(&g_now, esp_timer_get_time());
    xSemaphoreGive(g_now_mutex);
}

static int font_ascent(u8g2_t *u8)
{
    return u8g2_GetAscent(u8);
}

static void draw_text_centered(u8g2_t *u8, const char *text, int top, int x, int width, int height)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }

    const int text_width = (int)u8g2_GetUTF8Width(u8, text);
    int draw_x = x + (width - text_width) / 2;
    if (draw_x < x) {
        draw_x = x;
    }
    u8g2_SetClipWindow(u8, x, top, x + width, top + height);
    u8g2_DrawUTF8(u8, draw_x, top + font_ascent(u8), text);
    u8g2_SetMaxClipWindow(u8);
}

static void draw_bitmap_pixels(u8g2_t *u8,
                               const MusicBitmapSlot *slot,
                               int dst_x,
                               int dst_y,
                               int clip_x,
                               int clip_y,
                               int clip_w,
                               int clip_h)
{
    if (!slot_has_bitmap(slot)) {
        return;
    }

    const int row_bytes = (slot->width + 7) / 8;
    const int clip_right = clip_x + clip_w;
    const int clip_bottom = clip_y + clip_h;
    for (int sy = 0; sy < (int)slot->height; sy++) {
        const int py = dst_y + sy;
        if (py < clip_y || py >= clip_bottom) {
            continue;
        }
        const uint8_t *row = slot->data + (sy * row_bytes);
        for (int sx = 0; sx < (int)slot->width; sx++) {
            const int px = dst_x + sx;
            if (px < clip_x || px >= clip_right) {
                continue;
            }
            if ((row[sx / 8] & (1U << (sx & 7))) != 0) {
                u8g2_DrawPixel(u8, px, py);
            }
        }
    }
}

static void draw_bitmap_or_text(u8g2_t *u8,
                                const MusicBitmapSlot *slot,
                                const char *fallback_text,
                                int draw_x,
                                int top,
                                int area_x,
                                int area_w,
                                int area_h)
{
    if (slot_has_bitmap(slot)) {
        const int draw_y = top + (area_h - slot->height) / 2;
        if (draw_x >= area_x && draw_y >= top && draw_x + slot->width <= area_x + area_w &&
            draw_y + slot->height <= top + area_h) {
            u8g2_DrawXBM(u8, draw_x, draw_y, slot->width, slot->height, slot->data);
        } else {
            draw_bitmap_pixels(u8, slot, draw_x, draw_y, area_x, top, area_w, area_h);
        }
    } else {
        u8g2_DrawUTF8(u8, draw_x, top + font_ascent(u8), fallback_text == NULL ? "" : fallback_text);
    }
}

static int slot_content_width(u8g2_t *u8, const MusicBitmapSlot *slot, const char *fallback_text)
{
    if (slot_has_bitmap(slot)) {
        return slot->width;
    }
    if (fallback_text != NULL && fallback_text[0] != '\0') {
        return (int)u8g2_GetUTF8Width(u8, fallback_text);
    }
    return 0;
}

static void draw_marquee_slot(u8g2_t *u8,
                              const MusicBitmapSlot *slot,
                              const char *fallback_text,
                              int top,
                              int x,
                              int width,
                              int height,
                              int clip_top,
                              int clip_h,
                              int *offset,
                              uint32_t generation,
                              uint32_t *last_generation,
                              bool centered_when_static)
{
    if (last_generation != NULL && *last_generation != generation) {
        *offset = 0;
        *last_generation = generation;
    }
    if (!slot_has_bitmap(slot) && (fallback_text == NULL || fallback_text[0] == '\0')) {
        *offset = 0;
        return;
    }

    const int content_width = slot_content_width(u8, slot, fallback_text);
    if (content_width <= 0) {
        *offset = 0;
        return;
    }

    // Clip to a fixed band (clip_top/clip_h), which may differ from the draw
    // position (top/height) — during a line transition the text slides through
    // a stationary band, so the two must be independent or the sliding line
    // would draw over the progress bar / divider above it.
    u8g2_SetClipWindow(u8, x, clip_top, x + width, clip_top + clip_h);
    if (content_width <= width) {
        *offset = 0;
        const int draw_x = centered_when_static ? x + (width - content_width) / 2 : x;
        draw_bitmap_or_text(u8, slot, fallback_text, draw_x, top, x, width, height);
    } else {
        const int gap = 42;
        draw_bitmap_or_text(u8, slot, fallback_text, x - *offset, top, x, width, height);
        draw_bitmap_or_text(u8, slot, fallback_text, x - *offset + content_width + gap, top, x, width, height);
        *offset += 1;
        if (*offset >= content_width + gap) {
            *offset -= content_width + gap;
        }
    }
    u8g2_SetMaxClipWindow(u8);
}

static void format_time(int seconds, char *dst, size_t dst_len)
{
    if (seconds < 0) {
        seconds = 0;
    }
    snprintf(dst, dst_len, "%d:%02d", seconds / 60, seconds % 60);
}

static void format_elapsed_compact(int seconds, char *dst, size_t dst_len)
{
    if (seconds < 0) {
        seconds = 0;
    }
    const int hours = seconds / 3600;
    const int minutes = (seconds / 60) % 60;
    const int secs = seconds % 60;
    if (hours > 0) {
        snprintf(dst, dst_len, "%d:%02d:%02d", hours, minutes, secs);
    } else {
        snprintf(dst, dst_len, "%d:%02d", minutes, secs);
    }
}

// u8g2_DrawRBox has no internal radius clamp: a radius larger than half the
// requested width/height underflows its unsigned coordinate math. The track
// itself is a fixed size, but the elapsed-fill box shrinks to 0px at the
// start of a track, so it must clamp its own radius (or skip rounding).
static void draw_pill_box(u8g2_t *u8, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    int r = h / 2;
    if (r > w / 2) {
        r = w / 2;
    }
    if (r <= 0) {
        u8g2_DrawBox(u8, x, y, w, h);
        return;
    }
    u8g2_DrawRBox(u8, x, y, w, h, r);
}

static void draw_progress(u8g2_t *u8, int elapsed, int dur)
{
    char elapsed_text[16];
    char remaining[16];
    format_time(elapsed, elapsed_text, sizeof(elapsed_text));
    const int remaining_sec = dur > 0 ? dur - elapsed : 0;
    snprintf(remaining, sizeof(remaining), "-%d:%02d", remaining_sec / 60, remaining_sec % 60);

    u8g2_SetFont(u8, FONT_SMALL);
    const int baseline = 116;
    u8g2_DrawUTF8(u8, 14, baseline, elapsed_text);

    const int bar_x = 76;
    const int bar_y = 105;
    const int bar_w = 238;
    const int bar_h = 8;
    u8g2_DrawRFrame(u8, bar_x, bar_y, bar_w, bar_h, bar_h / 2);
    if (dur > 0) {
        int fill = (elapsed * (bar_w - 2)) / dur;
        fill = clamp_int(fill, 0, bar_w - 2);
        draw_pill_box(u8, bar_x + 1, bar_y + 1, fill, bar_h - 2);
    }

    const int remaining_width = (int)u8g2_GetUTF8Width(u8, remaining);
    u8g2_DrawUTF8(u8, MUSIC_DISPLAY_WIDTH - 14 - remaining_width, baseline, remaining);
}

static void render_state_locked(u8g2_t *u8, NowPlaying *state, bool allow_promote)
{
    static int title_offset = 0;
    static int artist_offset = 0;
    static int lyric_offset = 0;
    static int lyric2_offset = 0;
    static uint32_t last_generation[MUSIC_BITMAP_SLOT_COUNT] = {};

    const int64_t now_us = esp_timer_get_time();
    promote_full_frame_if_due(state, now_us);
    if (state->full_frame != NULL) {
        u8g2_DrawXBM(u8, 0, 0, MUSIC_DISPLAY_WIDTH, MUSIC_DISPLAY_HEIGHT, state->full_frame);
        return;
    }

    int elapsed = elapsed_for(state, now_us);
    promote_if_due(state, elapsed, now_us, allow_promote);
    elapsed = elapsed_for(state, now_us);

    u8g2_SetFontMode(u8, 1);
    u8g2_SetDrawColor(u8, 1);

    const int width = (int)u8g2_GetDisplayWidth(u8);
    const bool has_title = slot_or_text_present(&state->slots[MUSIC_SLOT_TITLE], state->title);

    if (!has_title) {
        title_offset = artist_offset = lyric_offset = lyric2_offset = 0;
        u8g2_DrawBox(u8, 0, 0, width, MUSIC_DISPLAY_HEIGHT);
        u8g2_SetDrawColor(u8, 0);
        char clock_text[16];
        char env_text[40];
        char connect_text[48];
        char ip_text[48];
        char wait_text[16];
        if (state->idle_time_valid) {
            snprintf(clock_text, sizeof(clock_text), "%02d:%02d", state->idle_hour, state->idle_minute);
        } else {
            snprintf(clock_text, sizeof(clock_text), "--:--");
        }
        if (state->idle_env_valid) {
            snprintf(env_text,
                     sizeof(env_text),
                     "%d.%d C   %d%% RH",
                     state->idle_temperature_c_x10 / 10,
                     abs(state->idle_temperature_c_x10 % 10),
                     state->idle_humidity_x10 / 10);
        } else {
            snprintf(env_text, sizeof(env_text), "RTC / SHTC3 idle");
        }
        const int wait_seconds =
            state->daemon_wait_start_us > 0 ? (int)((now_us - state->daemon_wait_start_us) / 1000000LL) : 0;
        format_elapsed_compact(wait_seconds, wait_text, sizeof(wait_text));
        if (state->daemon_connected) {
            snprintf(connect_text, sizeof(connect_text), "Connected to daemon");
        } else if (state->daemon_status[0] != '\0') {
            snprintf(connect_text, sizeof(connect_text), "%s", state->daemon_status);
        } else {
            snprintf(connect_text, sizeof(connect_text), "Waiting to connect  %s", wait_text);
        }
        snprintf(ip_text, sizeof(ip_text), "IP %s", state->board_ip[0] == '\0' ? "acquiring" : state->board_ip);
        u8g2_SetFont(u8, FONT_SMALL);
        u8g2_DrawUTF8(u8, 14, 22, "LYRICS DISPLAY");
        u8g2_DrawHLine(u8, 12, 32, width - 24);
        u8g2_SetFont(u8, FONT_LARGE);
        draw_text_centered(u8, clock_text, 88, 0, width, 42);
        u8g2_SetFont(u8, FONT_MEDIUM);
        draw_text_centered(u8, connect_text, 146, 0, width, 30);
        u8g2_SetFont(u8, FONT_SMALL);
        draw_text_centered(u8, ip_text, 188, 0, width, 18);
        draw_text_centered(u8, env_text, 226, 0, width, 18);
        draw_text_centered(u8, "Waiting for YouTube Music", 258, 0, width, 18);
        return;
    }

    u8g2_SetFont(u8, FONT_SMALL);
    u8g2_DrawUTF8(u8, 14, 19, "NOW PLAYING");
    u8g2_DrawHLine(u8, 12, 27, width - 24);

    u8g2_SetFont(u8, FONT_LARGE);
    draw_marquee_slot(u8,
                      &state->slots[MUSIC_SLOT_TITLE],
                      state->title,
                      42,
                      14,
                      width - 28,
                      32,
                      42,
                      32,
                      &title_offset,
                      state->generation[MUSIC_SLOT_TITLE],
                      &last_generation[MUSIC_SLOT_TITLE],
                      false);

    u8g2_SetFont(u8, FONT_MEDIUM);
    draw_marquee_slot(u8,
                      &state->slots[MUSIC_SLOT_ARTIST],
                      state->artist,
                      75,
                      14,
                      width - 28,
                      24,
                      75,
                      24,
                      &artist_offset,
                      state->generation[MUSIC_SLOT_ARTIST],
                      &last_generation[MUSIC_SLOT_ARTIST],
                      false);

    draw_progress(u8, elapsed, state->dur);
    u8g2_DrawHLine(u8, 12, 130, width - 24);

    if (state->transition_start_us > 0) {
        const int elapsed_ms = (int)((now_us - state->transition_start_us) / 1000LL);
        if (elapsed_ms >= TRANSITION_MS) {
            finish_transition(state);
        } else {
            const int travel = 54;
            const int band_top = 146;
            const int band_h = 56;
            const int out_y = 158 - ((travel * elapsed_ms) / TRANSITION_MS);
            const int in_y = 158 + travel - ((travel * elapsed_ms) / TRANSITION_MS);
            u8g2_SetFont(u8, FONT_LARGE);
            draw_marquee_slot(u8,
                              &state->transition_lyric,
                              state->transition_lyric_text,
                              out_y,
                              14,
                              width - 28,
                              36,
                              band_top,
                              band_h,
                              &lyric_offset,
                              state->generation[MUSIC_SLOT_LYRIC] - 1,
                              NULL,
                              true);
            int incoming_offset = 0;
            uint32_t incoming_generation = state->generation[MUSIC_SLOT_LYRIC];
            draw_marquee_slot(u8,
                              &state->slots[MUSIC_SLOT_LYRIC],
                              state->lyric,
                              in_y,
                              14,
                              width - 28,
                              36,
                              band_top,
                              band_h,
                              &incoming_offset,
                              incoming_generation,
                              NULL,
                              true);
        }
    }

    if (state->transition_start_us == 0) {
        u8g2_SetFont(u8, FONT_LARGE);
        draw_marquee_slot(u8,
                          &state->slots[MUSIC_SLOT_LYRIC],
                          state->lyric,
                          158,
                          14,
                          width - 28,
                          36,
                          158,
                          36,
                          &lyric_offset,
                          state->generation[MUSIC_SLOT_LYRIC],
                          &last_generation[MUSIC_SLOT_LYRIC],
                          true);
    }

    u8g2_SetFont(u8, FONT_MEDIUM);
    draw_marquee_slot(u8,
                      &state->slots[MUSIC_SLOT_LYRIC2],
                      state->lyric2,
                      212,
                      14,
                      width - 28,
                      28,
                      212,
                      28,
                      &lyric2_offset,
                      state->generation[MUSIC_SLOT_LYRIC2],
                      &last_generation[MUSIC_SLOT_LYRIC2],
                      true);

    u8g2_SetFont(u8, FONT_SMALL);
    const int footer_baseline = MUSIC_DISPLAY_HEIGHT - 8;
    u8g2_DrawUTF8(u8, 14, footer_baseline, state->paused ? "PAUSED" : "PLAYING");
    const char *host = "g4pys.company";
    const int host_width = (int)u8g2_GetUTF8Width(u8, host);
    u8g2_DrawUTF8(u8, width - 14 - host_width, footer_baseline, host);
}

void music_render_current(u8g2_t *u8, bool allow_promote)
{
    if (u8 == NULL) {
        return;
    }
    if (g_now_mutex == NULL) {
        music_screen_init();
    }

    xSemaphoreTake(g_now_mutex, portMAX_DELAY);
    render_state_locked(u8, &g_now, allow_promote);
    xSemaphoreGive(g_now_mutex);
}
