#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ambient firefly swarm clock: a few hundred ink flies drift around the
// panel and settle onto sample points of the current time, drawn as big
// 7-segment outlines. When the minute changes the targets move and the
// swarm morphs into the new digits. Like pet_screen and sand_screen,
// everything (button actions and rendering) runs on the render task, so
// there is no internal mutex.
void swarm_screen_init(void);
void swarm_scatter(void);
void swarm_toggle_roam(void);
void swarm_render_current(u8g2_t *u8);

#ifdef __cplusplus
}
#endif
