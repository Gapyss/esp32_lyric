#include "board_client.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "display_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "music_screen.h"
#include "wifi_secrets.h"

#ifndef LYRICS_DAEMON_HOST
#define LYRICS_DAEMON_HOST ""
#endif

#ifndef LYRICS_DAEMON_PORT
#define LYRICS_DAEMON_PORT 8766
#endif

#ifndef LYRICS_DAEMON_TOKEN
#define LYRICS_DAEMON_TOKEN ""
#endif

static const char *TAG = "board_client";
static const size_t FRAME_ENVELOPE_HEADER_BYTES = 28;
static const uint8_t FRAME_ENVELOPE_VERSION = 1;
static const uint8_t FRAME_KIND_FULL_NOW = 1;
static const uint8_t FRAME_KIND_FULL_SCHEDULED = 2;
static const uint8_t FRAME_KIND_RECT_NOW = 3;
static const uint8_t FRAME_KIND_RECT_SCHEDULED = 4;
static const size_t MAX_TEXT_PAYLOAD = 1024;
static const int RECONNECT_DELAY_MS = 3000;
static const int MDNS_TIMEOUT_MS = 3000;

typedef struct {
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint16_t port;
    char label[96];
} DaemonEndpoint;

typedef struct {
    char type[24];
    int width;
    int height;
    int x;
    int y;
    int rect_width;
    int rect_height;
    int row_bytes;
    uint32_t bytes;
    int swap_in_ms;
    bool frame_expected;
    bool rect_expected;
} FrameHeader;

static bool recv_exact(int sock, uint8_t *dst, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const int ret = recv(sock, dst + off, len - off, 0);
        if (ret <= 0) {
            return false;
        }
        off += (size_t)ret;
    }
    return true;
}

static bool send_all(int sock, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        const int ret = send(sock, data + off, len - off, 0);
        if (ret <= 0) {
            return false;
        }
        off += (size_t)ret;
    }
    return true;
}

static bool read_http_response(int sock)
{
    char response[768] = {};
    size_t off = 0;
    while (off + 1 < sizeof(response)) {
        char c;
        const int ret = recv(sock, &c, 1, 0);
        if (ret <= 0) {
            return false;
        }
        response[off++] = c;
        response[off] = '\0';
        if (off >= 4 && strcmp(response + off - 4, "\r\n\r\n") == 0) {
            return strstr(response, " 101 ") != NULL;
        }
    }
    return false;
}

static bool websocket_handshake(int sock, const DaemonEndpoint *endpoint)
{
    char request[512];
    const char *token = LYRICS_DAEMON_TOKEN;
    const char *path = token[0] == '\0' ? "/board" : "/board?token=" LYRICS_DAEMON_TOKEN;
    snprintf(request,
             sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%u\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "Sec-WebSocket-Key: ZzRweXMtbHlyaWNzLWJvYXJk\r\n"
             "\r\n",
             path,
             endpoint->label,
             endpoint->port);

    if (!send_all(sock, (const uint8_t *)request, strlen(request))) {
        return false;
    }
    return read_http_response(sock);
}

static bool send_ws_frame(int sock, uint8_t opcode, const uint8_t *payload, size_t len)
{
    uint8_t header[14];
    size_t header_len = 0;
    header[header_len++] = 0x80 | (opcode & 0x0F);
    if (len < 126) {
        header[header_len++] = 0x80 | (uint8_t)len;
    } else if (len <= 0xFFFF) {
        header[header_len++] = 0x80 | 126;
        header[header_len++] = (uint8_t)(len >> 8);
        header[header_len++] = (uint8_t)(len & 0xFF);
    } else {
        return false;
    }

    uint8_t mask[4];
    uint32_t random = esp_random();
    memcpy(mask, &random, sizeof(mask));
    memcpy(header + header_len, mask, sizeof(mask));
    header_len += sizeof(mask);

    if (!send_all(sock, header, header_len)) {
        return false;
    }

    uint8_t scratch[128];
    size_t off = 0;
    while (off < len) {
        const size_t chunk = (len - off) < sizeof(scratch) ? (len - off) : sizeof(scratch);
        for (size_t i = 0; i < chunk; i++) {
            scratch[i] = payload[off + i] ^ mask[(off + i) & 3];
        }
        if (!send_all(sock, scratch, chunk)) {
            return false;
        }
        off += chunk;
    }
    return true;
}

static bool send_ready(int sock)
{
    static const uint8_t ready[] = "ready";
    return send_ws_frame(sock, 1, ready, sizeof(ready) - 1);
}

