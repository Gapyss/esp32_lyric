#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "board_peripherals.h"
#include "board_client.h"
#include "display_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "http_api.h"
#include "mdns.h"
#include "music_screen.h"
#include "nvs_flash.h"
#include "u8g2_st7305.h"
#include "wifi_secrets.h"

static const char *TAG = "g4pys.company";
static const int WIFI_CONNECTED_BIT = BIT0;
static EventGroupHandle_t g_wifi_events;
static u8g2_st7305_t g_lcd;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        music_set_network_status("acquiring");
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
        music_set_network_status("acquiring");
        music_set_daemon_connected(false);
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        char ip[24];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        music_set_network_status(ip);
        ESP_LOGI(TAG, "WiFi connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    g_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", WIFI_PASS);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to WiFi SSID '%s'", WIFI_SSID);
    xEventGroupWaitBits(g_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

static void mdns_start(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("g4pys-company"));
    ESP_ERROR_CHECK(mdns_instance_name_set("g4pys.company Music Display"));
    ESP_ERROR_CHECK(mdns_service_add("g4pys.company HTTP", "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mDNS hostname set: g4pys-company.local");
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
        if (now_us - last_metrics_us > 1000000LL) {
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
        u8g2_ClearBuffer(u8);
        music_render_current(u8, true);
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

    music_screen_init();
    display_start();
    ESP_ERROR_CHECK(board_peripherals_start());

    BaseType_t task_ok = xTaskCreatePinnedToCore(render_task,
                                                "render",
                                                8192,
                                                u8g2_st7305_get_u8g2(&g_lcd),
                                                5,
                                                NULL,
                                                1);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    wifi_start();
    mdns_start();
    ESP_ERROR_CHECK(http_api_start());
    ESP_ERROR_CHECK(board_client_start());
}
