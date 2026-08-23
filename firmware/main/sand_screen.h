#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// Ambient falling-sand clock: grains sift down all day and pile onto the
// current time, drawn as solid 7-segment walls in the middle of the field.
// When the minute changes the walls move and the stranded sand collapses.
// Everything (button actions and rendering) runs on the render task, so
// there is no internal mutex.
void sand_screen_init(void);
void sand_pour(void);
void sand_clear(void);
void sand_render_current(u8g2_t *u8);

#ifdef __cplusplus
}
#endif
