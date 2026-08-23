#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// What the pack voltage has been doing over the last hour, which decides
// whether a "time left" figure means anything at all.
typedef enum {
    BOARD_BATTERY_TREND_UNKNOWN = 0, // too few samples so far to say
    BOARD_BATTERY_TREND_DISCHARGING, // battery_runtime_minutes is meaningful
    BOARD_BATTERY_TREND_CHARGING,    // voltage climbing, i.e. USB is plugged in
    BOARD_BATTERY_TREND_STEADY,      // drain too slow to put a number on
} BoardBatteryTrend;

// Runtime estimates are clamped here rather than extrapolated further; past a
// day out the fit is not saying anything a two-field figure should imply.
#define BOARD_BATTERY_RUNTIME_CAP_MINUTES (24 * 60)

typedef struct {
    bool time_valid;
    int hour;
    int minute;
    bool env_valid;
    int temperature_c_x10;
    int humidity_x10;
    bool battery_valid;
    int battery_mv; // smoothed, so the reading doesn't jitter frame to frame
    int battery_percent;
    BoardBatteryTrend battery_trend;
    int battery_runtime_minutes; // only set when trend is DISCHARGING
} BoardIdleMetrics;

// Rolling log of environment readings, sampled roughly once a minute
// regardless of which screen is active, so a trend has data to show as soon
// as the stats page is opened. Oldest sample is index 0.
#define BOARD_HISTORY_CAPACITY 120

typedef struct {
    int count;
    int temperature_c_x10[BOARD_HISTORY_CAPACITY];
    int humidity_x10[BOARD_HISTORY_CAPACITY];
} BoardHistory;

esp_err_t board_peripherals_start(void);
void board_peripherals_read(BoardIdleMetrics *metrics);
void board_peripherals_get_history(BoardHistory *history);
esp_err_t board_peripherals_set_rtc_from_local_time(const struct tm *local);
i2c_master_bus_handle_t board_peripherals_i2c_bus(void);

#ifdef __cplusplus
}
#endif
