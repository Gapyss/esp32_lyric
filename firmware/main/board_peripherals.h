#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool time_valid;
    int hour;
    int minute;
    bool env_valid;
    int temperature_c_x10;
    int humidity_x10;
    bool battery_valid;
    int battery_mv;
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
