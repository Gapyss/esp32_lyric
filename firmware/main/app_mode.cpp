#include "app_mode.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static AppMode g_mode = APP_DEFAULT_MODE;
static SemaphoreHandle_t g_mode_mutex;

void app_mode_init(void)
{
    if (g_mode_mutex == NULL) {
        g_mode_mutex = xSemaphoreCreateMutex();
    }
    g_mode = APP_DEFAULT_MODE;
}

AppMode app_mode_get(void)
{
    if (g_mode_mutex == NULL) {
        app_mode_init();
    }
    xSemaphoreTake(g_mode_mutex, portMAX_DELAY);
    const AppMode mode = g_mode;
    xSemaphoreGive(g_mode_mutex);
    return mode;
}

void app_mode_set(AppMode mode)
{
    if (mode != APP_MODE_MUSIC && mode != APP_MODE_WATER && mode != APP_MODE_STATS &&
        mode != APP_MODE_POMODORO && mode != APP_MODE_CLOCK && mode != APP_MODE_PET &&
        mode != APP_MODE_SAND) {
        return;
    }
    if (g_mode_mutex == NULL) {
        app_mode_init();
    }
    xSemaphoreTake(g_mode_mutex, portMAX_DELAY);
    g_mode = mode;
    xSemaphoreGive(g_mode_mutex);
}

AppMode app_mode_toggle(void)
{
    if (g_mode_mutex == NULL) {
        app_mode_init();
    }
    xSemaphoreTake(g_mode_mutex, portMAX_DELAY);
    switch (g_mode) {
        case APP_MODE_MUSIC:
            g_mode = APP_MODE_WATER;
            break;
        case APP_MODE_WATER:
            g_mode = APP_MODE_STATS;
            break;
        case APP_MODE_STATS:
            g_mode = APP_MODE_POMODORO;
            break;
        case APP_MODE_POMODORO:
            g_mode = APP_MODE_CLOCK;
            break;
        case APP_MODE_CLOCK:
            g_mode = APP_MODE_PET;
            break;
        case APP_MODE_PET:
            g_mode = APP_MODE_SAND;
            break;
        default:
            g_mode = APP_MODE_MUSIC;
            break;
    }
    const AppMode mode = g_mode;
    xSemaphoreGive(g_mode_mutex);
    return mode;
}

const char *app_mode_name(AppMode mode)
{
    switch (mode) {
        case APP_MODE_WATER:
            return "water";
        case APP_MODE_STATS:
            return "stats";
        case APP_MODE_POMODORO:
            return "pomodoro";
        case APP_MODE_CLOCK:
            return "clock";
        case APP_MODE_PET:
            return "pet";
        case APP_MODE_SAND:
            return "sand";
        default:
            return "music";
    }
}
