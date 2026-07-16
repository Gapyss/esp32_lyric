#include "http_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "app_mode.h"
#include "display_config.h"
#include "music_screen.h"
#include "water_screen.h"

static const char *TAG = "http_api";
static httpd_handle_t g_server;
static const uint16_t MAX_BITMAP_WIDTH = 2048;
static const uint16_t MAX_BITMAP_HEIGHT = 64;
static const uint32_t MAX_BITMAP_BYTES = 65536;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }

    size_t out = 0;
    for (size_t in = 0; src != NULL && src[in] != '\0' && out + 1 < dst_len; in++) {
        if (src[in] == '%' && src[in + 1] != '\0' && src[in + 2] != '\0') {
            const int hi = hex_value(src[in + 1]);
            const int lo = hex_value(src[in + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                in += 2;
                continue;
            }
        }
        dst[out++] = src[in] == '+' ? ' ' : src[in];
    }
    dst[out] = '\0';
}

static bool query_arg(const char *query, const char *key, char *dst, size_t dst_len)
{
    char encoded[768] = {0};
    if (httpd_query_key_value(query, key, encoded, sizeof(encoded)) != ESP_OK) {
        if (dst_len > 0) {
            dst[0] = '\0';
        }
        return false;
    }
    url_decode(dst, dst_len, encoded);
    return true;
}

static int query_int(const char *query, const char *key, int default_value)
{
    char value[32] = {0};
    if (!query_arg(query, key, value, sizeof(value))) {
        return default_value;
    }
    return atoi(value);
}

static bool parse_hhmm(const char *value, int *minutes)
{
    int hour = -1;
    int minute = -1;
    if (value == NULL || sscanf(value, "%d:%d", &hour, &minute) != 2) {
        return false;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }
    *minutes = hour * 60 + minute;
    return true;
}

static esp_err_t read_query(httpd_req_t *req, char **query_out)
{
    const size_t query_len = httpd_req_get_url_query_len(req);
    char *query = (char *)calloc(query_len + 1, 1);
    if (query == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (query_len > 0) {
        const esp_err_t err = httpd_req_get_url_query_str(req, query, query_len + 1);
        if (err != ESP_OK) {
            free(query);
            return err;
        }
    }
    *query_out = query;
    return ESP_OK;
}

static void frame_set_pixel(uint8_t *frame, int x, int y)
{
    if (x < 0 || x >= MUSIC_DISPLAY_WIDTH || y < 0 || y >= MUSIC_DISPLAY_HEIGHT) {
        return;
    }
    const int row_bytes = MUSIC_DISPLAY_ROW_BYTES;
    frame[y * row_bytes + (x / 8)] |= (uint8_t)(1U << (x & 7));
}

static void frame_box(uint8_t *frame, int x, int y, int w, int h)
{
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            frame_set_pixel(frame, px, py);
        }
    }
}

static void build_diag_frame(uint8_t *frame, const char *pattern)
{
    memset(frame, 0, MUSIC_DISPLAY_BYTES);
    if (strcmp(pattern, "polarity") == 0) {
        for (int y = 0; y < MUSIC_DISPLAY_HEIGHT; y++) {
            for (int x = 0; x < MUSIC_DISPLAY_WIDTH; x++) {
                if (((x / 8) + (y / 8)) & 1) {
                    frame_set_pixel(frame, x, y);
                }
            }
        }
        return;
    }
    if (strcmp(pattern, "timing") == 0) {
        for (int y = 0; y < MUSIC_DISPLAY_HEIGHT; y += 12) {
            frame_box(frame, 0, y, MUSIC_DISPLAY_WIDTH, 6);
        }
        for (int x = 0; x < MUSIC_DISPLAY_WIDTH; x += 24) {
            frame_box(frame, x, 0, 4, MUSIC_DISPLAY_HEIGHT);
        }
        return;
    }

    frame_box(frame, 0, 0, MUSIC_DISPLAY_WIDTH, 2);
    frame_box(frame, 0, MUSIC_DISPLAY_HEIGHT - 2, MUSIC_DISPLAY_WIDTH, 2);
    frame_box(frame, 0, 0, 2, MUSIC_DISPLAY_HEIGHT);
    frame_box(frame, MUSIC_DISPLAY_WIDTH - 2, 0, 2, MUSIC_DISPLAY_HEIGHT);
    frame_box(frame, 8, 8, 52, 52);
    frame_box(frame, MUSIC_DISPLAY_WIDTH - 60, 8, 52, 28);
    frame_box(frame, 8, MUSIC_DISPLAY_HEIGHT - 60, 28, 52);
    frame_box(frame, MUSIC_DISPLAY_WIDTH - 60, MUSIC_DISPLAY_HEIGHT - 60, 52, 52);
    for (int i = 0; i < MUSIC_DISPLAY_WIDTH && i < MUSIC_DISPLAY_HEIGHT; i++) {
        frame_set_pixel(frame, i, i);
        frame_set_pixel(frame, MUSIC_DISPLAY_WIDTH - 1 - i, i);
    }
}

