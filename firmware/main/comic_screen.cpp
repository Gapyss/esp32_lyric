#include "comic_screen.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "pngle.h"

static const char *TAG = "comic_screen";
static const char *XKCD_API_URL = "https://xkcd.com/info.0.json";
#ifndef NASA_API_KEY
#define NASA_API_KEY "DEMO_KEY"
#endif
static const char *NASA_APOD_API_URL =
    "https://api.nasa.gov/planetary/apod?api_key=" NASA_API_KEY "&thumbs=true";
static const int HTTP_TIMEOUT_MS = 15000;
static const int64_t AUTO_REFRESH_MS = 6LL * 60LL * 60LL * 1000LL;
static const size_t MAX_API_BYTES = 16384;
static const size_t MAX_PNG_BYTES = 8U * 1024U * 1024U;
static const size_t MAX_JPEG_BYTES = 4U * 1024U * 1024U;
static const size_t HTTP_BUFFER_BYTES = 4096;
static const int VIEWPORT_W = 384;
static const int VIEWPORT_H = 238;
static const int COMIC_CANVAS_MAX_W = 720;
static const size_t VIEWPORT_BYTES = (VIEWPORT_W / 8) * VIEWPORT_H;
static const uint32_t REFRESH_COMIC_BIT = BIT0;
static const uint32_t REFRESH_APOD_BIT = BIT1;

typedef enum {
    COMIC_EMPTY,
    COMIC_LOADING,
    COMIC_READY,
    COMIC_ERROR,
} ComicStatus;

typedef struct {
    SemaphoreHandle_t mutex;
    uint8_t *bitmap;
    int bitmap_width;
    int bitmap_height;
    int comic_number;
    char title[128];
    char error[64];
    ComicStatus status;
} ComicState;

typedef struct {
    SemaphoreHandle_t mutex;
    uint8_t *bitmap;
    int bitmap_width;
    int bitmap_height;
    char date[16];
    char title[128];
    char error[64];
    ComicStatus status;
} ApodState;

typedef struct {
    uint8_t *bitmap;
    int src_width;
    int src_height;
    int dst_width;
    int dst_height;
    int row_bytes;
    int max_width;
    int max_height;
    bool initialized;
    bool invalid_dimensions;
    bool done;
} DecodeContext;

static ComicState g_comic;
static ApodState g_apod;
static TaskHandle_t g_daily_task;
static uint8_t *g_comic_viewport;
static int64_t g_comic_view_started_us;
static int64_t g_comic_last_render_us;

static void *alloc_external(size_t bytes)
{
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory == NULL ? malloc(bytes) : memory;
}

static uint8_t *alloc_bitmap(size_t bytes)
{
    uint8_t *bitmap = (uint8_t *)heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bitmap == NULL) {
        bitmap = (uint8_t *)calloc(1, bytes);
    }
    return bitmap;
}

static void set_comic_status(ComicStatus status, const char *error)
{
    xSemaphoreTake(g_comic.mutex, portMAX_DELAY);
    g_comic.status = status;
    snprintf(g_comic.error, sizeof(g_comic.error), "%s", error == NULL ? "" : error);
    xSemaphoreGive(g_comic.mutex);
}

static void set_apod_status(ComicStatus status, const char *error)
{
    xSemaphoreTake(g_apod.mutex, portMAX_DELAY);
    g_apod.status = status;
    snprintf(g_apod.error, sizeof(g_apod.error), "%s", error == NULL ? "" : error);
    xSemaphoreGive(g_apod.mutex);
}

