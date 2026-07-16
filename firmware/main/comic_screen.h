#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

void comic_screen_init(void);
void comic_screen_start(void);
void comic_refresh(void);
void comic_render_current(u8g2_t *u8);
void apod_refresh(void);
void apod_render_current(u8g2_t *u8);

#ifdef __cplusplus
}
#endif
