#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "app_mode.h"
#include "audio_chime.h"
#include "board_peripherals.h"
#include "board_client.h"
#include "display_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_api.h"
#include "mdns.h"
#include "music_screen.h"
#include "network_manager.h"
#include "nvs_flash.h"
#include "pet_screen.h"
#include "pomodoro_screen.h"
#include "provisioning_display.h"
#include "sand_screen.h"
#include "stats_screen.h"
#include "swarm_screen.h"
#include "clock_screen.h"
#include "comic_screen.h"
#include "u8g2_st7305.h"
#include "ui_error.h"
#include "water_screen.h"

static const char *TAG = "g4pys.company";
static u8g2_st7305_t g_lcd;
static const gpio_num_t MODE_BUTTON_GPIO = GPIO_NUM_0;
static const gpio_num_t ACTION_BUTTON_GPIO = GPIO_NUM_18;
static const int64_t ACTION_BUTTON_DEBOUNCE_US = 350000LL;
static const int64_t ACTION_BUTTON_LONG_PRESS_US = 800000LL;

static bool g_mdns_initialized;
static bool g_mdns_http_advertised;
static bool g_long_lived_services_started;
static bool g_time_sync_started;

static void time_sync_task(void *arg)
{
    (void)arg;
    setenv("TZ", "ICT-7", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SNTP already initialized");
        vTaskDelete(NULL);
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP initialization failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    if (err == ESP_OK) {
        time_t now = 0;
        time(&now);
        struct tm local = {};
        localtime_r(&now, &local);
        ESP_LOGI(TAG,
                 "SNTP time synced: %04d-%02d-%02d %02d:%02d:%02d",
                 local.tm_year + 1900,
                 local.tm_mon + 1,
                 local.tm_mday,
                 local.tm_hour,
                 local.tm_min,
                 local.tm_sec);
        esp_err_t rtc_err = board_peripherals_set_rtc_from_local_time(&local);
        if (rtc_err == ESP_OK) {
            ESP_LOGI(TAG, "PCF85063 RTC updated from SNTP");
        } else {
            ESP_LOGW(TAG, "PCF85063 RTC update failed: %s", esp_err_to_name(rtc_err));
            ui_error_show("RTC SYNC FAILED");
        }
    } else {
        ESP_LOGW(TAG, "SNTP sync not ready yet: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void time_sync_start_async(void)
{
    if (g_time_sync_started) {
        return;
    }
    g_time_sync_started = true;
    const BaseType_t ok = xTaskCreate(time_sync_task, "sntp_sync", 4096, NULL, 3, NULL);
    if (ok != pdPASS) {
        g_time_sync_started = false;
        ESP_LOGW(TAG, "Could not start SNTP task");
    }
}

static void network_services_callback(NetworkServiceAction action,
                                      const NetworkSnapshot *snapshot,
                                      void *context)
{
    (void)context;
    if (action == NETWORK_SERVICES_PAUSE) {
        board_client_network_changed(false);
        http_api_stop();
        if (g_mdns_initialized && g_mdns_http_advertised) {
            mdns_service_remove("_http", "_tcp");
            g_mdns_http_advertised = false;
        }
        music_set_daemon_connected(false);
        return;
    }

    music_set_network_status(snapshot->station_ip[0] == '\0' ? "acquiring" : snapshot->station_ip);
    board_client_network_changed(true);
    if (!g_mdns_initialized) {
        if (mdns_init() != ESP_OK) {
            ESP_LOGW(TAG, "mDNS initialization failed");
        } else {
            g_mdns_initialized = true;
        }
    }
    if (g_mdns_initialized) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_hostname_set(snapshot->hostname));
        ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_instance_name_set("g4pys.company Music Display"));
    }
    if (g_mdns_initialized && !g_mdns_http_advertised &&
        mdns_service_add("g4pys.company HTTP", "_http", "_tcp", 80, NULL, 0) == ESP_OK) {
        g_mdns_http_advertised = true;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(http_api_start());
    if (!g_long_lived_services_started) {
        g_long_lived_services_started = true;
        comic_screen_start();
        ESP_ERROR_CHECK_WITHOUT_ABORT(board_client_start());
    }
    time_sync_start_async();
    ESP_LOGI(TAG, "normal services available at %s.local", snapshot->hostname);
}

static void mode_button_init(void)
{
    gpio_config_t cfg = {};
    cfg.intr_type = GPIO_INTR_DISABLE;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pin_bit_mask = 1ULL << MODE_BUTTON_GPIO;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));
}

