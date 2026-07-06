#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_chime_init(void);
void audio_chime_start(void);
void audio_chime_stop(void);
bool audio_chime_available(void);
bool audio_chime_is_playing(void);

#ifdef __cplusplus
}
#endif
