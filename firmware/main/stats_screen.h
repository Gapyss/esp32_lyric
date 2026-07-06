#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// "Board stats" page: temperature/humidity trend charts plus battery and
// history-span tiles. Only ever driven from the single render task, so
// unlike water_screen/music_screen it keeps no internal mutex.
void stats_screen_init(void);
void stats_render_current(u8g2_t *u8);

#ifdef __cplusplus
}
#endif
