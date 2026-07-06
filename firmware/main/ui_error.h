#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// Shows `message` as a banner on the next few render_task frames, then
// auto-hides after UI_ERROR_DURATION_US regardless of what mode/screen is
// active underneath.
void ui_error_show(const char *message);

// Draws the banner over whatever is already in the u8g2 buffer if one is
// currently active. Returns true if it drew anything.
bool ui_error_render(u8g2_t *u8g2);

#ifdef __cplusplus
}
#endif
