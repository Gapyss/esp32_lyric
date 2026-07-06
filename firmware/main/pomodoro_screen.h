#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// Single fixed-duration countdown screen. Only ever driven from the render
// task (button polls, tick, and render all happen there), so unlike
// water_screen it keeps no internal mutex -- same reasoning as stats_screen.
void pomodoro_screen_init(void);
void pomodoro_tick(void);
void pomodoro_toggle_start_pause(void);
void pomodoro_reset(void);
void pomodoro_render_current(u8g2_t *u8);

#ifdef __cplusplus
}
#endif
