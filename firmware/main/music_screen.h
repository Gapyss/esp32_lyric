#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_BITMAP_SLOT_COUNT 5

typedef enum {
    MUSIC_SLOT_TITLE = 0,
    MUSIC_SLOT_ARTIST = 1,
    MUSIC_SLOT_LYRIC = 2,
    MUSIC_SLOT_LYRIC2 = 3,
    MUSIC_SLOT_LYRIC3 = 4,
} MusicSlotIndex;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t data_len;
    uint8_t *data;
} MusicBitmapSlot;

typedef struct {
    char title[128];
    char artist[128];
    char lyric[192];
    char lyric2[192];
    char lyric3[192];
    uint16_t bitmap_width[MUSIC_BITMAP_SLOT_COUNT];
    uint16_t bitmap_height[MUSIC_BITMAP_SLOT_COUNT];
    int pos;
    int dur;
    int paused;
    int lyric_at;
    int lyric2_at;
    int elapsed;
} NowPlayingSnapshot;

void music_screen_init(void);
void music_set_from_args(const char *title,
                         const char *artist,
                         const char *lyric,
                         const char *lyric2,
                         int pos,
                         int dur,
                         int paused,
                         int lyric_at);
void music_set_from_bitmaps(const MusicBitmapSlot slots[MUSIC_BITMAP_SLOT_COUNT],
                            int pos,
                            int dur,
                            int paused,
                            int lyric_at,
                            int lyric2_at);
bool music_set_full_frame(const uint8_t *frame, uint32_t data_len, int swap_in_ms);
bool music_set_frame_rect(const uint8_t *data,
                          uint32_t data_len,
                          int x,
                          int y,
                          int width,
                          int height,
                          int row_bytes,
                          int swap_in_ms);
void music_clear_full_frame(void);
void music_set_idle_metrics(int hour,
                            int minute,
                            int temperature_c_x10,
                            int humidity_x10,
                            bool time_valid,
                            bool env_valid);
void music_set_network_status(const char *ip_address);
void music_set_daemon_connected(bool connected);
void music_get_snapshot(NowPlayingSnapshot *snapshot, bool allow_promote);
void music_render_current(u8g2_t *u8, bool allow_promote);

#ifdef __cplusplus
}
#endif
