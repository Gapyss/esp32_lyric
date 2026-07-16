#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// Desk creature ("EMBER") screen: a small companion whose mood is driven by
// the board's real sensors -- SHTC3 temperature/humidity, RTC time of day --
// and by petting it with the action button. Only ever driven from the render
// task (button poll and render both happen there), so like pomodoro_screen it
// keeps no internal mutex.
void pet_screen_init(void);
void pet_pet(void);
void pet_render_current(u8g2_t *u8);

#ifdef __cplusplus
}
#endif
