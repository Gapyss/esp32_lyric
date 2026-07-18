#include "board_client.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "display_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/md.h"
#include "mdns.h"
#include "music_screen.h"
#include "nvs.h"

#ifndef LYRICS_DAEMON_PORT
#define LYRICS_DAEMON_PORT 8766
#endif

#ifndef LYRICS_ALLOW_LEGACY_PROTO1
#define LYRICS_ALLOW_LEGACY_PROTO1 0
#endif

static const char *TAG = "board_client";
static const size_t FRAME_ENVELOPE_HEADER_BYTES = 28;
static const uint8_t FRAME_ENVELOPE_VERSION = 1;
static const uint8_t FRAME_KIND_FULL_NOW = 1;
static const uint8_t FRAME_KIND_FULL_SCHEDULED = 2;
static const uint8_t FRAME_KIND_RECT_NOW = 3;
static const uint8_t FRAME_KIND_RECT_SCHEDULED = 4;
static const size_t MAX_TEXT_PAYLOAD = 1024;
static const int MDNS_TIMEOUT_MS = 1500;
static const int HEARTBEAT_INTERVAL_MS = 20000;
static const int HEARTBEAT_TIMEOUT_MS = 10000;
static const size_t MAX_ENDPOINTS = 12;
static const size_t TOKEN_HEX_BYTES = 64;
static const size_t UUID_BYTES = 36;
static const char *AUTH_NAMESPACE = "lyrics_auth";
static const char *TOKEN_KEY = "token";
static const char *PREFERRED_UUID_KEY = "daemon_uuid";
static const uint8_t SEC2_VERSION = 1;
static const uint8_t SEC2_TEXT = 1;
static const uint8_t SEC2_BINARY = 2;
static const size_t SEC2_HEADER_BYTES = 20;
static const size_t SEC2_TAG_BYTES = 32;
static TaskHandle_t g_board_client_task;
static volatile bool g_network_online;
static volatile BoardConnectionState g_state = BOARD_CONNECTION_RETRY_WAIT;
static portMUX_TYPE g_socket_lock = portMUX_INITIALIZER_UNLOCKED;
static int g_active_socket = -1;
static void set_state(BoardConnectionState state);

static void clear_active_socket(int sock)
{
    portENTER_CRITICAL(&g_socket_lock);
    if (g_active_socket == sock) g_active_socket = -1;
    portEXIT_CRITICAL(&g_socket_lock);
}

typedef struct {
    struct sockaddr_storage addr;
    socklen_t addr_len;
    uint16_t port;
    char label[96];
    char daemon_uuid[UUID_BYTES + 1];
    int score;
    bool legacy;
} DaemonEndpoint;

typedef struct {
    uint8_t tx_key[32];
    uint8_t rx_key[32];
    uint64_t tx_sequence;
    uint64_t rx_sequence;
} SecureSession;

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

static bool websocket_handshake(int sock, const DaemonEndpoint *endpoint, const char *token)
{
    char request[512];
    char path[160] = "/board";
    if (endpoint->legacy) {
        snprintf(path, sizeof(path), "/board?proto=1&token=%s", token);
    }
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

static uint64_t read_be64(const uint8_t *data)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | data[i];
    return value;
}

static void write_be32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24); data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8); data[3] = (uint8_t)value;
}

static void write_be64(uint8_t *data, uint64_t value)
{
    for (int i = 7; i >= 0; --i) { data[i] = (uint8_t)value; value >>= 8; }
}

static bool is_hex_string(const char *text, size_t length)
{
    if (text == NULL || strlen(text) != length) return false;
    for (size_t i = 0; i < length; ++i) if (!isxdigit((unsigned char)text[i])) return false;
    return true;
}