static void action_button_init(void)
{
    gpio_config_t cfg = {};
    cfg.intr_type = GPIO_INTR_DISABLE;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pin_bit_mask = 1ULL << ACTION_BUTTON_GPIO;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&cfg));
}

static void action_button_short_press(AppMode mode)
{
    if (mode == APP_MODE_WATER) {
        water_log_drink();
        ESP_LOGI(TAG, "drink logged via button");
    } else if (mode == APP_MODE_POMODORO) {
        pomodoro_toggle_start_pause();
        ESP_LOGI(TAG, "pomodoro start/pause toggled via button");
    } else if (mode == APP_MODE_PET) {
        pet_pet();
        ESP_LOGI(TAG, "creature petted via button");
    } else if (mode == APP_MODE_SAND) {
        sand_pour();
        ESP_LOGI(TAG, "sand poured via button");
    } else if (mode == APP_MODE_SWARM) {
        swarm_scatter();
        ESP_LOGI(TAG, "swarm scattered via button");
    } else if (mode == APP_MODE_COMIC) {
        comic_refresh();
        ESP_LOGI(TAG, "comic refresh requested via button");
    } else if (mode == APP_MODE_APOD) {
        apod_refresh();
        ESP_LOGI(TAG, "APOD refresh requested via button");
    }
}

static void action_button_long_press(AppMode mode)
{
    if (mode == APP_MODE_POMODORO) {
        pomodoro_reset();
        ESP_LOGI(TAG, "pomodoro reset via long-press");
    } else if (mode == APP_MODE_SAND) {
        sand_clear();
        ESP_LOGI(TAG, "sand field cleared via long-press");
    } else if (mode == APP_MODE_SWARM) {
        swarm_toggle_roam();
        ESP_LOGI(TAG, "swarm roam toggled via long-press");
    }
}

static void buttons_poll(void)
{
    static int last_mode_level = 1;
    static int last_action_level = 1;
    static int64_t action_started_us;
    static int64_t last_mode_action_us;
    static int64_t last_action_us;
    static int64_t chord_started_us;
    static bool action_long_fired;
    static bool chord_suppressed;
    static bool setup_opened;
    static bool reset_fired;

    const int mode_level = gpio_get_level(MODE_BUTTON_GPIO);
    const int action_level = gpio_get_level(ACTION_BUTTON_GPIO);
    const int64_t now_us = esp_timer_get_time();
    const bool both_down = mode_level == 0 && action_level == 0;

    if (both_down) {
        if (!chord_suppressed) {
            chord_suppressed = true;
            chord_started_us = now_us;
            setup_opened = false;
            reset_fired = false;
            action_long_fired = true;
        }
        const int64_t held_us = now_us - chord_started_us;
        if (held_us >= 5000000LL && !setup_opened) {
            setup_opened = true;
            ESP_ERROR_CHECK_WITHOUT_ABORT(network_manager_open_manual_setup());
        }
        if (held_us >= 5000000LL && held_us < 15000000LL) {
            const int remaining = (int)((15000000LL - held_us + 999999LL) / 1000000LL);
            network_manager_set_reset_countdown(remaining);
        }
        if (held_us >= 15000000LL && !reset_fired) {
            reset_fired = true;
            network_manager_set_reset_countdown(0);
            ESP_ERROR_CHECK_WITHOUT_ABORT(network_manager_reset_provisioning());
        }
        last_mode_level = mode_level;
        last_action_level = action_level;
        return;
    }

    if (chord_suppressed) {
        network_manager_set_reset_countdown(0);
        if (mode_level == 1 && action_level == 1) {
            chord_suppressed = false;
        }
        last_mode_level = mode_level;
        last_action_level = action_level;
        return;
    }

    if (network_manager_provisioning_display_active()) {
        last_mode_level = mode_level;
        last_action_level = action_level;
        return;
    }

    const AppMode mode = app_mode_get();
    if (last_mode_level == 0 && mode_level == 1 && now_us - last_mode_action_us > 350000LL) {
        const AppMode next = app_mode_toggle();
        last_mode_action_us = now_us;
        ESP_LOGI(TAG, "BOOT button toggled mode to %s", app_mode_name(next));
        if (next != APP_MODE_WATER && next != APP_MODE_POMODORO) {
            audio_chime_stop();
        }
    }

    if (last_action_level == 1 && action_level == 0) {
        action_started_us = now_us;
        action_long_fired = false;
    } else if (last_action_level == 0 && action_level == 0 && !action_long_fired &&
               now_us - action_started_us > ACTION_BUTTON_LONG_PRESS_US) {
        action_long_fired = true;
        last_action_us = now_us;
        action_button_long_press(mode);
    } else if (last_action_level == 0 && action_level == 1) {
        if (!action_long_fired && now_us - last_action_us > ACTION_BUTTON_DEBOUNCE_US) {
            action_button_short_press(mode);
            last_action_us = now_us;
        }
        action_long_fired = false;
    }

    last_mode_level = mode_level;
    last_action_level = action_level;
}

