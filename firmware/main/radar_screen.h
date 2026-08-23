#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// Rain-radar scope for a fixed home location: live RainViewer returns around
// home on a 300x300 dark scope, with a beam that sweeps once a minute, plus a
// 100 px panel showing Open-Meteo rain probability at +3h and +7h.
//
// Everything runs on the board -- no Mac, no daemon -- so the screen keeps
// working while the Mac is asleep. Fetching happens only while this screen is
// displayed: radar_render_current() is what asks the worker task for fresh
// data, so the other screens cost nothing.
//
// See radar-screen-design.md for the tile math, the pixel-encoding survey and
// why the intensity threshold sits where it does.
void radar_screen_init(void);
void radar_screen_start(void);
void radar_refresh(void);
void radar_render_current(u8g2_t *u8);

#ifdef __cplusplus
}
#endif