static bool is_uuid_string(const char *text)
{
    if (text == NULL || strlen(text) != UUID_BYTES) return false;
    for (size_t i = 0; i < UUID_BYTES; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
        } else if (!isxdigit((unsigned char)text[i])) return false;
    }
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_decode(const char *text, uint8_t *output, size_t output_len)
{
    if (!is_hex_string(text, output_len * 2)) return false;
    for (size_t i = 0; i < output_len; ++i) {
        output[i] = (uint8_t)((hex_nibble(text[i * 2]) << 4) | hex_nibble(text[i * 2 + 1]));
    }
    return true;
}

static void hex_encode(const uint8_t *input, size_t input_len, char *output)
{
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < input_len; ++i) {
        output[i * 2] = HEX[input[i] >> 4]; output[i * 2 + 1] = HEX[input[i] & 15];
    }
    output[input_len * 2] = '\0';
}

static bool sha256_hmac(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len, uint8_t output[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    return info != NULL && mbedtls_md_hmac(info, key, key_len, data, data_len, output) == 0;
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

static bool auth_proof(const char *role, const char *token, const char *daemon_uuid,
                       const uint8_t server_nonce[32], const uint8_t client_nonce[32], uint8_t output[32])
{
    uint8_t transcript[160];
    size_t used = 0;
    used += snprintf((char *)transcript + used, sizeof(transcript) - used, "lyrics-v2/%s", role);
    transcript[used++] = 0;
    memcpy(transcript + used, daemon_uuid, strlen(daemon_uuid)); used += strlen(daemon_uuid);
    transcript[used++] = 0;
    memcpy(transcript + used, server_nonce, 32); used += 32;
    memcpy(transcript + used, client_nonce, 32); used += 32;
    return sha256_hmac((const uint8_t *)token, strlen(token), transcript, used, output);
}

static bool derive_session_keys(const char *token, const uint8_t server_nonce[32],
                                const uint8_t client_nonce[32], SecureSession *session)
{
    uint8_t salt[64], prk[32], previous[32], block_input[64];
    memcpy(salt, server_nonce, 32); memcpy(salt + 32, client_nonce, 32);
    if (!sha256_hmac(salt, sizeof(salt), (const uint8_t *)token, strlen(token), prk)) return false;
    static const uint8_t INFO[] = "lyrics-v2/session";
    size_t previous_len = 0;
    uint8_t output[64];
    for (uint8_t counter = 1; counter <= 2; ++counter) {
        size_t used = 0;
        if (previous_len) { memcpy(block_input, previous, previous_len); used += previous_len; }
        memcpy(block_input + used, INFO, sizeof(INFO) - 1); used += sizeof(INFO) - 1;
        block_input[used++] = counter;
        if (!sha256_hmac(prk, sizeof(prk), block_input, used, previous)) return false;
        memcpy(output + (counter - 1) * 32, previous, 32);
        previous_len = 32;
    }
    memcpy(session->tx_key, output, 32);       // client -> server
    memcpy(session->rx_key, output + 32, 32);  // server -> client
    session->tx_sequence = session->rx_sequence = 0;
    memset(prk, 0, sizeof(prk)); memset(output, 0, sizeof(output));
    return true;
}

static bool secure_send(int sock, SecureSession *session, uint8_t content_type,
                        const uint8_t *payload, size_t payload_len)
{
    const size_t total = SEC2_HEADER_BYTES + payload_len + SEC2_TAG_BYTES;
    uint8_t *record = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_8BIT);
    if (record == NULL) return false;
    memcpy(record, "SEC2", 4); record[4] = SEC2_VERSION; record[5] = content_type;
    record[6] = record[7] = 0;
    write_be64(record + 8, ++session->tx_sequence);
    write_be32(record + 16, (uint32_t)payload_len);
    memcpy(record + SEC2_HEADER_BYTES, payload, payload_len);
    bool ok = sha256_hmac(session->tx_key, sizeof(session->tx_key), record,
                          SEC2_HEADER_BYTES + payload_len, record + SEC2_HEADER_BYTES + payload_len) &&
              send_ws_frame(sock, 2, record, total);
    free(record);
    return ok;
}

static bool secure_unwrap(SecureSession *session, uint8_t *record, size_t record_len,
                          uint8_t *content_type, uint8_t **payload, size_t *payload_len)
{
    if (record_len < SEC2_HEADER_BYTES + SEC2_TAG_BYTES || memcmp(record, "SEC2", 4) != 0 ||
        record[4] != SEC2_VERSION || record[6] != 0 || record[7] != 0) return false;
    const uint64_t sequence = read_be64(record + 8);
    const uint32_t length = read_be32(record + 16);
    if (sequence != session->rx_sequence + 1 ||
        record_len != SEC2_HEADER_BYTES + length + SEC2_TAG_BYTES) return false;
    uint8_t expected[32];
    if (!sha256_hmac(session->rx_key, sizeof(session->rx_key), record,
                     SEC2_HEADER_BYTES + length, expected) ||
        !constant_time_equal(expected, record + SEC2_HEADER_BYTES + length, sizeof(expected))) return false;
    if (record[5] != SEC2_TEXT && record[5] != SEC2_BINARY) return false;
    session->rx_sequence = sequence;
    *content_type = record[5]; *payload = record + SEC2_HEADER_BYTES; *payload_len = length;
    return true;
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

static bool recv_ws_payload(int sock, uint8_t *opcode, uint8_t **payload, size_t *payload_len, size_t max_len)
{
    while (true) {
        uint64_t length = 0;
        if (!read_ws_header(sock, opcode, &length) || length > max_len) return false;
        uint8_t *data = (uint8_t *)heap_caps_malloc((size_t)length + 1, MALLOC_CAP_8BIT);
        if (data == NULL) return false;
        if (!recv_exact(sock, data, (size_t)length)) { free(data); return false; }
        data[length] = 0;
        if (*opcode == 9) {
            const bool sent = send_ws_frame(sock, 10, data, (size_t)length);
            free(data);
            if (!sent) return false;
            continue;
        }
        if (*opcode == 8) { free(data); return false; }
        *payload = data; *payload_len = (size_t)length;
        return true;
    }
}

static bool authenticate_daemon(int sock, const DaemonEndpoint *endpoint, const char *token,
                                SecureSession *session)
{
    uint8_t opcode = 0, *payload = NULL;
    size_t length = 0;
    if (!recv_ws_payload(sock, &opcode, &payload, &length, MAX_TEXT_PAYLOAD) || opcode != 1) {
        free(payload); return false;
    }
    char type[32], daemon_uuid[UUID_BYTES + 1], server_nonce_hex[65];
    int proto = 0;
    const bool challenge_ok = json_get_string((char *)payload, "type", type, sizeof(type)) &&
                              json_get_int((char *)payload, "proto", &proto) &&
                              json_get_string((char *)payload, "daemonUuid", daemon_uuid, sizeof(daemon_uuid)) &&
                              json_get_string((char *)payload, "serverNonce", server_nonce_hex, sizeof(server_nonce_hex));
    free(payload); payload = NULL;
    if (!challenge_ok || strcmp(type, "auth-challenge") != 0 || proto != 2 ||
        strcmp(daemon_uuid, endpoint->daemon_uuid) != 0) return false;

    uint8_t server_nonce[32], client_nonce[32], client_proof[32], server_proof[32];
    if (!hex_decode(server_nonce_hex, server_nonce, sizeof(server_nonce))) return false;
    esp_fill_random(client_nonce, sizeof(client_nonce));
    if (!auth_proof("client", token, daemon_uuid, server_nonce, client_nonce, client_proof)) return false;
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) return false;
    char client_nonce_hex[65], proof_hex[65], board_id[18];
    hex_encode(client_nonce, sizeof(client_nonce), client_nonce_hex);
    hex_encode(client_proof, sizeof(client_proof), proof_hex);
    snprintf(board_id, sizeof(board_id), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    char response[320];
    snprintf(response, sizeof(response),
             "{\"type\":\"auth-response\",\"proto\":2,\"clientNonce\":\"%s\",\"boardId\":\"%s\",\"proof\":\"%s\"}",
             client_nonce_hex, board_id, proof_hex);
    if (!send_ws_frame(sock, 1, (const uint8_t *)response, strlen(response)) ||
        !recv_ws_payload(sock, &opcode, &payload, &length, MAX_TEXT_PAYLOAD) || opcode != 1) {
        free(payload); return false;
    }
    char server_proof_hex[65]; proto = 0;
    const bool response_ok = json_get_string((char *)payload, "type", type, sizeof(type)) &&
                             json_get_int((char *)payload, "proto", &proto) &&
                             json_get_string((char *)payload, "proof", server_proof_hex, sizeof(server_proof_hex));
    free(payload);
    if (!response_ok || strcmp(type, "auth-ok") != 0 || proto != 2 ||
        !hex_decode(server_proof_hex, server_proof, sizeof(server_proof))) return false;
    uint8_t expected[32];
    if (!auth_proof("server", token, daemon_uuid, server_nonce, client_nonce, expected) ||
        !constant_time_equal(server_proof, expected, sizeof(expected))) return false;
    return derive_session_keys(token, server_nonce, client_nonce, session);
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

static bool legacy_websocket_loop(int sock)
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

static bool secure_websocket_loop(int sock, SecureSession *session, bool *became_connected)
{
    *became_connected = false;
    uint8_t opcode = 0, *record = NULL;
    size_t record_len = 0;
    if (!recv_ws_payload(sock, &opcode, &record, &record_len,
                         MUSIC_DISPLAY_BYTES + FRAME_ENVELOPE_HEADER_BYTES + SEC2_HEADER_BYTES + SEC2_TAG_BYTES) ||
        opcode != 2) {
        free(record); return false;
    }
    uint8_t content_type = 0, *clear = NULL;
    size_t clear_len = 0;
    const bool hello_ok = secure_unwrap(session, record, record_len, &content_type, &clear, &clear_len) &&
                          content_type == SEC2_TEXT && strstr((char *)clear, "\"type\":\"hello\"") != NULL;
    free(record);
    if (!hello_ok) return false;
    static const uint8_t READY[] = "{\"type\":\"ready\"}";
    if (!secure_send(sock, session, SEC2_TEXT, READY, sizeof(READY) - 1)) return false;
    *became_connected = true;
    set_state(BOARD_CONNECTION_CONNECTED);

    int64_t last_ping_ms = esp_timer_get_time() / 1000;
    int64_t pong_deadline_ms = 0;
    while (g_network_online) {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (pong_deadline_ms != 0 && now_ms >= pong_deadline_ms) {
            ESP_LOGW(TAG, "daemon heartbeat timed out");
            return false;
        }
        if (pong_deadline_ms == 0 && now_ms - last_ping_ms >= HEARTBEAT_INTERVAL_MS) {
            uint8_t ping[8]; write_be64(ping, (uint64_t)now_ms);
            if (!send_ws_frame(sock, 9, ping, sizeof(ping))) return false;
            last_ping_ms = now_ms;
            pong_deadline_ms = now_ms + HEARTBEAT_TIMEOUT_MS;
        }

        fd_set read_set;
        FD_ZERO(&read_set); FD_SET(sock, &read_set);
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
        const int selected = select(sock + 1, &read_set, NULL, NULL, &timeout);
        if (selected < 0) return false;
        if (selected == 0) continue;

        uint64_t length = 0;
        if (!read_ws_header(sock, &opcode, &length)) return false;
        const size_t max_record = MUSIC_DISPLAY_BYTES + FRAME_ENVELOPE_HEADER_BYTES + SEC2_HEADER_BYTES + SEC2_TAG_BYTES;
        if (length > max_record) return false;
        record = (uint8_t *)heap_caps_malloc((size_t)length + 1, MALLOC_CAP_8BIT);
        if (record == NULL || !recv_exact(sock, record, (size_t)length)) { free(record); return false; }
        record[length] = 0;
        if (opcode == 8) { free(record); return false; }
        if (opcode == 9) {
            const bool ok = send_ws_frame(sock, 10, record, (size_t)length);
            free(record); if (!ok) return false; continue;
        }
        if (opcode == 10) {
            pong_deadline_ms = 0; free(record); continue;
        }
        if (opcode != 2 || !secure_unwrap(session, record, (size_t)length,
                                           &content_type, &clear, &clear_len)) {
            free(record); return false;
        }
        if (content_type == SEC2_TEXT) {
            if (clear_len < MAX_TEXT_PAYLOAD) {
                clear[clear_len] = 0;
                char type[24];
                if (json_get_string((char *)clear, "type", type, sizeof(type)) && strcmp(type, "clear") == 0) {
                    music_clear_full_frame();
                }
            }
        } else {
            handle_binary_envelope(clear, clear_len);
        }
        free(record);
    }
    return false;
}

static void set_state(BoardConnectionState state)
{
    g_state = state;
    music_set_daemon_status(board_client_state_name(state));
    music_set_daemon_connected(state == BOARD_CONNECTION_CONNECTED);
}

static bool load_nvs_string(const char *key, char *output, size_t capacity)
{
    nvs_handle_t handle = 0;
    if (nvs_open(AUTH_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t length = capacity;
    const esp_err_t err = nvs_get_str(handle, key, output, &length);
    nvs_close(handle);
    return err == ESP_OK && length > 1 && length <= capacity;
}

static bool load_token(char token[TOKEN_HEX_BYTES + 1])
{
    return load_nvs_string(TOKEN_KEY, token, TOKEN_HEX_BYTES + 1) && is_hex_string(token, TOKEN_HEX_BYTES);
}

static void save_preferred_uuid(const char *daemon_uuid)
{
    if (strlen(daemon_uuid) != UUID_BYTES) return;
    nvs_handle_t handle = 0;
    if (nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_str(handle, PREFERRED_UUID_KEY, daemon_uuid) == ESP_OK) nvs_commit(handle);
        nvs_close(handle);
    }
}

static bool txt_value(const mdns_result_t *result, const char *key, char *output, size_t capacity)
{
    for (size_t i = 0; i < result->txt_count; ++i) {
        if (result->txt[i].key != NULL && result->txt[i].value != NULL && strcmp(result->txt[i].key, key) == 0) {
            const size_t length = result->txt_value_len != NULL ? result->txt_value_len[i] : strlen(result->txt[i].value);
            if (length >= capacity) return false;
            memcpy(output, result->txt[i].value, length); output[length] = '\0';
            return true;
        }
    }
    return false;
}

static int compare_endpoints(const void *left, const void *right)
{
    const DaemonEndpoint *a = (const DaemonEndpoint *)left;
    const DaemonEndpoint *b = (const DaemonEndpoint *)right;
    return b->score - a->score;
}

static size_t discover_endpoints(DaemonEndpoint endpoints[MAX_ENDPOINTS], const char *preferred_uuid)
{
    mdns_result_t *results = NULL;
    const esp_err_t err = mdns_query_ptr("_lyrics", "_tcp", MDNS_TIMEOUT_MS, MAX_ENDPOINTS, &results);
    if (err != ESP_OK || results == NULL) {
        ESP_LOGW(TAG, "mDNS _lyrics._tcp query returned no results: %s", esp_err_to_name(err));
        mdns_query_results_free(results);
        return 0;
    }
    size_t count = 0;
    bool found_secure = false;
    for (mdns_result_t *r = results; r != NULL && count < MAX_ENDPOINTS; r = r->next) {
        char proto[8] = {}, auth[32] = {}, daemon_uuid[UUID_BYTES + 1] = {};
        const bool has_proto = txt_value(r, "proto", proto, sizeof(proto));
        const bool secure = has_proto && strcmp(proto, "2") == 0 &&
                            txt_value(r, "auth", auth, sizeof(auth)) && strcmp(auth, "hmac-sha256") == 0 &&
                            txt_value(r, "uuid", daemon_uuid, sizeof(daemon_uuid)) && is_uuid_string(daemon_uuid);
        const bool legacy = LYRICS_ALLOW_LEGACY_PROTO1 && has_proto && strcmp(proto, "1") == 0 &&
                            txt_value(r, "auth", auth, sizeof(auth)) && strcmp(auth, "token") == 0;
        if (!secure && !legacy) {
            ESP_LOGW(TAG, "rejected mDNS service %s: incompatible or missing TXT", r->instance_name ?: "(unnamed)");
            continue;
        }
        ESP_LOGI(TAG, "compatible mDNS service %s uuid=%s port=%u addresses=%s",
                 r->instance_name ?: "(unnamed)", daemon_uuid, r->port,
                 r->addr == NULL ? "deferred" : "inline");
        found_secure = found_secure || secure;
        bool found_v4 = false;
        for (mdns_ip_addr_t *a = r->addr; a != NULL && count < MAX_ENDPOINTS; a = a->next) {
            if (a->addr.type != ESP_IPADDR_TYPE_V4) continue;
            found_v4 = true;
            DaemonEndpoint endpoint = {};
            struct sockaddr_in *address = (struct sockaddr_in *)&endpoint.addr;
            address->sin_family = AF_INET; address->sin_port = htons(r->port ? r->port : LYRICS_DAEMON_PORT);
            address->sin_addr.s_addr = a->addr.u_addr.ip4.addr;
            endpoint.addr_len = sizeof(*address); endpoint.port = ntohs(address->sin_port); endpoint.legacy = legacy;
            snprintf(endpoint.label, sizeof(endpoint.label), IPSTR, IP2STR(&a->addr.u_addr.ip4));
            snprintf(endpoint.daemon_uuid, sizeof(endpoint.daemon_uuid), "%s", daemon_uuid);
            if (preferred_uuid[0] != '\0' && strcmp(preferred_uuid, daemon_uuid) == 0) endpoint.score += 100;
            if (r->esp_netif != NULL) {
                esp_netif_ip_info_t info = {};
                if (esp_netif_get_ip_info(r->esp_netif, &info) == ESP_OK &&
                    (info.ip.addr & info.netmask.addr) == (a->addr.u_addr.ip4.addr & info.netmask.addr)) endpoint.score += 10;
            }
            bool duplicate = false;
            for (size_t i = 0; i < count; ++i) {
                const struct sockaddr_in *existing = (const struct sockaddr_in *)&endpoints[i].addr;
                if (existing->sin_addr.s_addr == address->sin_addr.s_addr && endpoints[i].port == endpoint.port &&
                    strcmp(endpoints[i].daemon_uuid, endpoint.daemon_uuid) == 0) { duplicate = true; break; }
            }
            if (!duplicate) endpoints[count++] = endpoint;
        }
        if (!found_v4 && r->hostname != NULL && count < MAX_ENDPOINTS) {
            esp_ip4_addr_t resolved = {};
            const esp_err_t resolve_err = mdns_query_a(r->hostname, 1000, &resolved);
            if (resolve_err == ESP_OK) {
                DaemonEndpoint endpoint = {};
                struct sockaddr_in *address = (struct sockaddr_in *)&endpoint.addr;
                address->sin_family = AF_INET;
                address->sin_port = htons(r->port ? r->port : LYRICS_DAEMON_PORT);
                address->sin_addr.s_addr = resolved.addr;
                endpoint.addr_len = sizeof(*address);
                endpoint.port = ntohs(address->sin_port);
                endpoint.legacy = legacy;
                snprintf(endpoint.label, sizeof(endpoint.label), IPSTR, IP2STR(&resolved));
                snprintf(endpoint.daemon_uuid, sizeof(endpoint.daemon_uuid), "%s", daemon_uuid);
                if (preferred_uuid[0] != '\0' && strcmp(preferred_uuid, daemon_uuid) == 0) endpoint.score += 100;
                esp_netif_ip_info_t info = {};
                if (r->esp_netif != NULL && esp_netif_get_ip_info(r->esp_netif, &info) == ESP_OK &&
                    (info.ip.addr & info.netmask.addr) == (resolved.addr & info.netmask.addr)) endpoint.score += 10;
                endpoints[count++] = endpoint;
                ESP_LOGI(TAG, "resolved deferred daemon address %s", endpoint.label);
            } else {
                ESP_LOGW(TAG, "mDNS A lookup for %s failed: %s", r->hostname, esp_err_to_name(resolve_err));
            }
        }
    }
    mdns_query_results_free(results);
    if (found_secure) {
        size_t write = 0;
        for (size_t read = 0; read < count; ++read) {
            if (!endpoints[read].legacy) endpoints[write++] = endpoints[read];
        }
        count = write;
    }
    qsort(endpoints, count, sizeof(endpoints[0]), compare_endpoints);
    ESP_LOGI(TAG, "mDNS discovery selected %u compatible endpoint(s)", (unsigned)count);
    return count;
}

static bool connect_with_timeout(int sock, const DaemonEndpoint *endpoint)
{
    const int original_flags = fcntl(sock, F_GETFL, 0);
    if (original_flags < 0 || fcntl(sock, F_SETFL, original_flags | O_NONBLOCK) < 0) return false;
    const int result = connect(sock, (const struct sockaddr *)&endpoint->addr, endpoint->addr_len);
    if (result != 0 && errno != EINPROGRESS) return false;
    if (result != 0) {
        fd_set write_set; FD_ZERO(&write_set); FD_SET(sock, &write_set);
        struct timeval timeout = {.tv_sec = 1, .tv_usec = 500000};
        if (select(sock + 1, NULL, &write_set, NULL, &timeout) <= 0) return false;
        int socket_error = 0; socklen_t error_len = sizeof(socket_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) != 0 || socket_error != 0) return false;
    }
    return fcntl(sock, F_SETFL, original_flags) == 0;
}

static void wait_retry(unsigned int attempt)
{
    uint32_t base = 500U << (attempt > 6 ? 6 : attempt);
    if (base > 30000U) base = 30000U;
    const uint32_t jitter = base > 4 ? esp_random() % (base / 4) : 0;
    const uint32_t delay_ms = base + jitter > 30000U ? 30000U : base + jitter;
    set_state(BOARD_CONNECTION_RETRY_WAIT);
    for (uint32_t elapsed = 0; elapsed < delay_ms && g_network_online; elapsed += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void board_client_task(void *arg)
{
    (void)arg;
    unsigned int retry_attempt = 0;
    while (true) {
        if (!g_network_online) { set_state(BOARD_CONNECTION_RETRY_WAIT); vTaskDelay(pdMS_TO_TICKS(250)); continue; }
        char token[TOKEN_HEX_BYTES + 1] = {};
        if (!load_token(token)) { set_state(BOARD_CONNECTION_PAIRING_REQUIRED); vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
        char preferred_uuid[UUID_BYTES + 1] = {};
        load_nvs_string(PREFERRED_UUID_KEY, preferred_uuid, sizeof(preferred_uuid));
        set_state(BOARD_CONNECTION_DISCOVERING);
        DaemonEndpoint endpoints[MAX_ENDPOINTS] = {};
        const size_t endpoint_count = discover_endpoints(endpoints, preferred_uuid);
        bool connected_once = false;
        for (size_t i = 0; i < endpoint_count && g_network_online; ++i) {
            const DaemonEndpoint *endpoint = &endpoints[i];
            set_state(BOARD_CONNECTION_CONNECTING);
            int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (sock < 0) continue;
            struct timeval io_timeout = {.tv_sec = 6, .tv_usec = 0};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
            setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
            portENTER_CRITICAL(&g_socket_lock); g_active_socket = sock; portEXIT_CRITICAL(&g_socket_lock);
            if (!connect_with_timeout(sock, endpoint) || !websocket_handshake(sock, endpoint, token)) {
                ESP_LOGW(TAG, "connect to %s:%u failed", endpoint->label, endpoint->port);
                clear_active_socket(sock);
                close(sock);
                continue;
            }
            set_state(BOARD_CONNECTION_AUTHENTICATING);
            SecureSession session = {};
            bool authenticated = endpoint->legacy || authenticate_daemon(sock, endpoint, token, &session);
            if (!authenticated) {
                set_state(BOARD_CONNECTION_AUTH_FAILED);
                ESP_LOGW(TAG, "authentication failed for daemon %s", endpoint->daemon_uuid);
                clear_active_socket(sock);
                close(sock);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            save_preferred_uuid(endpoint->daemon_uuid);
            if (endpoint->legacy) {
                set_state(BOARD_CONNECTION_CONNECTED);
                connected_once = true;
                legacy_websocket_loop(sock);
            } else {
                secure_websocket_loop(sock, &session, &connected_once);
            }
            if (connected_once) {
                retry_attempt = 0;
                ESP_LOGI(TAG, "authenticated lyrics daemon %s at %s:%u", endpoint->daemon_uuid, endpoint->label, endpoint->port);
            }
            clear_active_socket(sock);
            shutdown(sock, SHUT_RDWR); close(sock);
            music_clear_full_frame();
            if (g_network_online) ESP_LOGW(TAG, "lyrics daemon disconnected; starting fresh discovery");
            break;
        }
        memset(token, 0, sizeof(token));
        if (!connected_once) ++retry_attempt;
        if (g_network_online) wait_retry(retry_attempt);
    }
}

esp_err_t board_client_start(void)
{
    if (g_board_client_task != NULL) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(board_client_task,
                                           "lyrics_ws",
                                           8192,
                                           NULL,
                                           4,
                                           &g_board_client_task,
                                           0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void board_client_network_changed(bool online)
{
    g_network_online = online;
    if (!online) {
        portENTER_CRITICAL(&g_socket_lock);
        const int sock = g_active_socket;
        portEXIT_CRITICAL(&g_socket_lock);
        if (sock >= 0) shutdown(sock, SHUT_RDWR);
        music_set_daemon_connected(false);
    }
}

BoardConnectionState board_client_get_state(void) { return g_state; }

const char *board_client_state_name(BoardConnectionState state)
{
    switch (state) {
        case BOARD_CONNECTION_PAIRING_REQUIRED: return "PAIRING_REQUIRED";
        case BOARD_CONNECTION_DISCOVERING: return "DISCOVERING";
        case BOARD_CONNECTION_CONNECTING: return "CONNECTING";
        case BOARD_CONNECTION_AUTHENTICATING: return "AUTHENTICATING";
        case BOARD_CONNECTION_CONNECTED: return "CONNECTED";
        case BOARD_CONNECTION_AUTH_FAILED: return "AUTH_FAILED";
        case BOARD_CONNECTION_RETRY_WAIT: return "RETRY_WAIT";
        default: return "RETRY_WAIT";
    }
}

bool board_client_has_pairing_token(void)
{
    char token[TOKEN_HEX_BYTES + 1] = {};
    const bool present = load_token(token);
    memset(token, 0, sizeof(token));
    return present;
}

esp_err_t board_client_set_pairing_token(const char *token)
{
    if (!is_hex_string(token, TOKEN_HEX_BYTES)) return ESP_ERR_INVALID_ARG;
    char normalized[TOKEN_HEX_BYTES + 1];
    for (size_t i = 0; i < TOKEN_HEX_BYTES; ++i) normalized[i] = (char)tolower((unsigned char)token[i]);
    normalized[TOKEN_HEX_BYTES] = '\0';
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(AUTH_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_str(handle, TOKEN_KEY, normalized);
    if (err == ESP_OK) nvs_erase_key(handle, PREFERRED_UUID_KEY);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle != 0) nvs_close(handle);
    memset(normalized, 0, sizeof(normalized));
    if (err == ESP_OK) {
        portENTER_CRITICAL(&g_socket_lock); const int sock = g_active_socket; portEXIT_CRITICAL(&g_socket_lock);
        if (sock >= 0) shutdown(sock, SHUT_RDWR);
    }
    return err;
}
