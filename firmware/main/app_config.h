#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODE_MUSIC = 0,
    APP_MODE_WATER = 1,
    APP_MODE_STATS = 2,
    APP_MODE_POMODORO = 3,
    APP_MODE_CLOCK = 4,
    APP_MODE_PET = 5,
    APP_MODE_SAND = 6,
} AppMode;

#define APP_DEFAULT_MODE APP_MODE_WATER

#define HYDRATE_DEFAULT_INTERVAL_MIN 45
#define HYDRATE_DEFAULT_ACTIVE_START_MIN (9 * 60)
#define HYDRATE_DEFAULT_ACTIVE_END_MIN (18 * 60)
#define HYDRATE_ALERT_TIMEOUT_SEC 30

#define POMODORO_DEFAULT_MINUTES 25
#define POMODORO_ALERT_TIMEOUT_SEC 30

#define PET_NVS_NAMESPACE "pet"
#define PET_NVS_TOTAL_KEY "total"
#define PET_NVS_ADOPT_KEY "adopt"

#define HYDRATE_NVS_NAMESPACE "hydrate"
#define HYDRATE_NVS_INTERVAL_KEY "interval"
#define HYDRATE_NVS_START_KEY "start"
#define HYDRATE_NVS_END_KEY "end"

#ifdef __cplusplus
}
#endif
