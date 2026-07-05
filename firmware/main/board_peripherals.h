#pragma once

#include <stdbool.h>

#include "esp_err.h"

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
} BoardIdleMetrics;

esp_err_t board_peripherals_start(void);
void board_peripherals_read(BoardIdleMetrics *metrics);

#ifdef __cplusplus
}
#endif