static bool read_ws_header(int sock, uint8_t *opcode, uint64_t *payload_len)
{
    uint8_t h[2];
    if (!recv_exact(sock, h, sizeof(h))) {
        return false;
    }
    const bool fin = (h[0] & 0x80) != 0;
    const bool masked = (h[1] & 0x80) != 0;
    if (!fin || masked) {
        return false;
    }

    *opcode = h[0] & 0x0F;
    uint64_t len = h[1] & 0x7F;
    if (len == 126) {
        uint8_t ext[2];
        if (!recv_exact(sock, ext, sizeof(ext))) {
            return false;
        }
        len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (!recv_exact(sock, ext, sizeof(ext))) {
            return false;
        }
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | ext[i];
        }
    }
    *payload_len = len;
    return true;
}

static bool discard_payload(int sock, uint64_t len)
{
    uint8_t scratch[128];
    while (len > 0) {
        const size_t chunk = len < sizeof(scratch) ? (size_t)len : sizeof(scratch);
        if (!recv_exact(sock, scratch, chunk)) {
            return false;
        }
        len -= chunk;
    }
    return true;
}

static uint16_t read_be16(const uint8_t *data)
{
    return ((uint16_t)data[0] << 8) | data[1];
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p += strlen(pattern);
    while (isspace((unsigned char)*p)) {
        p++;
    }
    *out = atoi(p);
    return true;
}

static bool json_get_string(const char *json, const char *key, char *dst, size_t dst_len)
{
    if (dst_len == 0) {
        return false;
    }
    dst[0] = '\0';
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p += strlen(pattern);
    size_t out = 0;
    while (*p != '\0' && *p != '"' && out + 1 < dst_len) {
        dst[out++] = *p++;
    }
    dst[out] = '\0';
    return true;
}

static bool parse_frame_header(const char *json, FrameHeader *header)
{
    memset(header, 0, sizeof(*header));
    header->swap_in_ms = 0;
    if (!json_get_string(json, "type", header->type, sizeof(header->type))) {
        return false;
    }
    if (strcmp(header->type, "clear") == 0 || strcmp(header->type, "hello") == 0) {
        return true;
    }
    if (strcmp(header->type, "frame") != 0 && strcmp(header->type, "frame-now") != 0 &&
        strcmp(header->type, "rect") != 0 && strcmp(header->type, "rect-now") != 0) {
        return true;
    }

    char encoding[40];
    int bytes = 0;
    if (!json_get_int(json, "width", &header->width) || !json_get_int(json, "height", &header->height) ||
        !json_get_int(json, "bytes", &bytes) ||
        !json_get_string(json, "encoding", encoding, sizeof(encoding))) {
        return false;
    }
    json_get_int(json, "swapInMs", &header->swap_in_ms);
    header->bytes = bytes < 0 ? 0 : (uint32_t)bytes;
    if (strcmp(header->type, "rect") == 0 || strcmp(header->type, "rect-now") == 0) {
        if (!json_get_int(json, "x", &header->x) || !json_get_int(json, "y", &header->y) ||
            !json_get_int(json, "rectWidth", &header->rect_width) ||
            !json_get_int(json, "rectHeight", &header->rect_height) ||
            !json_get_int(json, "rowBytes", &header->row_bytes)) {
            return false;
        }
        header->rect_expected = header->width == MUSIC_DISPLAY_WIDTH && header->height == MUSIC_DISPLAY_HEIGHT &&
                                strcmp(encoding, "1bpp-lsb-rowmajor-rect") == 0 && header->x >= 0 &&
                                header->y >= 0 && header->rect_width > 0 && header->rect_height > 0 &&
                                header->row_bytes > 0 && (header->x % 8) == 0 &&
                                header->x + header->rect_width <= MUSIC_DISPLAY_WIDTH &&
                                header->y + header->rect_height <= MUSIC_DISPLAY_HEIGHT &&
                                header->bytes == (uint32_t)(header->row_bytes * header->rect_height);
        return header->rect_expected;
    }
    header->frame_expected = header->width == MUSIC_DISPLAY_WIDTH && header->height == MUSIC_DISPLAY_HEIGHT &&
                             header->bytes == MUSIC_DISPLAY_BYTES && strcmp(encoding, "1bpp-lsb-rowmajor") == 0;
    return header->frame_expected;
}

static bool handle_text_frame(int sock, const uint8_t *payload, uint64_t len, FrameHeader *pending)
{
    if (len >= MAX_TEXT_PAYLOAD) {
        return false;
    }
    char text[MAX_TEXT_PAYLOAD];
    memcpy(text, payload, (size_t)len);
    text[len] = '\0';

    FrameHeader header;
    if (!parse_frame_header(text, &header)) {
        ESP_LOGW(TAG, "ignored unsupported frame header: %s", text);
        memset(pending, 0, sizeof(*pending));
        return true;
    }
    if (strcmp(header.type, "clear") == 0) {
        music_clear_full_frame();
        memset(pending, 0, sizeof(*pending));
    } else if (strcmp(header.type, "frame") == 0 || strcmp(header.type, "frame-now") == 0 ||
               strcmp(header.type, "rect") == 0 || strcmp(header.type, "rect-now") == 0) {
        *pending = header;
    } else if (strcmp(header.type, "hello") == 0) {
        send_ready(sock);
    }
    return true;
}

