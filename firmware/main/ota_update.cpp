#include "ota_update.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "ota_update";

static const char OTA_NVS_NAMESPACE[] = "ota";
static const char OTA_NVS_TOKEN_KEY[] = "token";
static const char OTA_TOKEN_HEADER[] = "X-OTA-Token";

// Same shape and alphabet as the provisioning setup password in
// network_manager.cpp -- ~59 bits, and no characters that misread off a
// serial log. A dedicated token rather than reusing that password, because
// load_or_create_setup_password(true) rotates that one on every network
// reset, which would silently break OTA.
static const int OTA_TOKEN_LENGTH = 12;
static const char OTA_TOKEN_ALPHABET[] = "2346789ABCDEFGHJKLMNPQRTUVWXYZ";

// Read in chunks off the heap; a buffer this size has no business on the
// httpd handler stack.
static const size_t OTA_CHUNK_BYTES = 4096;

// Give the response time to reach the client before the board drops off the
// network to reboot into the new image.
static const int64_t OTA_REBOOT_DELAY_US = 1200000LL;

static char g_token[OTA_TOKEN_LENGTH + 1];
static bool g_update_in_progress;

static void generate_token(char *output)
{
    for (int i = 0; i < OTA_TOKEN_LENGTH; i++) {
        output[i] = OTA_TOKEN_ALPHABET[esp_random() % (sizeof(OTA_TOKEN_ALPHABET) - 1)];
    }
    output[OTA_TOKEN_LENGTH] = '\0';
}

esp_err_t ota_update_init(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    char token[OTA_TOKEN_LENGTH + 1] = {};
    size_t length = sizeof(token);
    if (nvs_get_str(handle, OTA_NVS_TOKEN_KEY, token, &length) != ESP_OK ||
        strlen(token) != (size_t)OTA_TOKEN_LENGTH) {
        generate_token(token);
        err = nvs_set_str(handle, OTA_NVS_TOKEN_KEY, token);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not persist OTA token: %s", esp_err_to_name(err));
        return err;
    }

    memcpy(g_token, token, sizeof(g_token));

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running from partition '%s'", running != NULL ? running->label : "?");
    ESP_LOGI(TAG, "OTA upload token: %s", g_token);
    return ESP_OK;
}

// Length-checked and non-short-circuiting, so a wrong token takes the same
// time to reject regardless of how much of it is right.
static bool token_matches(const char *candidate)
{
    if (candidate == NULL || g_token[0] == '\0') {
        return false;
    }
    if (strlen(candidate) != (size_t)OTA_TOKEN_LENGTH) {
        return false;
    }
    unsigned char diff = 0;
    for (int i = 0; i < OTA_TOKEN_LENGTH; i++) {
        diff |= (unsigned char)(candidate[i] ^ g_token[i]);
    }
    return diff == 0;
}

static void reboot_timer_cb(void *arg)
{
    ESP_LOGW(TAG, "rebooting into the newly written partition");
    esp_restart();
}

static void schedule_reboot(void)
{
    esp_timer_create_args_t args = {};
    args.callback = reboot_timer_cb;
    args.name = "ota_reboot";
    esp_timer_handle_t timer = NULL;
    if (esp_timer_create(&args, &timer) != ESP_OK ||
        esp_timer_start_once(timer, OTA_REBOOT_DELAY_US) != ESP_OK) {
        ESP_LOGW(TAG, "reboot timer unavailable, restarting immediately");
        esp_restart();
    }
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *desc = esp_app_get_description();

    char body[320];
    const int written = snprintf(body,
                                 sizeof(body),
                                 "{\"running\":\"%s\",\"next\":\"%s\",\"version\":\"%s\","
                                 "\"idf\":\"%s\",\"built\":\"%s %s\",\"capacity\":%u,"
                                 "\"busy\":%s}",
                                 running != NULL ? running->label : "?",
                                 next != NULL ? next->label : "?",
                                 desc != NULL ? desc->version : "?",
                                 desc != NULL ? desc->idf_ver : "?",
                                 desc != NULL ? desc->date : "?",
                                 desc != NULL ? desc->time : "?",
                                 next != NULL ? (unsigned)next->size : 0U,
                                 g_update_in_progress ? "true" : "false");
    if (written < 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status unavailable");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    char token[OTA_TOKEN_LENGTH + 8] = {};
    if (httpd_req_get_hdr_value_str(req, OTA_TOKEN_HEADER, token, sizeof(token)) != ESP_OK ||
        !token_matches(token)) {
        memset(token, 0, sizeof(token));
        ESP_LOGW(TAG, "rejected OTA upload with a missing or wrong token");
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "send header X-OTA-Token");
    }
    memset(token, 0, sizeof(token));

    if (req->content_len == 0) {
        // Auth probe. Rejecting a real upload means answering 401 without
        // draining the megabyte still in flight, which the client sees as a
        // broken pipe rather than as "wrong token" -- so uploaders check the
        // token with an empty POST first, where the answer is unambiguous.
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":true,\"probe\":true}", HTTPD_RESP_USE_STRLEN);
    }

    if (g_update_in_progress) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "an update is already running");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition available");
    }
    if (req->content_len > target->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image is larger than the OTA partition");
    }

    char *buffer = (char *)malloc(OTA_CHUNK_BYTES);
    if (buffer == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &ota);
    if (err != ESP_OK) {
        free(buffer);
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    g_update_in_progress = true;
    ESP_LOGI(TAG, "receiving %d bytes into '%s'", req->content_len, target->label);

    int remaining = req->content_len;
    while (remaining > 0) {
        const int want = remaining < (int)OTA_CHUNK_BYTES ? remaining : (int)OTA_CHUNK_BYTES;
        const int received = httpd_req_recv(req, buffer, want);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            // The render task holds the SPI bus in bursts, so a stall mid
            // upload is normal rather than fatal -- keep waiting.
            continue;
        }
        if (received <= 0) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "socket error %d with %d bytes left", received, remaining);
            break;
        }
        err = esp_ota_write(ota, buffer, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            break;
        }
        remaining -= received;
    }
    free(buffer);

    if (err != ESP_OK) {
        esp_ota_abort(ota);
        g_update_in_progress = false;
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "upload failed, image discarded");
    }

    // Validates the image header and checksum; a truncated or corrupt upload
    // is rejected here, before anything is pointed at it.
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        g_update_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_end rejected the image: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "image failed validation");
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        g_update_in_progress = false;
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "image accepted, next boot runs '%s'", target->label);
    httpd_resp_set_type(req, "application/json");
    char body[96];
    snprintf(body, sizeof(body), "{\"ok\":true,\"booting\":\"%s\",\"rebooting_in_ms\":%d}",
             target->label, (int)(OTA_REBOOT_DELAY_US / 1000));
    const esp_err_t send_err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);

    g_update_in_progress = false;
    schedule_reboot();
    return send_err;
}

esp_err_t ota_update_register(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_uri_t status = {
        .uri = "/ota",
        .method = HTTP_GET,
        .handler = ota_status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t upload = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_upload_handler,
        .user_ctx = NULL,
    };

    esp_err_t err = httpd_register_uri_handler(server, &status);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(server, &upload);
    }
    return err;
}