static esp_err_t open_https(const char *url, esp_http_client_handle_t *client, int64_t *content_length)
{
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.buffer_size = 2048;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    *client = esp_http_client_init(&config);
    if (*client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(*client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(*client);
        *client = NULL;
        return err;
    }

    *content_length = esp_http_client_fetch_headers(*client);
    const int status = esp_http_client_get_status_code(*client);
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s returned HTTP %d", url, status);
        esp_http_client_close(*client);
        esp_http_client_cleanup(*client);
        *client = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t fetch_json(const char *url, char *json, size_t json_size)
{
    esp_http_client_handle_t client = NULL;
    int64_t content_length = -1;
    esp_err_t err = open_https(url, &client, &content_length);
    if (err != ESP_OK) {
        return err;
    }
    if (content_length >= (int64_t)json_size || content_length > (int64_t)MAX_API_BYTES) {
        err = ESP_ERR_INVALID_SIZE;
    }

    size_t used = 0;
    while (err == ESP_OK) {
        const int read = esp_http_client_read(client, json + used, json_size - used - 1);
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        used += (size_t)read;
        if (used + 1 >= json_size || used > MAX_API_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
    }
    json[used] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t download_binary(const char *url, size_t max_bytes, uint8_t **data, size_t *data_size)
{
    *data = NULL;
    *data_size = 0;
    esp_http_client_handle_t client = NULL;
    int64_t content_length = -1;
    esp_err_t err = open_https(url, &client, &content_length);
    if (err != ESP_OK) {
        return err;
    }
    if (content_length > (int64_t)max_bytes) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t capacity = content_length > 0 ? (size_t)content_length : max_bytes;
    uint8_t *buffer = (uint8_t *)alloc_external(capacity);
    if (buffer == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t used = 0;
    while (used < capacity) {
        const int read = esp_http_client_read(client, (char *)buffer + used, capacity - used);
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        used += (size_t)read;
    }
    if (err == ESP_OK && used == capacity && content_length < 0) {
        char extra;
        if (esp_http_client_read(client, &extra, 1) > 0) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || used == 0) {
        free(buffer);
        return err == ESP_OK ? ESP_ERR_INVALID_RESPONSE : err;
    }
    *data = buffer;
    *data_size = used;
    return ESP_OK;
}

static void init_bitmap(DecodeContext *ctx, int width, int height)
{
    if (width <= 0 || height <= 0 || width > 10000 || height > 10000) {
        ctx->invalid_dimensions = true;
        return;
    }
    ctx->src_width = width;
    ctx->src_height = height;
    if (width <= ctx->max_width && height <= ctx->max_height) {
        ctx->dst_width = width;
        ctx->dst_height = height;
    } else if ((uint64_t)ctx->max_width * height <= (uint64_t)ctx->max_height * width) {
        ctx->dst_width = ctx->max_width;
        ctx->dst_height = (int)((uint64_t)height * ctx->max_width / width);
    } else {
        ctx->dst_height = ctx->max_height;
        ctx->dst_width = (int)((uint64_t)width * ctx->max_height / height);
    }
    if (ctx->dst_width < 1) ctx->dst_width = 1;
    if (ctx->dst_height < 1) ctx->dst_height = 1;
    ctx->row_bytes = (ctx->dst_width + 7) / 8;
    ctx->bitmap = alloc_bitmap((size_t)ctx->row_bytes * (size_t)ctx->dst_height);
    ctx->initialized = ctx->bitmap != NULL;
}

static void png_init(pngle_t *png, uint32_t width, uint32_t height)
{
    DecodeContext *ctx = (DecodeContext *)pngle_get_user_data(png);
    init_bitmap(ctx, (int)width, (int)height);
}

static void png_draw(pngle_t *png,
                     uint32_t x,
                     uint32_t y,
                     uint32_t width,
                     uint32_t height,
                     const uint8_t rgba[4])
{
    DecodeContext *ctx = (DecodeContext *)pngle_get_user_data(png);
    if (!ctx->initialized) {
        return;
    }

    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    const uint32_t x_end = x + width > (uint32_t)ctx->src_width ? (uint32_t)ctx->src_width : x + width;
    const uint32_t y_end = y + height > (uint32_t)ctx->src_height ? (uint32_t)ctx->src_height : y + height;
    const int luminance = (77 * rgba[0] + 150 * rgba[1] + 29 * rgba[2]) >> 8;
    const int blended = (luminance * rgba[3] + 255 * (255 - rgba[3])) / 255;

    // Select destination pixels whose nearest source-pixel center falls in
    // this callback's region. This avoids repeatedly overwriting every output
    // pixel while reducing a large source image.
    int dx0 = (int)((2ULL * x * ctx->dst_width + ctx->src_width - 1) / (2 * ctx->src_width));
    int dx1 = (int)((2ULL * x_end * ctx->dst_width + ctx->src_width - 1) / (2 * ctx->src_width));
    int dy0 = (int)((2ULL * y * ctx->dst_height + ctx->src_height - 1) / (2 * ctx->src_height));
    int dy1 = (int)((2ULL * y_end * ctx->dst_height + ctx->src_height - 1) / (2 * ctx->src_height));
    if (dx1 > ctx->dst_width) dx1 = ctx->dst_width;
    if (dy1 > ctx->dst_height) dy1 = ctx->dst_height;

    for (int dy = dy0; dy < dy1; dy++) {
        for (int dx = dx0; dx < dx1; dx++) {
            uint8_t *byte = &ctx->bitmap[dy * ctx->row_bytes + dx / 8];
            const uint8_t mask = (uint8_t)(1U << (dx & 7));
            const int threshold = bayer[dy & 3][dx & 3] * 16 + 8;
            if (blended < threshold) {
                *byte |= mask;
            } else {
                *byte &= (uint8_t)~mask;
            }
        }
    }
}

static void png_done(pngle_t *png)
{
    DecodeContext *ctx = (DecodeContext *)pngle_get_user_data(png);
    ctx->done = true;
}

static esp_err_t fetch_png(const char *url, DecodeContext *ctx)
{
    esp_http_client_handle_t client = NULL;
    int64_t content_length = -1;
    esp_err_t err = open_https(url, &client, &content_length);
    if (err != ESP_OK) {
        return err;
    }
    if (content_length > (int64_t)MAX_PNG_BYTES) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    pngle_t *png = pngle_new();
    if (png == NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    pngle_set_user_data(png, ctx);
    pngle_set_init_callback(png, png_init);
    pngle_set_draw_callback(png, png_draw);
    pngle_set_done_callback(png, png_done);

    uint8_t *buffer = (uint8_t *)alloc_external(HTTP_BUFFER_BYTES);
    if (buffer == NULL) {
        pngle_destroy(png);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    size_t remain = 0;
    size_t total = 0;
    while (err == ESP_OK && !ctx->done) {
        const int read = esp_http_client_read(client, (char *)buffer + remain, HTTP_BUFFER_BYTES - remain);
        if (read < 0) {
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            break;
        }
        total += (size_t)read;
        if (total > MAX_PNG_BYTES) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        const size_t available = remain + (size_t)read;
        const int consumed = pngle_feed(png, buffer, available);
        if (consumed < 0) {
            ESP_LOGW(TAG, "PNG decode failed: %s", pngle_error(png));
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        remain = available - (size_t)consumed;
        if (remain > 0 && consumed > 0) {
            memmove(buffer, buffer + consumed, remain);
        }
        if (remain == HTTP_BUFFER_BYTES) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
    }
    if (err == ESP_OK && (!ctx->done || !ctx->initialized)) {
        if (ctx->invalid_dimensions) {
            err = ESP_ERR_INVALID_SIZE;
        } else {
            err = ctx->initialized ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_NO_MEM;
        }
    }
    free(buffer);
    pngle_destroy(png);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static int rgb565_luminance(uint16_t color)
{
    const int r = ((color >> 11) & 0x1F) * 255 / 31;
    const int g = ((color >> 5) & 0x3F) * 255 / 63;
    const int b = (color & 0x1F) * 255 / 31;
    return (77 * r + 150 * g + 29 * b) >> 8;
}

static esp_err_t decode_jpeg(const uint8_t *jpeg, size_t jpeg_size, DecodeContext *ctx)
{
    esp_jpeg_image_cfg_t config = {};
    config.indata = (uint8_t *)jpeg;
    config.indata_size = jpeg_size;
    config.out_format = JPEG_IMAGE_FORMAT_RGB565;

    esp_jpeg_image_output_t info = {};
    esp_err_t err = esp_jpeg_get_image_info(&config, &info);
    if (err != ESP_OK || info.width == 0 || info.height == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    while (config.out_scale < JPEG_IMAGE_SCALE_1_8 &&
           ((info.width >> config.out_scale) > 768 || (info.height >> config.out_scale) > 476)) {
        config.out_scale = (esp_jpeg_image_scale_t)(config.out_scale + 1);
    }
    err = esp_jpeg_get_image_info(&config, &info);
    if (err != ESP_OK || info.output_len == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t *rgb = (uint8_t *)alloc_external(info.output_len);
    if (rgb == NULL) {
        return ESP_ERR_NO_MEM;
    }
    config.outbuf = rgb;
    config.outbuf_size = info.output_len;
    esp_jpeg_image_output_t decoded = {};
    err = esp_jpeg_decode(&config, &decoded);
    if (err != ESP_OK) {
        free(rgb);
        return err;
    }

    init_bitmap(ctx, decoded.width, decoded.height);
    if (!ctx->initialized) {
        free(rgb);
        return ctx->invalid_dimensions ? ESP_ERR_INVALID_SIZE : ESP_ERR_NO_MEM;
    }

    uint32_t histogram[256] = {};
    const uint16_t *pixels = (const uint16_t *)rgb;
    const size_t pixel_count = (size_t)decoded.width * decoded.height;
    for (size_t i = 0; i < pixel_count; i++) {
        histogram[rgb565_luminance(pixels[i])]++;
    }
    const uint32_t low_target = (uint32_t)(pixel_count / 50);
    const uint32_t high_target = (uint32_t)(pixel_count - pixel_count / 50);
    uint32_t cumulative = 0;
    int low = 0;
    int high = 255;
    for (int i = 0; i < 256; i++) {
        cumulative += histogram[i];
        if (cumulative >= low_target) {
            low = i;
            break;
        }
    }
    cumulative = 0;
    for (int i = 0; i < 256; i++) {
        cumulative += histogram[i];
        if (cumulative >= high_target) {
            high = i;
            break;
        }
    }
    if (high - low < 32) {
        low = 0;
        high = 255;
    }

    static const uint8_t bayer[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    for (int dy = 0; dy < ctx->dst_height; dy++) {
        const int sy = (int)(((2ULL * dy + 1) * decoded.height) / (2 * ctx->dst_height));
        for (int dx = 0; dx < ctx->dst_width; dx++) {
            const int sx = (int)(((2ULL * dx + 1) * decoded.width) / (2 * ctx->dst_width));
            int luminance = rgb565_luminance(pixels[sy * decoded.width + sx]);
            luminance = (luminance - low) * 255 / (high - low);
            if (luminance < 0) luminance = 0;
            if (luminance > 255) luminance = 255;
            const int threshold = bayer[dy & 3][dx & 3] * 16 + 8;
            if (luminance < threshold) {
                ctx->bitmap[dy * ctx->row_bytes + dx / 8] |= (uint8_t)(1U << (dx & 7));
            }
        }
    }
    free(rgb);
    ctx->done = true;
    return ESP_OK;
}

static esp_err_t fetch_jpeg(const char *url, DecodeContext *ctx)
{
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    esp_err_t err = download_binary(url, MAX_JPEG_BYTES, &jpeg, &jpeg_size);
    if (err == ESP_OK) {
        err = decode_jpeg(jpeg, jpeg_size, ctx);
    }
    free(jpeg);
    return err;
}

static esp_err_t refresh_comic(void)
{
    char *json = (char *)alloc_external(MAX_API_BYTES + 1);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = fetch_json(XKCD_API_URL, json, MAX_API_BYTES + 1);
    if (err != ESP_OK) {
        free(json);
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *number = cJSON_GetObjectItemCaseSensitive(root, "num");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "safe_title");
    const cJSON *image = cJSON_GetObjectItemCaseSensitive(root, "img");
    if (!cJSON_IsNumber(number) || !cJSON_IsString(title) || !cJSON_IsString(image) ||
        title->valuestring == NULL || image->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const int comic_number = number->valueint;
    char comic_title[sizeof(g_comic.title)];
    char image_url[384];
    snprintf(comic_title, sizeof(comic_title), "%s", title->valuestring);
    snprintf(image_url, sizeof(image_url), "%s", image->valuestring);
    cJSON_Delete(root);
    if (strncmp(image_url, "https://", strlen("https://")) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    DecodeContext decoded = {};
    decoded.max_width = COMIC_CANVAS_MAX_W;
    decoded.max_height = VIEWPORT_H;
    err = fetch_png(image_url, &decoded);
    if (err != ESP_OK) {
        free(decoded.bitmap);
        return err;
    }

    xSemaphoreTake(g_comic.mutex, portMAX_DELAY);
    free(g_comic.bitmap);
    g_comic.bitmap = decoded.bitmap;
    g_comic.bitmap_width = decoded.dst_width;
    g_comic.bitmap_height = decoded.dst_height;
    g_comic.comic_number = comic_number;
    snprintf(g_comic.title, sizeof(g_comic.title), "%s", comic_title);
    g_comic.error[0] = '\0';
    g_comic.status = COMIC_READY;
    xSemaphoreGive(g_comic.mutex);
    ESP_LOGI(TAG, "loaded xkcd #%d: %s (%dx%d)", comic_number, comic_title, decoded.dst_width, decoded.dst_height);
    return ESP_OK;
}

static esp_err_t refresh_apod(void)
{
    char *json = (char *)alloc_external(MAX_API_BYTES + 1);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = fetch_json(NASA_APOD_API_URL, json, MAX_API_BYTES + 1);
    if (err != ESP_OK) {
        free(json);
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *media_type = cJSON_GetObjectItemCaseSensitive(root, "media_type");
    const cJSON *title = cJSON_GetObjectItemCaseSensitive(root, "title");
    const cJSON *date = cJSON_GetObjectItemCaseSensitive(root, "date");
    const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (cJSON_IsString(media_type) && strcmp(media_type->valuestring, "video") == 0) {
        url = cJSON_GetObjectItemCaseSensitive(root, "thumbnail_url");
    }
    if (!cJSON_IsString(title) || !cJSON_IsString(date) || !cJSON_IsString(url) ||
        title->valuestring == NULL || date->valuestring == NULL || url->valuestring == NULL) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    char apod_title[sizeof(g_apod.title)];
    char apod_date[sizeof(g_apod.date)];
    char image_url[512];
    snprintf(apod_title, sizeof(apod_title), "%s", title->valuestring);
    snprintf(apod_date, sizeof(apod_date), "%s", date->valuestring);
    snprintf(image_url, sizeof(image_url), "%s", url->valuestring);
    cJSON_Delete(root);
    if (strncmp(image_url, "http://apod.nasa.gov/", strlen("http://apod.nasa.gov/")) == 0) {
        const size_t url_length = strlen(image_url);
        if (url_length + 1 >= sizeof(image_url)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memmove(image_url + 5, image_url + 4, url_length - 3);
        image_url[4] = 's';
    }
    if (strncmp(image_url, "https://", strlen("https://")) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    DecodeContext decoded = {};
    decoded.max_width = VIEWPORT_W;
    decoded.max_height = VIEWPORT_H;
    if (strstr(image_url, ".png") != NULL) {
        err = fetch_png(image_url, &decoded);
    } else {
        err = fetch_jpeg(image_url, &decoded);
    }
    if (err != ESP_OK) {
        free(decoded.bitmap);
        return err;
    }

    xSemaphoreTake(g_apod.mutex, portMAX_DELAY);
    free(g_apod.bitmap);
    g_apod.bitmap = decoded.bitmap;
    g_apod.bitmap_width = decoded.dst_width;
    g_apod.bitmap_height = decoded.dst_height;
    snprintf(g_apod.date, sizeof(g_apod.date), "%s", apod_date);
    snprintf(g_apod.title, sizeof(g_apod.title), "%s", apod_title);
    g_apod.error[0] = '\0';
    g_apod.status = COMIC_READY;
    xSemaphoreGive(g_apod.mutex);
    ESP_LOGI(TAG, "loaded APOD %s: %s (%dx%d)", apod_date, apod_title, decoded.dst_width, decoded.dst_height);
    return ESP_OK;
}

static void comic_task(void *arg)
{
    (void)arg;
    uint32_t pending = REFRESH_COMIC_BIT | REFRESH_APOD_BIT;
    while (true) {
        if ((pending & REFRESH_COMIC_BIT) != 0) {
            set_comic_status(COMIC_LOADING, NULL);
            const esp_err_t err = refresh_comic();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "comic refresh failed: %s", esp_err_to_name(err));
                set_comic_status(COMIC_ERROR, esp_err_to_name(err));
            }
        }
        if ((pending & REFRESH_APOD_BIT) != 0) {
            set_apod_status(COMIC_LOADING, NULL);
            const esp_err_t err = refresh_apod();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "APOD refresh failed: %s", esp_err_to_name(err));
                set_apod_status(COMIC_ERROR, esp_err_to_name(err));
            }
        }
        ESP_LOGI(TAG, "daily image stack reserve: %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        pending = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &pending, pdMS_TO_TICKS(AUTO_REFRESH_MS)) != pdTRUE) {
            pending = REFRESH_COMIC_BIT | REFRESH_APOD_BIT;
        }
    }
}

void comic_screen_init(void)
{
    memset(&g_comic, 0, sizeof(g_comic));
    memset(&g_apod, 0, sizeof(g_apod));
    g_comic.mutex = xSemaphoreCreateMutex();
    g_apod.mutex = xSemaphoreCreateMutex();
    g_comic_viewport = alloc_bitmap(VIEWPORT_BYTES);
}

void comic_screen_start(void)
{
    if (g_daily_task != NULL) {
        return;
    }
    const BaseType_t ok = xTaskCreate(comic_task, "daily_images", 12288, NULL, 4, &g_daily_task);
    if (ok != pdPASS) {
        g_daily_task = NULL;
        set_comic_status(COMIC_ERROR, "NO MEMORY");
        set_apod_status(COMIC_ERROR, "NO MEMORY");
    }
}

void comic_refresh(void)
{
    if (g_daily_task != NULL) {
        xTaskNotify(g_daily_task, REFRESH_COMIC_BIT, eSetBits);
    }
}

void apod_refresh(void)
{
    if (g_daily_task != NULL) {
        xTaskNotify(g_daily_task, REFRESH_APOD_BIT, eSetBits);
    }
}

static void draw_centered(u8g2_t *u8, const char *text, int baseline)
{
    const int width = (int)u8g2_GetDisplayWidth(u8);
    const int text_width = (int)u8g2_GetUTF8Width(u8, text);
    u8g2_DrawUTF8(u8, (width - text_width) / 2, baseline, text);
}

static void fit_title(u8g2_t *u8, char *title, int max_width)
{
    if ((int)u8g2_GetUTF8Width(u8, title) <= max_width) {
        return;
    }
    size_t len = strlen(title);
    while (len > 0) {
        size_t codepoint_start = len - 1;
        while (codepoint_start > 0 && ((uint8_t)title[codepoint_start] & 0xC0) == 0x80) {
            codepoint_start--;
        }
        len = codepoint_start;
        title[len] = '\0';
        char candidate[sizeof(g_comic.title)];
        snprintf(candidate, sizeof(candidate), "%s...", title);
        if ((int)u8g2_GetUTF8Width(u8, candidate) <= max_width) {
            snprintf(title, sizeof(g_comic.title), "%s", candidate);
            return;
        }
    }
    snprintf(title, sizeof(g_comic.title), "...");
}

static int comic_pan_offset(int bitmap_width, int64_t now_us)
{
    const int overflow = bitmap_width - VIEWPORT_W;
    if (overflow <= 0) {
        return 0;
    }
    if (g_comic_view_started_us == 0 || now_us - g_comic_last_render_us > 500000LL) {
        g_comic_view_started_us = now_us;
    }
    g_comic_last_render_us = now_us;
    const int64_t hold_us = 2000000LL;
    const int64_t step_us = 80000LL;
    const int64_t travel_us = (int64_t)overflow * step_us;
    const int64_t cycle_us = 2 * hold_us + 2 * travel_us;
    int64_t phase = (now_us - g_comic_view_started_us) % cycle_us;
    if (phase < hold_us) {
        return 0;
    }
    phase -= hold_us;
    if (phase < travel_us) {
        return (int)(phase / step_us);
    }
    phase -= travel_us;
    if (phase < hold_us) {
        return overflow;
    }
    phase -= hold_us;
    return overflow - (int)(phase / step_us);
}

static bool prepare_comic_viewport(const uint8_t *source, int width, int height, int x_offset)
{
    if (g_comic_viewport == NULL || source == NULL || width < VIEWPORT_W || height <= 0) {
        return false;
    }
    memset(g_comic_viewport, 0, VIEWPORT_BYTES);
    const int source_row_bytes = (width + 7) / 8;
    const int source_byte_offset = x_offset / 8;
    const int shift = x_offset & 7;
    for (int y = 0; y < height; y++) {
        const uint8_t *source_row = source + y * source_row_bytes;
        uint8_t *target_row = g_comic_viewport + y * (VIEWPORT_W / 8);
        for (int byte_x = 0; byte_x < VIEWPORT_W / 8; byte_x++) {
            const int source_index = source_byte_offset + byte_x;
            uint16_t bits = source_row[source_index];
            if (shift != 0 && source_index + 1 < source_row_bytes) {
                bits |= (uint16_t)source_row[source_index + 1] << 8;
            }
            target_row[byte_x] = (uint8_t)(bits >> shift);
        }
    }
    return true;
}

void comic_render_current(u8g2_t *u8)
{
    u8g2_SetDrawColor(u8, 1);
    u8g2_DrawBox(u8, 0, 0, u8g2_GetDisplayWidth(u8), u8g2_GetDisplayHeight(u8));
    u8g2_SetDrawColor(u8, 0);
    u8g2_SetFontMode(u8, 1);

    xSemaphoreTake(g_comic.mutex, portMAX_DELAY);
    const bool has_bitmap = g_comic.bitmap != NULL;
    if (has_bitmap) {
        const int y = 25 + (VIEWPORT_H - g_comic.bitmap_height) / 2;
        if (g_comic.bitmap_width <= VIEWPORT_W) {
            const int x = ((int)u8g2_GetDisplayWidth(u8) - g_comic.bitmap_width) / 2;
            u8g2_DrawXBM(u8, x, y, g_comic.bitmap_width, g_comic.bitmap_height, g_comic.bitmap);
        } else {
            const int offset = comic_pan_offset(g_comic.bitmap_width, esp_timer_get_time());
            if (prepare_comic_viewport(g_comic.bitmap, g_comic.bitmap_width, g_comic.bitmap_height, offset)) {
                u8g2_DrawXBM(u8, 8, y, VIEWPORT_W, g_comic.bitmap_height, g_comic_viewport);
            }
        }
    } else {
        u8g2_DrawFrame(u8, 8, 30, 384, 225);
        u8g2_SetFont(u8, u8g2_font_helvB14_tf);
        draw_centered(u8, g_comic.status == COMIC_ERROR ? "COMIC UNAVAILABLE" : "FETCHING TODAY'S XKCD", 137);
        if (g_comic.status == COMIC_ERROR) {
            u8g2_SetFont(u8, u8g2_font_6x12_tf);
            draw_centered(u8, g_comic.error, 158);
        }
    }

    char header[32];
    snprintf(header, sizeof(header), g_comic.comic_number > 0 ? "DAILY COMIC  |  XKCD #%d" : "DAILY COMIC  |  XKCD", g_comic.comic_number);
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    u8g2_DrawUTF8(u8, 8, 14, header);
    if (g_comic.status == COMIC_LOADING && has_bitmap) {
        const char *updating = "UPDATING";
        u8g2_DrawUTF8(u8, 392 - u8g2_GetUTF8Width(u8, updating), 14, updating);
    }

    char title[sizeof(g_comic.title)];
    snprintf(title, sizeof(title), "%s", has_bitmap ? g_comic.title : "PRESS ACTION TO RETRY");
    fit_title(u8, title, 384);
    draw_centered(u8, title, 292);
    xSemaphoreGive(g_comic.mutex);
}

void apod_render_current(u8g2_t *u8)
{
    u8g2_SetDrawColor(u8, 1);
    u8g2_DrawBox(u8, 0, 0, u8g2_GetDisplayWidth(u8), u8g2_GetDisplayHeight(u8));
    u8g2_SetDrawColor(u8, 0);
    u8g2_SetFontMode(u8, 1);

    xSemaphoreTake(g_apod.mutex, portMAX_DELAY);
    const bool has_bitmap = g_apod.bitmap != NULL;
    if (has_bitmap) {
        const int x = ((int)u8g2_GetDisplayWidth(u8) - g_apod.bitmap_width) / 2;
        const int y = 25 + (VIEWPORT_H - g_apod.bitmap_height) / 2;
        u8g2_DrawXBM(u8, x, y, g_apod.bitmap_width, g_apod.bitmap_height, g_apod.bitmap);
    } else {
        u8g2_DrawFrame(u8, 8, 30, VIEWPORT_W, 225);
        u8g2_SetFont(u8, u8g2_font_helvB14_tf);
        draw_centered(u8, g_apod.status == COMIC_ERROR ? "APOD UNAVAILABLE" : "FETCHING NASA APOD", 137);
        if (g_apod.status == COMIC_ERROR) {
            u8g2_SetFont(u8, u8g2_font_6x12_tf);
            draw_centered(u8, g_apod.error, 158);
        }
    }

    char header[40];
    snprintf(header, sizeof(header), g_apod.date[0] != '\0' ? "NASA APOD  |  %s" : "NASA APOD", g_apod.date);
    u8g2_SetFont(u8, u8g2_font_6x12_tf);
    u8g2_DrawUTF8(u8, 8, 14, header);
    if (g_apod.status == COMIC_LOADING && has_bitmap) {
        const char *updating = "UPDATING";
        u8g2_DrawUTF8(u8, 392 - u8g2_GetUTF8Width(u8, updating), 14, updating);
    }

    char title[sizeof(g_apod.title)];
    snprintf(title, sizeof(title), "%s", has_bitmap ? g_apod.title : "PRESS ACTION TO RETRY");
    fit_title(u8, title, VIEWPORT_W);
    draw_centered(u8, title, 292);
    xSemaphoreGive(g_apod.mutex);
}
