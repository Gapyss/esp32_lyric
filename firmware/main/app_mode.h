#pragma once

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_mode_init(void);
AppMode app_mode_get(void);
void app_mode_set(AppMode mode);
AppMode app_mode_toggle(void);
const char *app_mode_name(AppMode mode);

#ifdef __cplusplus
}
#endif
