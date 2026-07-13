#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

// "Year clock" page (Tend design): a day-of-year progress grid -- one square
// per day of the year, elapsed days filled, today highlighted, remaining days
// hollow -- alongside the same temperature/humidity trend charts as the stats
// page. Driven only from the single render task, so like stats_screen it keeps
// no internal mutex.
void clock_screen_init(void);
void clock_render_current(u8g2_t *u8);

#ifdef __cplusplus
}
#endif