static void free_slots(MusicBitmapSlot slots[MUSIC_BITMAP_SLOT_COUNT])
{
    for (int i = 0; i < MUSIC_BITMAP_SLOT_COUNT; i++) {
        free(slots[i].data);
        slots[i].data = NULL;
        slots[i].width = 0;
        slots[i].height = 0;
        slots[i].data_len = 0;
    }
}

static esp_err_t recv_exact(httpd_req_t *req, uint8_t *dst, size_t len, size_t *remaining)
{
    size_t received = 0;
    while (received < len) {
        if (*remaining == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        const size_t want = len - received;
        const size_t chunk = want < *remaining ? want : *remaining;
        const int ret = httpd_req_recv(req, (char *)dst + received, chunk);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)ret;
        *remaining -= (size_t)ret;
    }
    return ESP_OK;
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) | ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static bool validate_bitmap_header(uint16_t width, uint16_t height, uint32_t data_len)
{
    if (width == 0 || height == 0) {
        return width == 0 && height == 0 && data_len == 0;
    }
    if (width > MAX_BITMAP_WIDTH || height > MAX_BITMAP_HEIGHT || data_len > MAX_BITMAP_BYTES) {
        return false;
    }
    const uint32_t expected = ((uint32_t)width + 7U) / 8U * (uint32_t)height;
    return data_len == expected;
}

static void *alloc_bitmap_bytes(uint32_t data_len)
{
    if (data_len == 0) {
        return NULL;
    }
    void *data = heap_caps_malloc(data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        data = heap_caps_malloc(data_len, MALLOC_CAP_8BIT);
    }
    return data;
}