static bool handle_binary_envelope(const uint8_t *payload, uint64_t len)
{
    if (len < FRAME_ENVELOPE_HEADER_BYTES || memcmp(payload, "LYR1", 4) != 0) {
        return false;
    }
    const uint8_t version = payload[4];
    const uint8_t kind = payload[5];
    const uint16_t width = read_be16(payload + 6);
    const uint16_t height = read_be16(payload + 8);
    const uint16_t x = read_be16(payload + 10);
    const uint16_t y = read_be16(payload + 12);
    const uint16_t rect_width = read_be16(payload + 14);
    const uint16_t rect_height = read_be16(payload + 16);
    const uint16_t row_bytes = read_be16(payload + 18);
    const uint32_t swap_in_ms = read_be32(payload + 20);
    const uint32_t data_len = read_be32(payload + 24);
    const uint8_t *data = payload + FRAME_ENVELOPE_HEADER_BYTES;

    if (version != FRAME_ENVELOPE_VERSION || width != MUSIC_DISPLAY_WIDTH || height != MUSIC_DISPLAY_HEIGHT ||
        data_len != len - FRAME_ENVELOPE_HEADER_BYTES) {
        ESP_LOGW(TAG, "ignored invalid binary frame envelope");
        return true;
    }

    bool ok = false;
    if (kind == FRAME_KIND_FULL_NOW || kind == FRAME_KIND_FULL_SCHEDULED) {
        if (x != 0 || y != 0 || rect_width != MUSIC_DISPLAY_WIDTH || rect_height != MUSIC_DISPLAY_HEIGHT ||
            row_bytes != MUSIC_DISPLAY_ROW_BYTES || data_len != MUSIC_DISPLAY_BYTES) {
            ESP_LOGW(TAG, "ignored invalid full-frame envelope");
            return true;
        }
        ok = music_set_full_frame(data, data_len, swap_in_ms);
    } else if (kind == FRAME_KIND_RECT_NOW || kind == FRAME_KIND_RECT_SCHEDULED) {
        if ((x % 8) != 0 || rect_width == 0 || rect_height == 0 || row_bytes == 0 ||
            x + rect_width > MUSIC_DISPLAY_WIDTH || y + rect_height > MUSIC_DISPLAY_HEIGHT ||
            data_len != (uint32_t)(row_bytes * rect_height)) {
            ESP_LOGW(TAG, "ignored invalid rect-frame envelope");
            return true;
        }
        ok = music_set_frame_rect(data, data_len, x, y, rect_width, rect_height, row_bytes, swap_in_ms);
    } else {
        ESP_LOGW(TAG, "ignored unknown binary frame envelope kind=%u", kind);
        return true;
    }

    if (!ok) {
        ESP_LOGE(TAG, "failed to store framebuffer envelope");
    }
    return true;
}

static bool handle_binary_frame(const uint8_t *payload, uint64_t len, FrameHeader *pending)
{
    if (handle_binary_envelope(payload, len)) {
        memset(pending, 0, sizeof(*pending));
        return true;
    }
    if ((!pending->frame_expected && !pending->rect_expected) || pending->bytes != len) {
        ESP_LOGW(TAG, "unexpected binary frame len=%llu", (unsigned long long)len);
        memset(pending, 0, sizeof(*pending));
        return true;
    }
    bool ok = false;
    if (pending->rect_expected) {
        ok = music_set_frame_rect(payload,
                                  (uint32_t)len,
                                  pending->x,
                                  pending->y,
                                  pending->rect_width,
                                  pending->rect_height,
                                  pending->row_bytes,
                                  pending->swap_in_ms);
    } else {
        ok = music_set_full_frame(payload, (uint32_t)len, pending->swap_in_ms);
    }
    if (!ok) {
        ESP_LOGE(TAG, "failed to store framebuffer");
    }
    memset(pending, 0, sizeof(*pending));
    return true;
}

