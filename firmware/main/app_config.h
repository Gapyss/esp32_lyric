#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_MODE_MUSIC = 0,
    APP_MODE_STATS = 1,
    APP_MODE_POMODORO = 2,
    APP_MODE_CLOCK = 3,
    APP_MODE_SAND = 4,
    APP_MODE_COMIC = 5,
    APP_MODE_APOD = 6,
} AppMode;

#define APP_DEFAULT_MODE APP_MODE_STATS

#define POMODORO_DEFAULT_MINUTES 25
#define POMODORO_ALERT_TIMEOUT_SEC 30

#ifdef __cplusplus
}
#endif