static esp_err_t read_bitmap_body(httpd_req_t *req, MusicBitmapSlot slots[MUSIC_BITMAP_SLOT_COUNT])
{
    size_t remaining = req->content_len;
    for (int i = 0; i < MUSIC_BITMAP_SLOT_COUNT; i++) {
        uint8_t header[8];
        esp_err_t err = recv_exact(req, header, sizeof(header), &remaining);
        if (err != ESP_OK) {
            return err;
        }

        const uint16_t width = read_le16(header);
        const uint16_t height = read_le16(header + 2);
        const uint32_t data_len = read_le32(header + 4);
        if (!validate_bitmap_header(width, height, data_len) || data_len > remaining) {
            ESP_LOGW(TAG,
                     "rejecting bitmap slot %d: %ux%u len=%lu remaining=%u",
                     i,
                     width,
                     height,
                     (unsigned long)data_len,
                     (unsigned)remaining);
            return ESP_ERR_INVALID_SIZE;
        }

        slots[i].width = width;
        slots[i].height = height;
        slots[i].data_len = data_len;
        if (data_len > 0) {
            slots[i].data = (uint8_t *)alloc_bitmap_bytes(data_len);
            if (slots[i].data == NULL) {
                return ESP_ERR_NO_MEM;
            }
            err = recv_exact(req, slots[i].data, data_len, &remaining);
            if (err != ESP_OK) {
                return err;
            }
        }
    }

    return remaining == 0 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t nowplaying_handler(httpd_req_t *req)
{
    const size_t query_len = httpd_req_get_url_query_len(req);
    char *query = (char *)calloc(query_len + 1, 1);
    if (query == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    if (query_len > 0) {
        esp_err_t err = httpd_req_get_url_query_str(req, query, query_len + 1);
        if (err != ESP_OK) {
            free(query);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad query");
        }
    }

    if (req->content_len > 0) {
        MusicBitmapSlot slots[MUSIC_BITMAP_SLOT_COUNT] = {};
        const esp_err_t err = read_bitmap_body(req, slots);
        if (err == ESP_OK) {
            music_set_from_bitmaps(slots,
                                   query_int(query, "pos", 0),
                                   query_int(query, "dur", 0),
                                   query_int(query, "paused", 0),
                                   query_int(query, "lt", -1),
                                   query_int(query, "lt2", -1));
            memset(slots, 0, sizeof(slots));
        }
        free_slots(slots);
        free(query);
        if (err == ESP_ERR_NO_MEM) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        }
        if (err != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad bitmap body");
        }
    } else {
        char title[128] = {0};
        char artist[128] = {0};
        char lyric[192] = {0};
        char lyric2[192] = {0};
        const bool has_title = query_arg(query, "title", title, sizeof(title));
        query_arg(query, "artist", artist, sizeof(artist));
        query_arg(query, "lyric", lyric, sizeof(lyric));
        query_arg(query, "lyric2", lyric2, sizeof(lyric2));

        if (has_title) {
            music_set_from_args(title,
                                artist,
                                lyric,
                                lyric2,
                                query_int(query, "pos", 0),
                                query_int(query, "dur", 0),
                                query_int(query, "paused", 0),
                                query_int(query, "lt", -1));
        }
        free(query);
    }

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static void json_escape(char *dst, size_t dst_len, const char *src)
{
    if (dst_len == 0) {
        return;
    }

    size_t out = 0;
    for (size_t in = 0; src != NULL && src[in] != '\0' && out + 1 < dst_len; in++) {
        const unsigned char c = (unsigned char)src[in];
        if ((c == '"' || c == '\\') && out + 2 < dst_len) {
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c == '\n' && out + 2 < dst_len) {
            dst[out++] = '\\';
            dst[out++] = 'n';
        } else if (c == '\r' && out + 2 < dst_len) {
            dst[out++] = '\\';
            dst[out++] = 'r';
        } else if (c == '\t' && out + 2 < dst_len) {
            dst[out++] = '\\';
            dst[out++] = 't';
        } else if (c < 0x20 && out + 7 < dst_len) {
            out += snprintf(dst + out, dst_len - out, "\\u%04x", c);
        } else {
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
}

static esp_err_t usage_handler(httpd_req_t *req)
{
    NowPlayingSnapshot snapshot;
    music_get_snapshot(&snapshot, false);

    char title[257];
    char artist[257];
    char lyric[385];
    char lyric2[385];
    char lyric3[385];
    json_escape(title, sizeof(title), snapshot.title);
    json_escape(artist, sizeof(artist), snapshot.artist);
    json_escape(lyric, sizeof(lyric), snapshot.lyric);
    json_escape(lyric2, sizeof(lyric2), snapshot.lyric2);
    json_escape(lyric3, sizeof(lyric3), snapshot.lyric3);

    char body[2304];
    snprintf(body,
             sizeof(body),
             "{\"mode\":\"music\",\"title\":\"%s\",\"artist\":\"%s\",\"lyric\":\"%s\","
             "\"lyric2\":\"%s\",\"lyric3\":\"%s\",\"pos\":%d,\"dur\":%d,\"paused\":%d,"
             "\"lt\":%d,\"lt2\":%d,"
             "\"bitmaps\":[[%u,%u],[%u,%u],[%u,%u],[%u,%u],[%u,%u]]}",
             title,
             artist,
             lyric,
             lyric2,
             lyric3,
             snapshot.elapsed,
             snapshot.dur,
             snapshot.paused,
             snapshot.lyric_at,
             snapshot.lyric2_at,
             snapshot.bitmap_width[0],
             snapshot.bitmap_height[0],
             snapshot.bitmap_width[1],
             snapshot.bitmap_height[1],
             snapshot.bitmap_width[2],
             snapshot.bitmap_height[2],
             snapshot.bitmap_width[3],
             snapshot.bitmap_height[3],
             snapshot.bitmap_width[4],
             snapshot.bitmap_height[4]);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t mode_handler(httpd_req_t *req)
{
    char *query = NULL;
    esp_err_t err = read_query(req, &query);
    if (err == ESP_ERR_NO_MEM) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad query");
    }

    char mode[16] = {};
    if (query_arg(query, "set", mode, sizeof(mode))) {
        if (strcmp(mode, "water") == 0) {
            app_mode_set(APP_MODE_WATER);
        } else if (strcmp(mode, "music") == 0) {
            app_mode_set(APP_MODE_MUSIC);
        } else if (strcmp(mode, "stats") == 0) {
            app_mode_set(APP_MODE_STATS);
        } else if (strcmp(mode, "clock") == 0) {
            app_mode_set(APP_MODE_CLOCK);
        } else if (strcmp(mode, "pet") == 0) {
            app_mode_set(APP_MODE_PET);
        } else if (strcmp(mode, "pomodoro") == 0) {
            app_mode_set(APP_MODE_POMODORO);
        } else if (strcmp(mode, "sand") == 0) {
            app_mode_set(APP_MODE_SAND);
        } else if (strcmp(mode, "swarm") == 0) {
            app_mode_set(APP_MODE_SWARM);
        } else if (strcmp(mode, "comic") == 0) {
            app_mode_set(APP_MODE_COMIC);
        } else if (strcmp(mode, "apod") == 0) {
            app_mode_set(APP_MODE_APOD);
        } else {
            free(query);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be water, music, stats, pomodoro, clock, pet, sand, swarm, comic, or apod");
        }
    }
    free(query);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, app_mode_name(app_mode_get()));
}

static esp_err_t hydrate_now_handler(httpd_req_t *req)
{
    app_mode_set(APP_MODE_WATER);
    water_fire_now();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t hydrate_log_handler(httpd_req_t *req)
{
    app_mode_set(APP_MODE_WATER);
    water_log_drink();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t hydrate_snooze_handler(httpd_req_t *req)
{
    char *query = NULL;
    esp_err_t err = read_query(req, &query);
    if (err == ESP_ERR_NO_MEM) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad query");
    }
    const int minutes = query_int(query, "min", 10);
    free(query);
    if (minutes <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "min must be positive");
    }
    app_mode_set(APP_MODE_WATER);
    water_snooze(minutes);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t hydrate_config_handler(httpd_req_t *req)
{
    char *query = NULL;
    esp_err_t err = read_query(req, &query);
    if (err == ESP_ERR_NO_MEM) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad query");
    }

    WaterSnapshot current;
    water_get_snapshot(&current);
    int interval = query_int(query, "interval", current.interval_min);
    int start = current.active_start_min;
    int end = current.active_end_min;
    char value[16] = {};
    if (query_arg(query, "start", value, sizeof(value)) && !parse_hhmm(value, &start)) {
        free(query);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "start must be HH:MM");
    }
    if (query_arg(query, "end", value, sizeof(value)) && !parse_hhmm(value, &end)) {
        free(query);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "end must be HH:MM");
    }
    free(query);

    err = water_configure(interval, start, end);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad hydration config");
    }
    app_mode_set(APP_MODE_WATER);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t hydrate_json_handler(httpd_req_t *req)
{
    WaterSnapshot snapshot;
    water_get_snapshot(&snapshot);
    char clock[8];
    if (snapshot.clock_valid) {
        snprintf(clock, sizeof(clock), "%02d:%02d", snapshot.hour, snapshot.minute);
    } else {
        snprintf(clock, sizeof(clock), "--:--");
    }
    char body[384];
    snprintf(body,
             sizeof(body),
             "{\"mode\":\"water\",\"clock\":\"%s\",\"next_in_min\":%d,\"next_in_sec\":%d,"
             "\"interval\":%d,\"start\":\"%02d:%02d\",\"end\":\"%02d:%02d\","
             "\"alerting\":%s,\"audio_available\":%s,\"audio_playing\":%s,\"drinks_today\":%d}",
             clock,
             (snapshot.next_in_sec + 59) / 60,
             snapshot.next_in_sec,
             snapshot.interval_min,
             snapshot.active_start_min / 60,
             snapshot.active_start_min % 60,
             snapshot.active_end_min / 60,
             snapshot.active_end_min % 60,
             snapshot.alerting ? "true" : "false",
             snapshot.audio_available ? "true" : "false",
             snapshot.audio_playing ? "true" : "false",
             snapshot.drinks_today);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t diag_display_handler(httpd_req_t *req)
{
    const size_t query_len = httpd_req_get_url_query_len(req);
    char query[128] = {};
    char pattern[32] = "orientation";
    if (query_len > 0 && query_len < sizeof(query) &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        query_arg(query, "pattern", pattern, sizeof(pattern));
    }
    if (strcmp(pattern, "clear") == 0) {
        music_clear_full_frame();
        return httpd_resp_sendstr(req, "cleared");
    }
    if (strcmp(pattern, "orientation") != 0 && strcmp(pattern, "polarity") != 0 &&
        strcmp(pattern, "timing") != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pattern must be orientation, polarity, timing, or clear");
    }
    uint8_t *frame = (uint8_t *)heap_caps_malloc(MUSIC_DISPLAY_BYTES, MALLOC_CAP_8BIT);
    if (frame == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    build_diag_frame(frame, pattern);
    const bool ok = music_set_full_frame(frame, MUSIC_DISPLAY_BYTES, 0);
    free(frame);
    if (!ok) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to set frame");
    }
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t http_api_start(void)
{
    if (g_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;

    esp_err_t err = httpd_start(&g_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t nowplaying_get = {
        .uri = "/nowplaying",
        .method = HTTP_GET,
        .handler = nowplaying_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t nowplaying_post = nowplaying_get;
    nowplaying_post.method = HTTP_POST;

    httpd_uri_t usage = {
        .uri = "/usage.json",
        .method = HTTP_GET,
        .handler = usage_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t mode_get = {
        .uri = "/mode",
        .method = HTTP_GET,
        .handler = mode_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t mode_post = mode_get;
    mode_post.method = HTTP_POST;

    httpd_uri_t hydrate_now_get = {
        .uri = "/hydrate/now",
        .method = HTTP_GET,
        .handler = hydrate_now_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t hydrate_now_post = hydrate_now_get;
    hydrate_now_post.method = HTTP_POST;

    httpd_uri_t hydrate_log_get = {
        .uri = "/hydrate/log",
        .method = HTTP_GET,
        .handler = hydrate_log_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t hydrate_log_post = hydrate_log_get;
    hydrate_log_post.method = HTTP_POST;

    httpd_uri_t hydrate_snooze_get = {
        .uri = "/hydrate/snooze",
        .method = HTTP_GET,
        .handler = hydrate_snooze_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t hydrate_snooze_post = hydrate_snooze_get;
    hydrate_snooze_post.method = HTTP_POST;

    httpd_uri_t hydrate_config_get = {
        .uri = "/hydrate/config",
        .method = HTTP_GET,
        .handler = hydrate_config_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t hydrate_config_post = hydrate_config_get;
    hydrate_config_post.method = HTTP_POST;

    httpd_uri_t hydrate_json = {
        .uri = "/hydrate.json",
        .method = HTTP_GET,
        .handler = hydrate_json_handler,
        .user_ctx = NULL,
    };

    httpd_uri_t diag_display = {
        .uri = "/diag/display",
        .method = HTTP_GET,
        .handler = diag_display_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &nowplaying_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &nowplaying_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &usage));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &mode_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &mode_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &hydrate_now_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &hydrate_now_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &hydrate_log_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &hydrate_log_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &hydrate_snooze_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &hydrate_snooze_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &hydrate_config_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &hydrate_config_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &hydrate_json));
    ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &diag_display));

    ESP_LOGI(TAG, "HTTP API listening on port %d", config.server_port);
    return ESP_OK;
}