static bool websocket_loop(int sock)
{
    FrameHeader pending = {};
    if (!send_ready(sock)) {
        return false;
    }

    while (true) {
        uint8_t opcode = 0;
        uint64_t len = 0;
        if (!read_ws_header(sock, &opcode, &len)) {
            return false;
        }

        if (opcode == 8) {
            discard_payload(sock, len);
            return false;
        }
        if (opcode == 9) {
            uint8_t ping[125];
            if (len > sizeof(ping) || !recv_exact(sock, ping, (size_t)len)) {
                return false;
            }
            send_ws_frame(sock, 10, ping, (size_t)len);
            continue;
        }
        if (opcode == 10) {
            if (!discard_payload(sock, len)) {
                return false;
            }
            continue;
        }

        const size_t max_len = opcode == 1 ? MAX_TEXT_PAYLOAD : MUSIC_DISPLAY_BYTES + FRAME_ENVELOPE_HEADER_BYTES;
        if ((opcode != 1 && opcode != 2) || len > max_len) {
            return discard_payload(sock, len);
        }

        uint8_t *payload = (uint8_t *)heap_caps_malloc((size_t)len + 1, MALLOC_CAP_8BIT);
        if (payload == NULL) {
            ESP_LOGE(TAG, "out of memory for websocket payload");
            return false;
        }
        const bool read_ok = recv_exact(sock, payload, (size_t)len);
        if (read_ok && opcode == 1) {
            handle_text_frame(sock, payload, len, &pending);
        } else if (read_ok && opcode == 2) {
            handle_binary_frame(payload, len, &pending);
        }
        free(payload);
        if (!read_ok) {
            return false;
        }
    }
}

static bool endpoint_from_mdns(DaemonEndpoint *endpoint)
{
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_lyrics", "_tcp", MDNS_TIMEOUT_MS, 5, &results);
    if (err != ESP_OK || results == NULL) {
        mdns_query_results_free(results);
        return false;
    }

    bool found = false;
    for (mdns_result_t *r = results; r != NULL && !found; r = r->next) {
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) {
                continue;
            }
            struct sockaddr_in *addr = (struct sockaddr_in *)&endpoint->addr;
            memset(endpoint, 0, sizeof(*endpoint));
            addr->sin_family = AF_INET;
            addr->sin_port = htons(r->port ? r->port : LYRICS_DAEMON_PORT);
            addr->sin_addr.s_addr = a->addr.u_addr.ip4.addr;
            endpoint->addr_len = sizeof(struct sockaddr_in);
            endpoint->port = ntohs(addr->sin_port);
            snprintf(endpoint->label, sizeof(endpoint->label), IPSTR, IP2STR(&a->addr.u_addr.ip4));
            found = true;
            break;
        }
    }

    mdns_query_results_free(results);
    return found;
}

static bool endpoint_from_config(DaemonEndpoint *endpoint)
{
    if (LYRICS_DAEMON_HOST[0] == '\0') {
        return false;
    }

    char port[8];
    snprintf(port, sizeof(port), "%u", LYRICS_DAEMON_PORT);
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(LYRICS_DAEMON_HOST, port, &hints, &res) != 0 || res == NULL) {
        return false;
    }

    memset(endpoint, 0, sizeof(*endpoint));
    memcpy(&endpoint->addr, res->ai_addr, res->ai_addrlen);
    endpoint->addr_len = (socklen_t)res->ai_addrlen;
    endpoint->port = LYRICS_DAEMON_PORT;
    snprintf(endpoint->label, sizeof(endpoint->label), "%s", LYRICS_DAEMON_HOST);
    freeaddrinfo(res);
    return true;
}

static bool resolve_endpoint(DaemonEndpoint *endpoint)
{
    if (endpoint_from_mdns(endpoint)) {
        ESP_LOGI(TAG, "discovered lyrics daemon at %s:%u", endpoint->label, endpoint->port);
        return true;
    }
    if (endpoint_from_config(endpoint)) {
        ESP_LOGI(TAG, "using configured lyrics daemon at %s:%u", endpoint->label, endpoint->port);
        return true;
    }
    ESP_LOGW(TAG, "lyrics daemon not found via mDNS; set LYRICS_DAEMON_HOST for fallback");
    return false;
}

static void board_client_task(void *arg)
{
    (void)arg;
    while (true) {
        DaemonEndpoint endpoint;
        if (!resolve_endpoint(&endpoint)) {
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        int sock = socket(endpoint.addr.ss_family, SOCK_STREAM, IPPROTO_IP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        if (connect(sock, (struct sockaddr *)&endpoint.addr, endpoint.addr_len) != 0) {
            ESP_LOGW(TAG, "connect to %s:%u failed: errno=%d", endpoint.label, endpoint.port, errno);
            close(sock);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
            continue;
        }

        ESP_LOGI(TAG, "connected to lyrics daemon");
        if (websocket_handshake(sock, &endpoint)) {
            music_set_daemon_connected(true);
            websocket_loop(sock);
        } else {
            ESP_LOGW(TAG, "websocket handshake failed");
        }
        close(sock);
        music_set_daemon_connected(false);
        music_clear_full_frame();
        ESP_LOGW(TAG, "lyrics daemon disconnected");
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
    }
}

esp_err_t board_client_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(board_client_task, "lyrics_ws", 8192, NULL, 4, NULL, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