static void display_start(void)
{
    u8g2_st7305_config_t cfg = u8g2_st7305_default_config();
    cfg.mosi_io = GPIO_NUM_12;
    cfg.sclk_io = GPIO_NUM_11;
    cfg.dc_io = GPIO_NUM_5;
    cfg.cs_io = GPIO_NUM_40;
    cfg.reset_io = GPIO_NUM_41;
    cfg.rotation = U8G2_R1;
    cfg.tile_buf_height = U8G2_ST7305_TILE_BUF_FULL;
    ESP_ERROR_CHECK(u8g2_st7305_init(&g_lcd, &cfg));
    u8g2_t *u8 = u8g2_st7305_get_u8g2(&g_lcd);
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int height = (int)u8g2_GetDisplayHeight(u8);
    ESP_LOGI(TAG,
             "LCD canvas %dx%d, frame protocol %ux%u",
             width,
             height,
             MUSIC_DISPLAY_WIDTH,
             MUSIC_DISPLAY_HEIGHT);
    ESP_ERROR_CHECK(width == MUSIC_DISPLAY_WIDTH && height == MUSIC_DISPLAY_HEIGHT ? ESP_OK : ESP_ERR_INVALID_SIZE);
}

static void render_task(void *arg)
{
    u8g2_t *u8 = (u8g2_t *)arg;
    int64_t last_metrics_us = 0;

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        const AppMode mode = app_mode_get();
        if (mode == APP_MODE_MUSIC && now_us - last_metrics_us > 1000000LL) {
            BoardIdleMetrics metrics = {};
            board_peripherals_read(&metrics);
            music_set_idle_metrics(metrics.hour,
                                   metrics.minute,
                                   metrics.temperature_c_x10,
                                   metrics.humidity_x10,
                                   metrics.time_valid,
                                   metrics.env_valid);
            last_metrics_us = now_us;
        }
        buttons_poll();
        // Ticks unconditionally, unlike water_tick, so a countdown started on
        // this screen keeps advancing (and can still alert) while another
        // screen is displayed.
        pomodoro_tick();
        u8g2_ClearBuffer(u8);
        if (network_manager_provisioning_display_active()) {
            provisioning_display_render(u8);
        } else if (mode == APP_MODE_WATER) {
            water_tick();
            water_render_current(u8);
        } else if (mode == APP_MODE_STATS) {
            stats_render_current(u8);
        } else if (mode == APP_MODE_POMODORO) {
            pomodoro_render_current(u8);
        } else if (mode == APP_MODE_CLOCK) {
            clock_render_current(u8);
        } else if (mode == APP_MODE_PET) {
            pet_render_current(u8);
        } else if (mode == APP_MODE_SAND) {
            sand_render_current(u8);
        } else if (mode == APP_MODE_SWARM) {
            swarm_render_current(u8);
        } else if (mode == APP_MODE_COMIC) {
            comic_render_current(u8);
        } else if (mode == APP_MODE_APOD) {
            apod_render_current(u8);
        } else {
            music_render_current(u8, true);
        }
        ui_error_render(u8);
        u8g2_SendBuffer(u8);
        vTaskDelay(pdMS_TO_TICKS(70));
    }
}

extern "C" void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    app_mode_init();
    ESP_LOGI(TAG, "app mode default: %s", app_mode_name(app_mode_get()));
    music_screen_init();
    water_screen_init();
    stats_screen_init();
    clock_screen_init();
    pomodoro_screen_init();
    pet_screen_init();
    sand_screen_init();
    swarm_screen_init();
    comic_screen_init();
    display_start();
    ESP_ERROR_CHECK(board_peripherals_start());
    mode_button_init();
    action_button_init();
    esp_err_t audio_err = audio_chime_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "Audio chime unavailable at boot: %s", esp_err_to_name(audio_err));
        ui_error_show("AUDIO INIT FAILED");
    }

    ESP_LOGI(TAG, "starting render task");
    BaseType_t task_ok = xTaskCreatePinnedToCore(render_task,
                                                "render",
                                                8192,
                                                u8g2_st7305_get_u8g2(&g_lcd),
                                                5,
                                                NULL,
                                                1);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    music_set_network_status("acquiring");
    ESP_LOGI(TAG, "starting non-blocking network manager");
    ESP_ERROR_CHECK(network_manager_start(network_services_callback, NULL));
}
