#include "provisioning_portal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "network_manager.h"
#include "board_client.h"

namespace {

constexpr char TAG[] = "provisioning";
constexpr size_t MAX_BODY_BYTES = 512;
constexpr size_t MAX_JSON_BYTES = 8192;

httpd_handle_t g_server;
TaskHandle_t g_dns_task;
int g_dns_socket = -1;
portMUX_TYPE g_activity_lock = portMUX_INITIALIZER_UNLOCKED;
int64_t g_last_activity_us;

const char SETUP_HTML[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>G4PYS Wi-Fi Setup</title><style>
:root{font-family:system-ui,sans-serif;color-scheme:light dark}body{max-width:34rem;margin:auto;padding:1rem;background:#f4f5f7;color:#18202a}
.card{background:white;border-radius:14px;padding:1rem;margin:.8rem 0;box-shadow:0 2px 12px #0001}h1{font-size:1.45rem;margin:.3rem 0}label{display:block;margin:.7rem 0 .25rem}
select,input,button{box-sizing:border-box;width:100%;font:inherit;padding:.75rem;border:1px solid #9aa3ad;border-radius:9px;background:white;color:#18202a}
button{background:#18202a;color:white;border:0;font-weight:650;margin-top:.7rem}button.secondary{background:#e8ebef;color:#18202a}button:disabled{opacity:.5}.muted{color:#68727d;font-size:.9rem}.bad{color:#a22}.good{color:#176b36}
@media(prefers-color-scheme:dark){body{background:#11151a;color:#e8edf2}.card,select,input{background:#1b222a;color:#e8edf2}.secondary{background:#303943!important;color:#fff!important}}
</style></head><body><div class="card"><h1>Connect this display</h1><div id="state" class="muted">Loading nearby networks…</div></div>
<div class="card"><form id="form"><label for="network">Wi-Fi network</label><select id="network"></select>
<div id="hiddenFields" hidden><label for="ssid">Hidden network name</label><input id="ssid" maxlength="32" autocomplete="off">
<label for="hiddenAuth">Security</label><select id="hiddenAuth"><option value="1">WPA2 Personal</option><option value="2">WPA2/WPA3 transition</option><option value="0">Open</option></select></div>
<div id="passwordFields"><label for="password">Password</label><input id="password" type="password" minlength="8" maxlength="63" autocomplete="new-password"></div>
<label for="pairingToken">Lyrics daemon pairing token</label><input id="pairingToken" maxlength="64" minlength="64" pattern="[0-9A-Fa-f]{64}" autocomplete="off">
<p class="muted">On the Mac, run <code>lyrics_display_daemon.py pairing-token</code>. You can leave this blank and pair later; Wi-Fi will still connect.</p>
<button id="connect" type="submit">Connect</button></form><button id="refresh" class="secondary">Refresh networks</button><button id="cancel" class="secondary" hidden>Cancel setup</button>
<p class="muted">Only 2.4 GHz open, WPA2, and WPA2/WPA3 transition networks are supported.</p></div>
<script>
const $=id=>document.getElementById(id);let networks=[];
function selection(){const v=$('network').value, hidden=v==='hidden';$('hiddenFields').hidden=!hidden;let auth=hidden?+$('hiddenAuth').value:+(networks[v]?.auth??255);$('passwordFields').hidden=auth===0;$('password').required=auth!==0;}
function drawNetworks(data){networks=data.networks||[];$('network').textContent='';networks.forEach((n,i)=>{let o=document.createElement('option');o.value=i;o.disabled=!n.supported;o.textContent=`${n.ssid||'(hidden)'}  ${n.rssi} dBm  ${n.security}${n.supported?'':' — unsupported'}`;$('network').append(o)});let h=document.createElement('option');h.value='hidden';h.textContent='Hidden network…';$('network').append(h);selection();$('refresh').disabled=!!data.scanning;}
async function loadNetworks(){let r=await fetch('/api/networks',{cache:'no-store'});drawNetworks(await r.json())}
async function status(){try{let r=await fetch('/api/status',{cache:'no-store'}),s=await r.json();$('state').textContent=s.error||s.status||s.state;$('state').className=s.error?'bad':(s.state==='handoff'?'good':'muted');$('connect').disabled=!!s.validating;$('refresh').disabled=!!s.validating||!!s.scanning;$('cancel').hidden=!s.can_cancel;if(s.state==='handoff')$('state').textContent=`Connected: ${s.ip} — ${s.hostname}.local`; }catch(e){}}
$('network').onchange=selection;$('hiddenAuth').onchange=selection;
$('refresh').onclick=async()=>{await fetch('/api/scan',{method:'POST'});await loadNetworks()};
$('cancel').onclick=async()=>{await fetch('/api/cancel',{method:'POST'});$('state').textContent='Setup canceled'};
$('form').onsubmit=async e=>{e.preventDefault();let hidden=$('network').value==='hidden',n=hidden?null:networks[+$('network').value],ssid=hidden?$('ssid').value:n.ssid,auth=hidden?+$('hiddenAuth').value:n.auth,p=auth===0?'':$('password').value;let body=new URLSearchParams({ssid,password:p,auth,pairingToken:$('pairingToken').value.trim()});let r=await fetch('/api/configure',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});if(!r.ok)$('state').textContent=await r.text();else $('state').textContent='Connecting…';};
loadNetworks();status();setInterval(status,1000);
</script></body></html>)HTML";

void mark_activity()
{
    portENTER_CRITICAL(&g_activity_lock);
    g_last_activity_us = esp_timer_get_time();
    portEXIT_CRITICAL(&g_activity_lock);
}

esp_err_t set_common_headers(httpd_req_t *req)
{
    return httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

esp_err_t send_text(httpd_req_t *req, const char *status, const char *text)
{
    set_common_headers(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, text);
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void url_decode(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) return;
    size_t out = 0;
    for (size_t in = 0; src != nullptr && src[in] != '\0' && out + 1 < dst_size; ++in) {
        if (src[in] == '%' && src[in + 1] != '\0' && src[in + 2] != '\0') {
            const int hi = hex_value(src[in + 1]);
            const int lo = hex_value(src[in + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = static_cast<char>((hi << 4) | lo);
                in += 2;
                continue;
            }
        }
        dst[out++] = src[in] == '+' ? ' ' : src[in];
    }
    dst[out] = '\0';
}

bool form_value(const char *body, const char *key, char *output, size_t output_size)
{
    const size_t key_len = strlen(key);
    const char *cursor = body;
    while (cursor != nullptr && *cursor != '\0') {
        if ((cursor == body || cursor[-1] == '&') && strncmp(cursor, key, key_len) == 0 && cursor[key_len] == '=') {
            cursor += key_len + 1;
            const char *end = strchr(cursor, '&');
            const size_t encoded_len = end == nullptr ? strlen(cursor) : static_cast<size_t>(end - cursor);
            char encoded[256];
            if (encoded_len >= sizeof(encoded)) return false;
            memcpy(encoded, cursor, encoded_len);
            encoded[encoded_len] = '\0';
            url_decode(output, output_size, encoded);
            return true;
        }
        cursor = strchr(cursor, '&');
        if (cursor != nullptr) ++cursor;
    }
    if (output_size > 0) output[0] = '\0';
    return false;
}

esp_err_t read_body(httpd_req_t *req, char *body, size_t body_size)
{
    if (req->content_len == 0 || req->content_len >= body_size || req->content_len > MAX_BODY_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t offset = 0;
    while (offset < req->content_len) {
        const int got = httpd_req_recv(req, body + offset, req->content_len - offset);
        if (got <= 0) return ESP_FAIL;
        offset += static_cast<size_t>(got);
    }
    body[offset] = '\0';
    return ESP_OK;
}

size_t json_escape(char *dst, size_t capacity, const char *src)
{
    size_t out = 0;
    for (size_t i = 0; src != nullptr && src[i] != '\0' && out + 1 < capacity; ++i) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        if ((c == '"' || c == '\\') && out + 2 < capacity) {
            dst[out++] = '\\'; dst[out++] = static_cast<char>(c);
        } else if (c < 0x20 && out + 7 < capacity) {
            out += snprintf(dst + out, capacity - out, "\\u%04x", c);
        } else {
            dst[out++] = static_cast<char>(c);
        }
    }
    dst[out] = '\0';
    return out;
}

const char *security_name(ProvisioningAuth auth)
{
    switch (auth) {
        case PROVISIONING_AUTH_OPEN: return "Open";
        case PROVISIONING_AUTH_WPA2: return "WPA2";
        case PROVISIONING_AUTH_WPA2_WPA3: return "WPA2/WPA3";
        default: return "Unsupported";
    }
}

esp_err_t root_handler(httpd_req_t *req)
{
    mark_activity();
    set_common_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t networks_handler(httpd_req_t *req)
{
    mark_activity();
    ProvisioningNetwork networks[24];
    const size_t count = network_manager_get_scan_results(networks, 24);
    NetworkSnapshot snapshot = {};
    network_manager_get_snapshot(&snapshot);
    char *json = static_cast<char *>(calloc(MAX_JSON_BYTES, 1));
    if (json == nullptr) return send_text(req, "500 Internal Server Error", "out of memory");
    size_t used = snprintf(json, MAX_JSON_BYTES, "{\"scanning\":%s,\"networks\":[", snapshot.scanning ? "true" : "false");
    for (size_t i = 0; i < count && used + 256 < MAX_JSON_BYTES; ++i) {
        char escaped[200];
        json_escape(escaped, sizeof(escaped), networks[i].ssid);
        used += snprintf(json + used, MAX_JSON_BYTES - used,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%u,\"security\":\"%s\",\"supported\":%s}",
                         i == 0 ? "" : ",", escaped, networks[i].rssi,
                         static_cast<unsigned>(networks[i].auth), security_name(networks[i].auth),
                         networks[i].supported ? "true" : "false");
    }
    snprintf(json + used, MAX_JSON_BYTES - used, "]}");
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

esp_err_t scan_handler(httpd_req_t *req)
{
    mark_activity();
    const esp_err_t err = network_manager_request_scan();
    if (err == ESP_ERR_INVALID_STATE) return send_text(req, "409 Conflict", "scan unavailable while setup is busy");
    if (err != ESP_OK) return send_text(req, "500 Internal Server Error", "could not start scan");
    return send_text(req, "202 Accepted", "scan started");
}

esp_err_t configure_handler(httpd_req_t *req)
{
    mark_activity();
    NetworkSnapshot snapshot = {};
    network_manager_get_snapshot(&snapshot);
    if (snapshot.validating) return send_text(req, "409 Conflict", "setup already in progress");

    char body[MAX_BODY_BYTES + 1] = {};
    char ssid[33] = {};
    char password[64] = {};
    char auth_text[8] = {};
    char pairing_token[65] = {};
    if (read_body(req, body, sizeof(body)) != ESP_OK || !form_value(body, "ssid", ssid, sizeof(ssid)) ||
        !form_value(body, "password", password, sizeof(password)) ||
        !form_value(body, "auth", auth_text, sizeof(auth_text))) {
        return send_text(req, "400 Bad Request", "invalid setup request");
    }
    form_value(body, "pairingToken", pairing_token, sizeof(pairing_token));
    const int auth_value = atoi(auth_text);
    if (auth_value < PROVISIONING_AUTH_OPEN || auth_value > PROVISIONING_AUTH_WPA2_WPA3) {
        return send_text(req, "400 Bad Request", "unsupported network security");
    }
    if (pairing_token[0] != '\0' && board_client_set_pairing_token(pairing_token) != ESP_OK) {
        return send_text(req, "400 Bad Request", "pairing token must be exactly 64 hexadecimal characters");
    }
    const esp_err_t err = network_manager_submit_candidate(ssid, password, static_cast<ProvisioningAuth>(auth_value));
    memset(password, 0, sizeof(password));
    memset(body, 0, sizeof(body));
    if (err == ESP_ERR_INVALID_STATE) return send_text(req, "409 Conflict", "setup already in progress");
    if (err == ESP_ERR_INVALID_ARG) return send_text(req, "400 Bad Request", "SSID or password length is invalid");
    if (err != ESP_OK) return send_text(req, "500 Internal Server Error", "could not begin setup");
    return send_text(req, "202 Accepted", "connecting");
}

esp_err_t status_handler(httpd_req_t *req)
{
    NetworkSnapshot snapshot = {};
    network_manager_get_snapshot(&snapshot);
    char status[160], error[220];
    json_escape(status, sizeof(status), snapshot.status);
    json_escape(error, sizeof(error), snapshot.error);
    char json[768];
    snprintf(json, sizeof(json),
             "{\"state\":\"%s\",\"status\":\"%s\",\"error\":\"%s\",\"validating\":%s,\"scanning\":%s,\"can_cancel\":%s,\"paired\":%s,\"ip\":\"%s\",\"hostname\":\"%s\"}",
             network_state_name(snapshot.state), status, error, snapshot.validating ? "true" : "false",
             snapshot.scanning ? "true" : "false", snapshot.can_cancel ? "true" : "false",
             board_client_has_pairing_token() ? "true" : "false",
             snapshot.station_ip, snapshot.hostname);
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

esp_err_t cancel_handler(httpd_req_t *req)
{
    mark_activity();
    NetworkSnapshot snapshot = {};
    network_manager_get_snapshot(&snapshot);
    if (!snapshot.can_cancel) return send_text(req, "409 Conflict", "cancel is unavailable");
    if (network_manager_cancel_setup() != ESP_OK) return send_text(req, "500 Internal Server Error", "could not cancel setup");
    return send_text(req, "202 Accepted", "setup canceled");
}

esp_err_t captive_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, nullptr, 0);
}

esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t)
{
    return captive_handler(req);
}

void dns_task(void *)
{
    const int dns_socket = g_dns_socket;
    uint8_t request[512];
    while (true) {
        sockaddr_in source = {};
        socklen_t source_len = sizeof(source);
        const int length = recvfrom(dns_socket, request, sizeof(request), 0,
                                    reinterpret_cast<sockaddr *>(&source), &source_len);
        if (length < 12) {
            if (g_dns_socket != dns_socket) break;
            continue;
        }
        size_t question_end = 12;
        while (question_end < static_cast<size_t>(length) && request[question_end] != 0) {
            const uint8_t label_len = request[question_end];
            if (label_len > 63 || question_end + label_len + 1 >= static_cast<size_t>(length)) break;
            question_end += label_len + 1;
        }
        question_end += 5;  // zero terminator plus QTYPE/QCLASS
        if (question_end > static_cast<size_t>(length) || question_end + 16 > sizeof(request)) continue;
        request[2] = 0x81; request[3] = 0x80;
        request[6] = 0; request[7] = 1;
        request[8] = request[9] = request[10] = request[11] = 0;
        size_t out = question_end;
        const uint8_t answer[] = {0xC0,0x0C, 0x00,0x01, 0x00,0x01, 0,0,0,30, 0,4, 192,168,4,1};
        memcpy(request + out, answer, sizeof(answer));
        out += sizeof(answer);
        sendto(dns_socket, request, out, 0, reinterpret_cast<sockaddr *>(&source), source_len);
    }
    g_dns_task = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t start_dns()
{
    g_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_dns_socket < 0) return ESP_FAIL;
    int reuse = 1;
    setsockopt(g_dns_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(53);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_dns_socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        close(g_dns_socket); g_dns_socket = -1; return ESP_FAIL;
    }
    const BaseType_t ok = xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 5, &g_dns_task);
    if (ok != pdPASS) {
        close(g_dns_socket); g_dns_socket = -1; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t register_route(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t route = {.uri = uri, .method = method, .handler = handler, .user_ctx = nullptr};
    return httpd_register_uri_handler(g_server, &route);
}

}  // namespace

extern "C" esp_err_t provisioning_portal_start(void)
{
    if (g_server != nullptr) return ESP_OK;
    mark_activity();
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&g_server, &config);
    if (err != ESP_OK) return err;

    err = register_route("/", HTTP_GET, root_handler);
    if (err == ESP_OK) err = register_route("/api/networks", HTTP_GET, networks_handler);
    if (err == ESP_OK) err = register_route("/api/scan", HTTP_POST, scan_handler);
    if (err == ESP_OK) err = register_route("/api/configure", HTTP_POST, configure_handler);
    if (err == ESP_OK) err = register_route("/api/status", HTTP_GET, status_handler);
    if (err == ESP_OK) err = register_route("/api/cancel", HTTP_POST, cancel_handler);
    if (err == ESP_OK) err = register_route("/*", HTTP_GET, captive_handler);
    if (err == ESP_OK) err = httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND, not_found_handler);
    if (err != ESP_OK) {
        httpd_stop(g_server); g_server = nullptr; return err;
    }
    err = start_dns();
    if (err != ESP_OK) {
        httpd_stop(g_server); g_server = nullptr; return err;
    }
    ESP_LOGI(TAG, "captive portal listening at http://192.168.4.1");
    return ESP_OK;
}

extern "C" void provisioning_portal_stop(void)
{
    const TaskHandle_t dns_task_to_stop = g_dns_task;
    if (g_dns_socket >= 0) {
        const int socket_to_close = g_dns_socket;
        g_dns_socket = -1;
        shutdown(socket_to_close, SHUT_RDWR);
        close(socket_to_close);
        for (int i = 0; i < 20 && g_dns_task == dns_task_to_stop; ++i) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (dns_task_to_stop != nullptr && g_dns_task == dns_task_to_stop) {
            vTaskDelete(dns_task_to_stop);
            g_dns_task = nullptr;
        }
    }
    if (g_server != nullptr) {
        httpd_stop(g_server);
        g_server = nullptr;
    }
}

extern "C" bool provisioning_portal_is_active(void)
{
    return g_server != nullptr;
}

extern "C" int64_t provisioning_portal_last_activity_us(void)
{
    portENTER_CRITICAL(&g_activity_lock);
    const int64_t value = g_last_activity_us;
    portEXIT_CRITICAL(&g_activity_lock);
    return value;
}
