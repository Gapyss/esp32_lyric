#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool alerting;
    bool clock_valid;
    int hour;
    int minute;
    int next_in_sec;
    int interval_min;
    int active_start_min;
    int active_end_min;
    int alert_remaining_sec;
    bool audio_available;
    bool audio_playing;
    int drinks_today;
} WaterSnapshot;

void water_screen_init(void);
void water_tick(void);
void water_render_current(u8g2_t *u8);
void water_fire_now(void);
void water_snooze(int minutes);
void water_log_drink(void);
esp_err_t water_configure(int interval_min, int active_start_min, int active_end_min);
void water_get_snapshot(WaterSnapshot *snapshot);

#ifdef __cplusplus
}
#endif
